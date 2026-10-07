"""Run the actual native automation definitions against synthetic nights, no devices."""
import copy,importlib.util,json,types
from datetime import datetime,timedelta,timezone
from zoneinfo import ZoneInfo
from pathlib import Path
from jinja2.nativetypes import NativeEnvironment
from jinja2 import StrictUndefined
root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('wake_automation',root/'homeassistant/automation/build.py')
builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
CONFIG=builder.build();LISBON=ZoneInfo('Europe/Lisbon')
def stamp(day,hour,minute=0,second=0):return int(datetime(2026,10,day,hour,minute,second,tzinfo=LISBON).timestamp())
class Stop(Exception):pass
class States:
    def __init__(self,sim):self.sim=sim
    def __call__(self,name):return self.sim.values.get(name,'unknown')
    def __getitem__(self,name):return types.SimpleNamespace(attributes=self.sim.attributes.get(name,{}))
class Sim:
    def __init__(self,now=None,stage=5):
        self.now=stamp(8,9) if now is None else now;self.calls=[];self.counter=0
        self.values={builder.HELPER:'unknown',builder.ENABLED:'on','number.helio_smart_wake_sleep_target':'8.5','number.helio_smart_wake_wake_window':'30',
                     'binary_sensor.helio_smart_wake_bridge_connected':'on','sensor.helio_smart_wake_owner':'Home Assistant'}
        self.data={'night':stamp(7,18),'session_end':stamp(8,18),'onset':self.now-8*3600,'awake':0,'raw_onset':self.now-8*3600,'raw_awake':0,'band_valid':True,
                   'complete':True,'nap':False,'band_stage':stage,'through':self.now,'read_at':self.now,'model_valid':True,'model_stage':stage,'model_sample':self.now,'model_read_at':self.now,
                   'wear_state':0,'wear_sample':self.now,'wear_read_at':self.now}
        self.commands={'manual':0,'pending':False,'verified_epoch':0,'last_instruction':{}}
        self.attributes={builder.OBS:self.data,builder.COMMANDS:self.commands}
        self.env=NativeEnvironment(undefined=StrictUndefined)
        self.env.filters['to_json']=json.dumps
        self.env.filters['from_json']=lambda s,default=None:json.loads(s) if s and s!='unknown' else default
        self.env.globals.update(states=States(self),now=lambda:datetime.fromtimestamp(self.now,LISBON),as_datetime=lambda t:datetime.fromtimestamp(t,timezone.utc),
                                as_local=lambda d:d.astimezone(LISBON),as_timestamp=lambda d:d.timestamp(),timedelta=timedelta)
    def render(self,x,variables):
        if isinstance(x,str) and ('{{' in x or '{%' in x):return self.env.from_string(x).render(**variables)
        if isinstance(x,dict):return {k:self.render(v,variables) for k,v in x.items()}
        if isinstance(x,list):return [self.render(v,variables) for v in x]
        return x
    def condition(self,c,v):
        if c['condition']=='template':return bool(self.render(c['value_template'],v))
        if c['condition']=='state':return self.values[c['entity_id']]==c['state']
        raise AssertionError(c)
    def sequence(self,steps,v):
        for step in steps:
            if 'variables' in step:
                for key,value in step['variables'].items():v[key]=self.render(value,v)
            elif 'if' in step:
                branch='then' if all(self.condition(c,v) for c in step['if']) else 'else'
                self.sequence(step.get(branch,[]),v)
            elif 'choose' in step:
                for choice in step['choose']:
                    if all(self.condition(c,v) for c in choice['conditions']):
                        self.sequence(choice['sequence'],v);break
            elif 'condition' in step:
                if not self.condition(step,v):raise Stop()
            elif 'stop' in step:raise Stop()
            elif 'action' in step:
                action=step['action'];data=self.render(step.get('data',{}),v)
                if action=='input_text.set_value':
                    assert isinstance(data['value'],str) and len(data['value'])<=255
                    self.values[builder.HELPER]=data['value']
                elif action=='helio_smart_wake.refresh':pass
                elif action in ('helio_smart_wake.set_alarm','helio_smart_wake.cancel_alarm'):
                    self.counter+=1;self.calls.append((action,data));v[step['response_variable']]={'command_id':self.counter,'result':'pending'}
                    self.commands['pending']=True
                    self.commands['last_instruction']={'command_id':self.counter,'context':copy.deepcopy(data['context']),'epoch':data.get('when',0),'result':'pending'}
                else:raise AssertionError(action)
            else:raise AssertionError(step)
    def run(self):
        if all(self.condition(c,{}) for c in CONFIG['conditions']):
            try:self.sequence(CONFIG['actions'],{})
            except Stop:pass
        return self.calls
    def verify(self):
        self.commands['pending']=False;self.commands['verified_epoch']=self.commands['last_instruction']['epoch'];self.commands['last_instruction']['result']='verified'
    @property
    def state(self):return json.loads(self.values[builder.HELPER][3:])
    def fresh(self,now,wear=1):
        self.now=now;self.data.update(through=now,read_at=now,model_sample=now,model_read_at=now,wear_state=wear,wear_sample=now-60,wear_read_at=now)

