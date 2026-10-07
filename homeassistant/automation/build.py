"""Build the native, UI-editable wake automation; no runtime Python policy."""
from pathlib import Path
import json

HELPER = 'input_text.helio_wake_session'
OBS = 'sensor.helio_smart_wake_sleep_observations'
COMMANDS = 'sensor.helio_smart_wake_alarm_instructions'
ENABLED = 'switch.helio_smart_wake_smart_wake'

# Each complex expression relates dated observations and restored state; no native
# single-entity condition/helper expresses these relationships.
def expr(code): return '{{ ' + code + ' }}'
def check(code): return {'condition':'template', 'value_template':expr(code)}
def save_state(state='s'):
    return {'alias':'Persist the dated automation state', 'action':'input_text.set_value',
            'target':{'entity_id':HELPER}, 'data':{'value':expr("'V2:' ~ ("+state+" | to_json)")}}
def instruction(kind, epoch, next_state, follow=False, cancel=False):
    # Receipt context is caller-owned and durable BEFORE BLE writes; it repairs a
    # restart between accepting an instruction and updating the helper.
    return [
        {'alias':'Prepare the next dated state', 'variables':{'next_state':expr(next_state)}},
        {'alias': 'Cancel the bridge-owned alarm' if cancel else 'Ask the integration to save and verify the alarm',
         'action':'helio_smart_wake.cancel_alarm' if cancel else 'helio_smart_wake.set_alarm',
         'data': {**({} if cancel else {'when':expr(epoch)}),
                  'request_id':expr("s.n ~ '-"+kind+"-' ~ ("+epoch+") ~ '-' ~ manual"),
                  'context':{'routine_state':expr('next_state'), 'follow':follow, 'reason':kind}},
         'response_variable':'receipt'},
        {'variables':{'s':expr('dict(next_state, c=receipt.command_id)')}},
        save_state(),
    ]

