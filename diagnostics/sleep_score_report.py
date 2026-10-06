"""Summarize already downloaded score audits; no network, labels, fitting or writes."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path


def summarize(events):
    nights = {}
    for event in events:
        if event.get('kind') != 'personal_sleep_score':
            continue
        score = event['score']
        key = (score['version'], score['onset'])
        # Downloads normally arrive chronologically, but reject duplicate/older observations.
        old = nights.get(key)
        if old is None or score['observed'] >= old['observed']:
            nights[key] = score
    return {
        'interpretation': 'Experimental overnight index; comparison does not establish which score measures quality better.',
        'nights': [dict(
            version=s['version'], onset_utc=datetime.fromtimestamp(s['onset'], timezone.utc).isoformat(),
            score=round(s['score'], 1), helio=s['helio_score'],
            first_score=round(s['first_score'], 1), first_helio=s['first_helio_score'],
            component_coverage=s['component_coverage'], overnight_reading_percent=round(s['activity_coverage'] * 100, 1),
            settled=s['settled'], asleep_minutes=s['asleep_minutes'], awake_minutes=s['awake_minutes'],
            awakening_bouts=s['awakening_bouts'], prior_baseline_nights=s['baseline_nights'],
            available_components=s['available_components'],
            longest_awake_minutes=s.get('longest_awake_minutes'), wake_cluster_30m=s.get('wake_cluster_30m'),
            hr_minute_change=s.get('hr_minute_change'), hr_late_minus_early=s.get('hr_late_minus_early'),
            movement_bursts=s.get('movement_bursts'), longest_movement_burst=s.get('longest_movement_burst'),
            recent_history_nights=s.get('recent_history_nights'), recent_shortfall_fraction=s.get('recent_shortfall_fraction'),
            previous_version=s.get('previous_version'), previous_score=s.get('previous_score'),
            previous_first_score=s.get('previous_first_score'), previous_component_coverage=s.get('previous_component_coverage'),
        ) for _, s in sorted(nights.items(), key=lambda x: (x[0][1], x[0][0]))]
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('events', type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(json.loads(line) for line in args.events.read_text().splitlines() if line.strip()), indent=2))