# Full target retains duration + awake time; no fixed 10:00 deadline.
s=Sim(stamp(8,10));s.data.update(onset=stamp(8,2,30),raw_onset=stamp(8,2,30),awake=20,raw_awake=20);s.run()
assert s.calls[-1][1]['when']==stamp(8,11,20)
for band in (4,7):
    for model in (4,7):
        s=Sim(stage=band);s.data['model_stage']=model;s.run();assert s.calls[-1][1]['when']==stamp(8,9,1) and s.state['e']==1
for changes in ({'model_stage':5},{'band_stage':8},{'model_valid':False},{'band_valid':False},{'complete':False},{'nap':True},
                {'model_sample':stamp(8,8,55)},{'model_sample':stamp(8,9,1)},{'model_read_at':stamp(8,8,58)},
                {'read_at':stamp(8,8,58)},{'through':stamp(8,8,55)},{'raw_awake':5}):
    s=Sim(stage=4);s.data.update(changes);s.run();assert s.calls[-1][1]['when']==stamp(8,9,30),changes
s=Sim(stage=4);s.data['model_sample']=s.now-120;s.run();assert s.calls[-1][1]['when']==stamp(8,9,30)
s=Sim();s.data.update(onset=0,raw_onset=0);s.run();assert not s.calls and s.state['n']==s.data['night']
# No action while disconnected or transport has an unfinished accepted instruction.
s=Sim(stage=4);s.values['binary_sensor.helio_smart_wake_bridge_connected']='off';s.run();assert not s.calls
s=Sim(stage=4);s.commands['pending']=True;s.run();assert not s.calls
# A lost helper update is repaired from the receipt, never a different early time.
s=Sim(stage=4);s.run();s.verify();s.values[builder.HELPER]='unknown';s.fresh(stamp(8,9,0,20));s.run();assert len(s.calls)==1 and s.state['e']==1
# Full target and early time can change only until an early instruction is selected.
s=Sim();s.run();s.verify();s.fresh(stamp(8,9,5));s.data.update(band_stage=4,model_stage=4);s.run();assert s.calls[-1][1]['when']==stamp(8,9,6)
s.verify();s.fresh(stamp(8,9,5,10));s.run();assert len(s.calls)==2
# Reminders use actual verified times; a fresh removal cancels only a future reminder.
s.fresh(stamp(8,9,7));s.run();assert s.calls[-1][1]['when']==stamp(8,9,11) and s.calls[-1][1]['context']['follow']
s.verify();s.fresh(stamp(8,9,8),2);s.run();assert s.calls[-1][0]=='helio_smart_wake.cancel_alarm' and s.state['x']==1
s.verify();s.fresh(stamp(8,9,12),1);s.run();assert len(s.calls)==4
for wear in (0,):
    s=Sim();s.run();s.verify();s.fresh(stamp(8,9,32),wear);s.run();assert len(s.calls)==1
# Manual edits pause a dated night even before the first automatic target exists.
s=Sim();s.data['onset']=0;s.run();s.commands['manual']=1;s.run();assert s.state['x']==2 and not s.calls
s=Sim(stage=4);s.run();s.commands['pending']=False;s.commands['last_instruction']['result']='unverified';s.fresh(stamp(8,9,2));s.run();assert s.state['x']==3 and len(s.calls)==1
# Off cancels a future alarm, and can be re-enabled safely before that target.
s=Sim();s.run();s.verify();s.values[builder.ENABLED]='off';s.run();assert s.calls[-1][0]=='helio_smart_wake.cancel_alarm'
s.verify();s.values[builder.ENABLED]='on';s.run();assert s.calls[-1][0]=='helio_smart_wake.set_alarm'
# A target crossing 18:00 remains dated to its existing night, with no wake cap.
s=Sim(stamp(8,17,50));s.data.update(onset=stamp(8,10),raw_onset=stamp(8,10));s.run();assert s.calls[-1][1]['when']==stamp(8,18,30)
s.verify();s.fresh(stamp(8,18,31));s.data.update(night=stamp(8,18),session_end=stamp(9,18),onset=0);s.run();assert s.calls[-1][1]['when']==stamp(8,18,35)
print('Native automation rules: duration/awake compensation, dual gate, freshness/alignment, no deadline, restart/receipt recovery, pending/uncertainty, manual/off, locked early time, five-minute follow-ups and removal passed')