def build():
    return {
        'id':'helio_smart_wake_routine_v2', 'alias':'Helio smart wake routine',
        'description':'Owns wake decisions: stable sleep onset + 8.5h target with awake compensation, fresh aligned Light/Awake agreement, 30s buffer and 5min reminders while worn. The integration supplies data and executes explicit verified commands. No fixed wake deadline.',
        'mode':'queued', 'max':10, 'max_exceeded':'silent',
        'triggers':[
            {'trigger':'state','entity_id':[OBS,COMMANDS,'binary_sensor.helio_smart_wake_bridge_connected',ENABLED,'number.helio_smart_wake_sleep_target','number.helio_smart_wake_wake_window']},
            {'trigger':'time_pattern','seconds':'/10'},
            {'trigger':'homeassistant','event':'start'},
            {'trigger':'time','at':'sensor.helio_smart_wake_verified_alarm','id':'saved_wake_time'},
        ],
        'conditions':[
            {'condition':'state','entity_id':'binary_sensor.helio_smart_wake_bridge_connected','state':'on'},
            {'condition':'state','entity_id':'sensor.helio_smart_wake_owner','state':'Home Assistant'},
        ],
        'actions':[
            {'alias':'Read current data when this queued run actually starts', 'variables':{
                'clock':expr('now().timestamp() | int'),
                # HA attributes include enum keys (friendly_name); normalize them
                # before native rendering, otherwise the mapping becomes a string.
                'd':expr("states['"+OBS+"'].attributes | default({}, true) | to_json | from_json"),
                'commands':expr("states['"+COMMANDS+"'].attributes | default({}, true) | to_json | from_json"),
                'old':expr("(states('"+HELPER+"')[3:] | from_json({})) if states('"+HELPER+"').startswith('V2:') else {}"),
            }},
            {'variables':{
                'night':expr('old.get("n",0) if old.get("p",0)>0 and not old.get("x",0) and clock < as_timestamp(as_local(as_datetime(old.p)).replace(hour=18,minute=0,second=0,microsecond=0) + timedelta(days=1 if as_local(as_datetime(old.p)).hour>=18 else 0)) else (d.get("night",0) | int)'),
                'manual':expr('commands.get("manual", 0) | int'),
                'verified':expr('commands.get("verified_epoch", 0) | int'),
                'last':expr('commands.get("last_instruction", {})'),
                'window':expr('states("number.helio_smart_wake_wake_window") | int(30) * 60'),
            }},
            {'variables':{'s':expr('old if old.get("n",0)==night else dict(n=night,p=0,a=0,e=0,x=0,m=manual,c=0,t=0,o=0,w=0)')}},
            {'alias':'Start a durable dated night before any alarm', 'if':[check('night>0 and old.get("n",0)!=night')], 'then':[save_state()]},
            {'alias':'Recover caller state from a durable accepted instruction', 'if':[
                check('last.get("context",{}).get("routine_state",{}).get("n",0)==night and last.get("context",{}).get("routine_state",{}).get("m",-1)==manual and last.get("command_id",0)>s.get("c",0)')],
             'then':[{'variables':{'s':expr('dict(last.context.routine_state, c=last.command_id)')}},save_state()]},
            {'alias':'Remember external manual control for this dated night', 'if':[check('s.m != manual')],
             'then':[{'variables':{'s':expr('dict(s, x=2, m=manual)')}},save_state()]},
            {'alias':'Re-enable before the original wake time, if only disabled', 'if':[
                {'condition':'state','entity_id':ENABLED,'state':'on'},check('s.x==4 and (not s.p or clock<s.p)')],
             'then':[{'variables':{'s':expr('dict(s, x=0, p=0, a=0, e=0)')}},save_state()]},
            {'variables':{
                'onset':expr('d.get("onset",0) if d.get("night",0)==night else s.get("o",0)'),
                'awake':expr('d.get("awake",0) if d.get("night",0)==night else (d.get("raw_awake",0) if d.get("band_valid",false) and d.get("complete",false) and not d.get("nap",true) and d.get("raw_onset",0)==s.get("o",0) and d.get("through",0)>=clock-180 and d.get("through",0)<=clock+60 else s.get("w",0))'),
            }},
            {'variables':{'target':expr('((((onset | int) + (states("number.helio_smart_wake_sleep_target") | float(8.5) * 3600) + (awake | int * 60) + 59) // 60) * 60) | int if onset else 0')}},
            {'alias':'Request minute reads near waking and during reminders', 'if':[
                {'condition':'state','entity_id':ENABLED,'state':'on'},
                check('not s.x and night>0 and clock<d.get("session_end",0) and ((s.p>0 and clock>=s.p) or (target>clock and target-clock<=[900,window] | max))')],
             'then':[{'action':'helio_smart_wake.refresh','data':{'frequent':True}}]},
            {'alias':'Wait for the current explicit instruction to finish', 'condition':check('not commands.get("pending",false)')['condition'],
             'value_template':expr('not commands.get("pending",false)')},
            {'alias':'Select a wake action from the current data', 'choose':[
                {'alias':'Smart wake disabled: cancel a future owned alarm', 'conditions':[{'condition':'state','entity_id':ENABLED,'state':'off'}],
                 'sequence':[{'if':[check('s.x!=4')], 'then':[{'variables':{'s':expr('dict(s,x=4)')}},save_state()]},
                             {'if':[check('verified>clock')], 'then':instruction('off','verified','dict(s, a=0, x=4)',cancel=True)}]},
                {'alias':'Manual control pauses this night', 'conditions':[check('s.x==2')], 'sequence':[{'stop':'Manual alarm control; wait for the next dated night'}]},
                {'alias':'A fresh removal after the first alarm stops reminders', 'conditions':[
                    check('s.p>0 and clock>=s.p and d.get("wear_state",0)==2 and d.get("wear_sample",0)>=s.p and d.get("wear_sample",0)<=clock and clock-d.get("wear_sample",0)<=180 and d.get("wear_read_at",0)<=clock and clock-d.get("wear_read_at",0)<=90')],
                 'sequence':[{'variables':{'s':expr('dict(s, x=1)')}},save_state(),
                             {'if':[check('verified>clock')], 'then':instruction('removed','verified','dict(s,a=0,x=1)',follow=True,cancel=True)}]},
                {'alias':'Continue cancellation while a stopped reminder remains', 'conditions':[check('s.x==1 and verified>clock')],
                 'sequence':instruction('removed','verified','dict(s,a=0,x=1)',follow=True,cancel=True)},
                {'alias':'Expired uncertain writes never arm a later alarm', 'conditions':[check('s.a>0 and s.a!=verified and s.a<=clock and not s.x')],
                 'sequence':[{'variables':{'s':expr('dict(s,x=3)')}},save_state()]},
                {'alias':'Stopped nights remain stopped', 'conditions':[check('s.x>0')], 'sequence':[{'stop':'This dated wake sequence has stopped'}]},
                {'alias':'Repeat five minutes later only with fresh worn data', 'conditions':[
                    check('s.p>0 and clock>=s.p and verified>0 and clock>=verified and s.a==verified and d.get("wear_state",0)==1 and d.get("wear_sample",0)>=verified and d.get("wear_sample",0)<=clock and clock-d.get("wear_sample",0)<=180 and d.get("wear_read_at",0)<=clock and clock-d.get("wear_read_at",0)<=90')],
                 'sequence':[{'variables':{'next_alarm':expr('((([verified+300,clock+30] | max)+59)//60)*60')}},
                             check('next_alarm<d.get("session_end",0)'),
                             *instruction('follow','next_alarm','dict(s,a=next_alarm)',follow=True)]},
                {'alias':'Wait after the first alarm if wear evidence is missing', 'conditions':[check('s.p>0 and clock>=s.p')], 'sequence':[{'stop':'Waiting for fresh worn or removed evidence'}]},
                {'alias':'Stop if the full sleep target passed without scheduling', 'conditions':[check('target>0 and clock>=target')],
                 'sequence':[{'variables':{'s':expr('dict(s,x=3)')}},save_state()]},
                {'alias':'Choose early waking when both sources report Light or Awake', 'conditions':[
                    check('not s.e and target>clock and window>0 and clock>=target-window and d.get("band_valid",false) and d.get("complete",false) and not d.get("nap",true) and d.get("raw_onset",0)==onset and d.get("raw_awake",-1)==awake and d.get("band_stage",0) in [4,7] and d.get("through",0)<=clock+60 and d.get("through",0)>=clock-180 and d.get("read_at",0)<=clock and d.get("read_at",0)>=clock-90 and d.get("model_valid",false) and d.get("model_stage",0) in [4,7] and d.get("model_sample",0)<=clock and d.get("model_sample",0)>=clock-180 and d.get("model_read_at",0)<=clock and d.get("model_read_at",0)>=clock-90 and ((d.get("model_sample",0)-d.get("through",0)) | abs)<=60 and ((clock+89)//60)*60<target and (not verified or verified>=clock+30)')],
                 'sequence':[{'variables':{'early_alarm':expr('((clock+89)//60)*60')}},
                             *instruction('early','early_alarm','dict(s,p=early_alarm,a=early_alarm,e=1,t=target,o=onset,w=awake)')]},
                {'alias':'Lock a selected early alarm, including an uncertain write', 'conditions':[check('s.e==1')],
                 'sequence':[{'stop':'Selected early alarm remains locked; retain the last verified alarm'}]},
                {'alias':'Save or update the full sleep-duration target', 'conditions':[
                    check('night>0 and target>=clock+30 and target<clock+24*3600-60 and target!=verified and (not verified or verified>=clock+30)')],
                 'sequence':instruction('target','target','dict(s,p=target,a=target,t=target,o=onset,w=awake)')},
            ]},
        ],
        'trace':{'stored_traces':20},
    }

if __name__ == '__main__':
    Path(__file__).with_name('helio-smart-wake.json').write_text(json.dumps(build(),indent=2)+'\n')
