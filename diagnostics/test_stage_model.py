"""Retrospective, purged block testing of an experimental HR/activity classifier.

Inputs are causal summaries available at each read. Training occurs offline after
labels arrive: this is not a simulation of fitting a live model during that night.
One-night agreement with revised strap labels is not independent staging accuracy.
"""
import argparse
from collections import Counter
from datetime import datetime
import json
import math
from pathlib import Path
import statistics
from zoneinfo import ZoneInfo
from prepare_shadow import prepare

CLASSES = ('Light', 'Deep', 'REM', 'Awake')
FEATURE_NAMES = ('heart_rate', 'hr_mean_5m', 'hr_sd_5m', 'intensity_mean_5m', 'steps_5m')
PURGE_SECONDS = 10 * 60
FOLDS = 5
ITERATIONS = 1200
LEARNING_RATE = .08
REGULARIZATION = .2


def features(row):
    f = row['features']
    return [float(f[n]) if i < 3 else math.log1p(float(f[n]))
            for i, n in enumerate(FEATURE_NAMES)]


class StageModel:
    """Fixed class-balanced, L2-regularized multinomial logistic regression."""
    def fit(self, x, y):
        self.classes = [c for c in CLASSES if c in y]
        if not x or len(self.classes) < 2:
            raise ValueError('need at least two classes in training')
        self.mean = [statistics.mean(a[j] for a in x) for j in range(len(x[0]))]
        self.scale = [max(statistics.pstdev(a[j] for a in x), 1e-6) for j in range(len(x[0]))]
        z = [self.transform(a) for a in x]
        counts = Counter(y)
        weights = [len(y)/(len(counts)*counts[c]) for c in y]
        self.w = [[0.] * len(z[0]) for _ in self.classes]
        lookup = {c:i for i,c in enumerate(self.classes)}
        for _ in range(ITERATIONS):
            gradients = [[0.] * len(z[0]) for _ in self.classes]
            for a, label, weight in zip(z, y, weights):
                probabilities = self.probabilities_z(a)
                for k, prob in enumerate(probabilities):
                    delta = weight * (prob - (lookup[label] == k)) / len(z)
                    for j, value in enumerate(a):
                        gradients[k][j] += delta * value
            for k in range(len(self.classes)):
                for j in range(len(z[0])):
                    penalty = REGULARIZATION * self.w[k][j] if j else 0.
                    self.w[k][j] -= LEARNING_RATE * (gradients[k][j] + penalty)
        return self

    def transform(self, x):
        return [1.] + [(v-m)/s for v,m,s in zip(x, self.mean, self.scale)]

    def probabilities_z(self, z):
        scores = [sum(a*b for a,b in zip(w,z)) for w in self.w]
        highest = max(scores)
        exps = [math.exp(s-highest) for s in scores]
        total = sum(exps)
        return [v/total for v in exps]

    def predict(self, x):
        values = self.probabilities_z(self.transform(x))
        probabilities = dict(zip(self.classes, values))
        # Scores are not calibrated probabilities of physiological sleep stages.
        return self.classes[max(range(len(values)), key=values.__getitem__)], probabilities

    def export(self):
        return dict(model='class-balanced multinomial logistic regression', classes=self.classes,
                    feature_names=FEATURE_NAMES, movement_transform='log1p', mean=self.mean,
                    scale=self.scale, weights=self.w, regularization=REGULARIZATION,
                    iterations=ITERATIONS, learning_rate=LEARNING_RATE,
                    calibrated=False, alarm_control=False)


def metrics(truth, predictions):
    confusion = {a:{b:0 for b in CLASSES} for a in CLASSES}
    for a,b in zip(truth,predictions): confusion[a][b] += 1
    per_class = {}
    for c in CLASSES:
        support = sum(confusion[c].values())
        predicted = sum(confusion[a][c] for a in CLASSES)
        correct = confusion[c][c]
        precision = correct/predicted if predicted else 0.
        recall = correct/support if support else None
        f1 = 2*precision*recall/(precision+recall) if recall and precision+recall else 0.
        per_class[c] = dict(support=support,predictions=predicted,precision=precision,recall=recall,f1=f1)
    recalls = [v['recall'] for v in per_class.values() if v['recall'] is not None]
    labelled_sleep = [c for c in ('Light','Deep','REM') if per_class[c]['support']]
    light_predictions = sum(p=='Light' for p in predictions)
    return dict(observations=len(truth), agreement=sum(a==b for a,b in zip(truth,predictions))/len(truth),
                balanced_agreement=statistics.mean(recalls),
                sleep_stages_balanced_agreement=statistics.mean(per_class[c]['recall'] for c in labelled_sleep),
                macro_f1=statistics.mean(v['f1'] for v in per_class.values() if v['support']),
                light_predictions=light_predictions,
                deep_or_rem_labelled_light=sum(a in ('Deep','REM') and b=='Light' for a,b in zip(truth,predictions)),
                per_class=per_class, confusion=confusion)


