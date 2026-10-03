import sys, struct
sys.path.insert(0,'diagnostics')
from prepare_shadow import prepare
from activity_decode import decode_activity
p=bytes([1])+struct.pack('<II',60,60)+bytes(8)+bytes([120,2,0,255,0,0,0,0])
a=decode_activity(p)
assert a['rows'][0]['heart_rate'] is None and a['rows'][0]['timestamp']==60
rows=[dict(timestamp=t,kind=120,intensity=1,steps=0,heart_rate=60,raw_hex='') for t in range(60,361,60)]
events=[dict(kind='activity',device_time=310,rows=rows),
        dict(kind='snapshot',device_time=600,records=[dict(night=[dict(start=300,end=360,stage='REM')])])]
a=prepare(events)[0]
assert a['sample_time']==300 and a['decision_time']==310 and a['feature_ready']
assert a['later_zepp_stage']=='REM' and not a['prediction']
b=prepare(events[:1])[0]
assert a['features']==b['features'] # Future labels cannot change features.
rows[0]['kind']=122; rows[0]['raw_hex']='ffffffffffffffff'
assert prepare(events)[0]['features']==a['features'] # Stage bytes cannot be inputs.
events[0]['device_time']=1000
assert not prepare(events)[0]['feature_ready'] and prepare(events)[0]['later_zepp_stage'] is None
print('Causal arrival timing, missing HR, future-label isolation and freshness checks passed')
