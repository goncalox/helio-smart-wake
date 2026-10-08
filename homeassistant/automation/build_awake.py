"""Native one-cue routine: confirm Awake after enough recorded sleep."""
from pathlib import Path
import json
from build import OBS, COMMANDS, ENABLED, expr, check

HELPER = 'input_text.helio_awake_cue_session'
HOURS = 'input_number.helio_ready_to_get_up_sleep_hours'
PREFIX = 'AC1:'
ROUTINE = 'helio_awake_after_rest_v1'

def save():
    return {'alias':'Save the dated cue state', 'action':'input_text.set_value',
            'target':{'entity_id':HELPER},'data':{'value':expr("'AC1:' ~ (s | to_json)")}}

def request(cancel=False):
    return [
        {'variables':{'next_state':expr('dict(s,x=4)' if cancel else 'dict(s,p=cue_time)')}},
        {'alias':'Cancel this routine’s future cue' if cancel else 'Save and verify one get-up cue on the strap',
         'action':'helio_smart_wake.cancel_alarm' if cancel else 'helio_smart_wake.set_alarm',
         'data':{**({} if cancel else {'when':expr('cue_time')}),
                 'request_id':expr("'awake-cue-' ~ s.n ~ '-"+('off' if cancel else 'awake')+"-' ~ "+('s.p' if cancel else 'cue_time')+" ~ '-' ~ manual"),
                 'context':{'routine':ROUTINE,'awake_cue_state':expr('next_state'),
                            'follow':False,'reason':'cue-off' if cancel else 'awake-after-rest'}},
         'response_variable':'receipt'},
        {'variables':{'s':expr('dict(next_state,c=receipt.command_id)')}},save(),
    ]

def build():
    return {
        'id':ROUTINE, 'alias':'Helio — Ready to get up',
        'description':(
            'Sends one gentle strap alarm only after you are already Awake and have at least the configured recorded sleep duration (7h30 by default), excluding recorded awake minutes.\n\n'
            + 'Requires fresh Awake from either the strap OR our model; Light alone never qualifies.\n\n'
            + 'Uses Home Assistant data changes and a once-per-minute clock check, requesting minute reads near the threshold.\n\n'
            'Asks the integration to save and verify a cue at least 30 seconds ahead, rounded up to a minute.\n\n'
            'There is no full-target alarm, deadline or repeating reminder; if qualifying Awake data never arrives, it stays silent.\n\n'
            'Saved state and instruction receipts prevent duplicate cues after restart; manual alarm control pauses the night, and switching smart wake off cancels this routine’s future cue.'),
        'mode':'queued','max':10,'max_exceeded':'silent','trace':{'stored_traces':20},
        'triggers':[
            {'trigger':'state','entity_id':[OBS,COMMANDS,'binary_sensor.helio_smart_wake_bridge_connected',ENABLED,HOURS]},
            {'trigger':'time_pattern','minutes':'/1','seconds':'0'},
            {'trigger':'homeassistant','event':'start'},
        ],
        'conditions':[
            {'condition':'state','entity_id':'binary_sensor.helio_smart_wake_bridge_connected','state':'on'},
            {'condition':'state','entity_id':'sensor.helio_smart_wake_owner','state':'Home Assistant'},
        ],
        'actions':[
            {'variables':{
                'clock':expr('now().timestamp() | int'),
                'd':expr("states['"+OBS+"'].attributes | default({},true) | to_json | from_json"),
                'commands':expr("states['"+COMMANDS+"'].attributes | default({},true) | to_json | from_json"),
                'old':expr("(states('"+HELPER+"')[4:] | from_json({})) if states('"+HELPER+"').startswith('AC1:') else {}"),
                'threshold':expr('states("'+HOURS+'") | float(7.5) * 3600'),
            }},
            {'variables':{'night':expr('d.get("night",0) | int'),'manual':expr('commands.get("manual",0) | int'),
                          'last':expr('commands.get("last_instruction",{})'),'verified':expr('commands.get("verified_epoch",0) | int')}},
            {'variables':{'s':expr('old if old.get("n",0)==night else dict(n=night,p=0,m=manual,c=0,x=0)')}},
            {'if':[check('night>0 and old.get("n",0)!=night')],'then':[save()]},
            {'alias':'Recover an accepted cue if the helper update was interrupted','if':[
                check('last.get("context",{}).get("routine","")=="'+ROUTINE+'" and last.get("context",{}).get("awake_cue_state",{}).get("n",0)==night and last.get("context",{}).get("awake_cue_state",{}).get("m",-1)==manual and last.get("command_id",0)>s.c')],
             'then':[{'variables':{'s':expr('dict(last.context.awake_cue_state,c=last.command_id)')}},save()]},
            {'if':[check('s.m!=manual')],'then':[{'variables':{'s':expr('dict(s,m=manual,x=2)')}},save()]},
            {'alias':'Stop or cancel when smart wake is switched off','choose':[
                {'conditions':[{'condition':'state','entity_id':ENABLED,'state':'off'}],
                 'sequence':[{'condition':'template','value_template':expr('not commands.get("pending",false)')},
                             {'if':[check('s.p>clock and verified==s.p and last.get("context",{}).get("routine","")=="'+ROUTINE+'"')],'then':request(cancel=True)},
                             {'stop':'Smart wake is off'}]},
            ]},
            {'condition':'template','value_template':expr('night>0 and not s.p and not s.x and not commands.get("pending",false)')},
            {'variables':{'onset':expr('d.get("onset",0) | int'),'awake':expr('d.get("awake",0) | int')}},
            {'alias':'Request faster reads near the recorded-sleep threshold','if':[
                check('onset>0 and clock<d.get("session_end",0) and clock-onset-awake*60>=threshold-900')],
             'then':[{'action':'helio_smart_wake.refresh','data':{'frequent':True}}]},
            {'alias':'Require enough complete recorded sleep and a fresh explicit Awake stage',
             'condition':'template','value_template':expr(
                 'onset>0 and onset>=night and threshold>=6*3600 and threshold<=10*3600 '
                 'and d.get("band_valid",false) and d.get("complete",false) and not d.get("nap",true) '
                 'and d.get("raw_onset",0)==onset and d.get("raw_awake",-1)==awake '
                 'and d.get("through",0)-onset-awake*60>=threshold '
                 
                 'and d.get("through",0)<=clock and d.get("through",0)>=clock-180 '
                 'and d.get("read_at",0)<=clock and d.get("read_at",0)>=clock-90 '
                 + 'and (d.get("band_stage",0)==7 or (d.get("model_valid",false) and d.get("model_stage",0)==7 '
                    'and d.get("model_sample",0)<=clock and d.get("model_sample",0)>=clock-180 '
                    'and d.get("model_read_at",0)<=clock and d.get("model_read_at",0)>=clock-90 '
                    'and ((d.get("model_sample",0)+60-d.get("through",0)) | abs)<=60)) '
                 + 'and (not verified or verified>=clock+30)')},
            {'variables':{'cue_time':expr('((clock+89)//60)*60')}},
            *request(),
        ],
    }

if __name__=='__main__':
    Path(__file__).with_name('helio-awake-after-rest.json').write_text(json.dumps(build(),indent=2)+'\n')
