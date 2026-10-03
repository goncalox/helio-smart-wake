import sys
from pathlib import Path
from zoneinfo import ZoneInfo
sys.path.insert(0,'diagnostics')
from test_stage_model import StageModel, holdouts, metrics, features, dataset
# Synthetic separable classes verify training/prediction instead of implementation details.
x=[[-3+i*.02,0] for i in range(20)]+[[3+i*.02,0] for i in range(20)]
y=['Light']*20+['Deep']*20
model=StageModel().fit(x,y)
assert model.predict([-3,0])[0]=='Light' and model.predict([3,0])[0]=='Deep'
assert model.scale[1]>0 and abs(sum(model.predict([0,0])[1].values())-1)<1e-10
# Every observation is tested once; adjacent/overlapping feature windows are purged.
rows=[dict(sample_time=i*300) for i in range(50)]
tested=[]
for _,train,test in holdouts(rows):
 tested+=test
 lo=rows[test[0]]['sample_time'];hi=rows[test[-1]]['sample_time']
 assert not set(train)&set(test)
 assert all(rows[i]['sample_time']<lo-600 or rows[i]['sample_time']>hi+600 for i in train)
assert sorted(tested)==list(range(50))
m=metrics(['Light','Deep','REM','Awake'],['Light']*4)
assert m['agreement']==.25 and m['balanced_agreement']==.25 and m['deep_or_rem_labelled_light']==2
# New labels/strap metadata are never independent feature inputs.
r=dict(features=dict(heart_rate=53,hr_mean_5m=52,hr_sd_5m=1,intensity_mean_5m=0,steps_5m=0),later_zepp_stage='Light')
a=features(r);r['later_zepp_stage']='REM';r['kind']=122;r['raw_hex']='ffffffffffffffff'
assert features(r)==a
# Replacing a trimmed reference record removes the older extra label rather than retaining it.
base=1790985600;start=base+180*60
record=dict(base=base,onset=start,night=[dict(start=start,end=start+600,stage='Light')])
trimmed=dict(base=base,onset=start+60,night=[dict(start=start+60,end=start+600,stage='Deep')])
activity=[]
for i in range(10):activity.append(dict(timestamp=start+i*60,heart_rate=52,intensity=0,steps=0,kind=120))
events=[dict(kind='snapshot',device_time=start+700,records=[record]),dict(kind='activity',device_time=start+570,rows=activity),dict(kind='snapshot',device_time=start+900,records=[trimmed])]
d,interval=dataset(events,'2026-10-03',ZoneInfo('Europe/Lisbon'))
assert interval['onset']==start+60 and d[0]['later_zepp_stage']=='Deep'
print('Classifier separation, constant features, purged independent test blocks, baseline metrics, label exclusion and revised-reference checks passed')
