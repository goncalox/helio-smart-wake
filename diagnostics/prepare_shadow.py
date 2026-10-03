"""Build causal feature rows and later reference labels; never controls alarms.

No classifier is fitted here: one person's revised Zepp labels are not independent
sleep-study ground truth, and current-day data is not a held-out overnight test.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import statistics


def prepare(events):
    events = sorted(events, key=lambda e: (e['device_time'], e.get('batch', 0), e.get('uptime_ms', 0)))
    # These are evaluation labels only and never enter the feature calculation.
    reference = {}
    for event in events:
        if event['kind'] != 'snapshot': continue
        for record in event['records']:
            for segment in record.get('night', []):
                if segment['stage'] not in ('Light', 'Deep', 'REM', 'Awake'): continue
                for minute in range(segment['start'], segment['end'], 60):
                    reference[minute] = (segment['stage'], event['device_time'])
    available = {}; output = []
    for event in events:
        if event['kind'] != 'activity': continue
        arrived = event['device_time']
        for row in event['rows']:
            if row['timestamp'] <= arrived:
                available[row['timestamp']] = row
        # Never backdate a prediction to when a delayed sample was measured.
        latest = max((t for t in available if t <= arrived), default=0)
        if not latest: continue
        window = [available[t] for t in sorted(available) if latest-240 <= t <= latest]
        # kind can encode Zepp's own sleep stage; bytes 4–7 may also contain stages.
        # Neither is an input, and movement intensity is an undocumented summary.
        valid_hr = [r['heart_rate'] for r in window if r['heart_rate'] is not None]
        hr = valid_hr[-1] if valid_hr else None
        features = dict(heart_rate=hr, hr_mean_5m=statistics.mean(valid_hr) if valid_hr else None,
                        hr_sd_5m=statistics.pstdev(valid_hr) if len(valid_hr)>1 else None,
                        hr_samples_5m=len(valid_hr), minute_samples_5m=len(window),
                        steps_5m=sum(r['steps'] for r in window),
                        intensity_mean_5m=statistics.mean(r['intensity'] for r in window))
        stage, label_at = reference.get(latest//60*60, (None, None))
        if label_at is not None and label_at < arrived: stage, label_at = None, None
        output.append(dict(decision_time=arrived, sample_time=latest, age_seconds=arrived-latest,
                           feature_ready=arrived-latest <= 180 and len(window)==5 and len(valid_hr)>=3,
                           features=features, later_zepp_stage=stage, reference_observed_at=label_at,
                           prediction=None))
        available = {t:r for t,r in available.items() if t >= arrived-3600}
    return output


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('events', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args=parser.parse_args()
    rows=prepare([json.loads(s) for s in args.events.read_text().splitlines() if s])
    args.output.write_text(''.join(json.dumps(r)+'\n' for r in rows))
    ages=[r['age_seconds'] for r in rows]
    print(json.dumps(dict(observations=len(rows),feature_ready=sum(r['feature_ready'] for r in rows),
                          median_age_seconds=statistics.median(ages) if ages else None,
                          classifier='not trained', alarm_control=False)))

if __name__=='__main__': main()