def dataset(events, night, timezone):
    # Replace whole records, rather than retain labels from trimmed old segments.
    latest = {}
    for e in sorted(events,key=lambda e:(e['device_time'],e.get('batch',0),e.get('uptime_ms',0))):
        if e['kind']=='snapshot':
            for record in e['records']:
                if record.get('night'): latest[record['base']] = (record,e['device_time'])
    selected = [(r,at) for r,at in latest.values()
                if datetime.fromtimestamp(r['onset'],timezone).date().isoformat()==night]
    if len(selected)!=1: raise ValueError('need exactly one latest reference night')
    reference, at = selected[0]
    onset = reference['onset']; end = max(s['end'] for s in reference['night'])
    labels = {t:s['stage'] for s in reference['night'] if s['stage'] in CLASSES
              for t in range(max(s['start'],onset),s['end'],60)}
    rows = []
    for row in prepare(events):
        t=row['sample_time']//60*60
        if not row['feature_ready'] or not onset<=t<end or t not in labels: continue
        if not all(row['features'][n] is not None for n in FEATURE_NAMES): continue
        row = dict(row, later_zepp_stage=labels[t], reference_observed_at=at)
        rows.append(row)
    return rows, dict(onset=onset,end=end,reference_observed_at=at)


def holdouts(rows):
    """Contiguous test blocks, with both sides purged to separate feature windows."""
    if len(rows)<25: raise ValueError('not enough usable observations for the fixed block test')
    for fold in range(FOLDS):
        start = len(rows)*fold//FOLDS; stop = len(rows)*(fold+1)//FOLDS
        indices = list(range(start,stop))
        lo = rows[start]['sample_time']; hi = rows[stop-1]['sample_time']
        train = [i for i,r in enumerate(rows)
                 if r['sample_time'] < lo-PURGE_SECONDS or r['sample_time'] > hi+PURGE_SECONDS]
        if len(train)<10: raise ValueError('insufficient training after purging')
        yield fold,train,indices


def run(events, night, timezone):
    rows, interval = dataset(events,night,timezone)
    x = [features(r) for r in rows]; y = [r['later_zepp_stage'] for r in rows]
    predictions = [None]*len(rows); majority = [None]*len(rows); folds = []
    for fold,train,test in holdouts(rows):
        model=StageModel().fit([x[i] for i in train],[y[i] for i in train])
        common=Counter(y[i] for i in train).most_common(1)[0][0]
        for i in test:
            p,scores=model.predict(x[i]);predictions[i]=p;majority[i]=common
            rows[i]=dict(rows[i],fold=fold,prediction=p,model_scores=scores,majority_prediction=common)
        folds.append(dict(fold=fold,training_observations=len(train),test_observations=len(test),
                          heldout_start=rows[test[0]]['sample_time'],heldout_end=rows[test[-1]]['sample_time'],
                          training_classes=dict(Counter(y[i] for i in train)),
                          model=metrics([y[i] for i in test],[predictions[i] for i in test]),
                          majority=metrics([y[i] for i in test],[majority[i] for i in test])))
    assert all(predictions)
    summary=dict(night=night,timezone=str(timezone),interval=interval,observations=len(rows),
                 label_counts=dict(Counter(y)),prediction_counts=dict(Counter(predictions)),
                 protocol='Retrospective five contiguous held-out blocks within one night; ten-minute purge on both sides',
                 feature_timing='Only readings available by the decision timestamp; trailing five measured minutes',
                 training_timing='Offline after morning labels; training can use other later blocks of this night',
                 model=metrics(y,predictions),always_light=metrics(y,['Light']*len(y)),
                 train_majority=metrics(y,majority),folds=folds,
                 limitations=['One night, correlated observations; no held-out night',
                              'Revised strap labels are not independent physiological ground truth',
                              'Awake has very little support; held-out classes absent from training cannot be predicted',
                              'Fixed exploratory baseline, with no tuned hyperparameters or calibrated confidence',
                              'This test predicts the newest available sample, typically about one minute old, not an unobserved current stage'],
                 alarm_control=False)
    # A full-data fit is an experimental artifact for later shadow testing only.
    fitted=StageModel().fit(x,y).export()
    fitted.update(training_night=night,training_observations=len(rows),validation='one-night block comparison only')
    return summary,rows,fitted


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('events',type=Path)
    parser.add_argument('--night',required=True)
    parser.add_argument('--timezone',default='Europe/Lisbon')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    events=[json.loads(s) for s in args.events.read_text().splitlines() if s]
    summary,rows,fitted=run(events,args.night,ZoneInfo(args.timezone))
    args.output.mkdir(parents=True,exist_ok=True)
    (args.output/'summary.json').write_text(json.dumps(summary,indent=2))
    (args.output/'predictions.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
    (args.output/'experimental-model.json').write_text(json.dumps(fitted,indent=2))
    print(json.dumps({k:summary[k] for k in ('observations','label_counts','prediction_counts')}))
    for key in ('model','always_light','train_majority'):
        m=summary[key];print(key,json.dumps({k:m[k] for k in ('agreement','balanced_agreement','sleep_stages_balanced_agreement','light_predictions','deep_or_rem_labelled_light')}))

if __name__=='__main__':main()
