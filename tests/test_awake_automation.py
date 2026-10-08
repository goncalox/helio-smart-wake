"""Exercise the generated get-up cue policy without a strap or HA writes."""
import json
import sys
from pathlib import Path
import test_automation_policy as harness

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'homeassistant/automation'))
import build_awake as cue

harness.CONFIG = cue.build()
harness.builder.HELPER = cue.HELPER

class Sim(harness.Sim):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.values[cue.HOURS] = '7.5'

    @property
    def state(self):
        return json.loads(self.values[cue.HELPER][len(cue.PREFIX):])

# OR is deliberate: neither Light nor agreement on Light counts as already Awake.
for band in (4, 5, 7, 8):
    for model in (4, 5, 7, 8):
        s = Sim(stage=band)
        s.data['model_stage'] = model
        s.run()
        assert bool(s.calls) == (band == 7 or model == 7), (band, model)
        if s.calls:
            assert s.calls[0][1]['when'] == s.now + 60
            assert not s.calls[0][1]['context']['follow']

# The threshold uses recorded duration, subtracts awake time, and includes equality.
for sleep_minutes, expected in ((449, False), (450, True), (451, True)):
    s = Sim(stage=7)
    onset = s.now - (sleep_minutes + 30) * 60
    s.data.update(onset=onset, raw_onset=onset, awake=30, raw_awake=30)
    s.run()
    assert bool(s.calls) == expected, sleep_minutes
s = Sim(stage=7)
s.values[cue.HOURS] = '8.5'
s.run()
assert not s.calls

# Invalid/old sleep data cannot invent completed sleep, even if the model says Awake.
for changes in ({'band_valid':False}, {'complete':False}, {'nap':True},
                {'onset':0}, {'raw_awake':5}, {'raw_onset':1},
                {'through':harness.stamp(8,8,56)}, {'through':harness.stamp(8,9,1)},
                {'read_at':harness.stamp(8,8,58)}, {'read_at':harness.stamp(8,9,1)}):
    s = Sim(stage=7)
    s.data.update(changes)
    s.run()
    assert not s.calls, changes

# A stale/invalid model is ignored, but cannot veto the strap's fresh Awake reading.
for changes in ({'model_valid':False}, {'model_sample':harness.stamp(8,8,56)},
                {'model_sample':harness.stamp(8,9,1)}, {'model_read_at':harness.stamp(8,8,58)},
                {'model_read_at':harness.stamp(8,9,1)}, {'model_sample':harness.stamp(8,8,57)}):
    for band in (4, 7):
        s = Sim(stage=band)
        s.data.update(model_stage=7, **changes)
        s.run()
        assert bool(s.calls) == (band == 7), (band, changes)
s = Sim(stage=4)
s.data.update(model_stage=7, model_sample=s.now-120)
s.run()
assert s.calls  # minute activity is dated by its start; sleep through is its end

# Keep 30 seconds' preparation time around the minute boundary.
for second, offset in ((0,60), (30,30), (31,89), (59,61)):
    s = Sim(harness.stamp(8,9,0,second), stage=7)
    s.run()
    assert s.calls[0][1]['when'] == s.now + offset

# Acceptance, not a passed clock, locks out another cue, including after a restart.
s = Sim(stage=7)
s.run()
s.verify()
s.values[cue.HELPER] = 'unknown'
s.run()
assert len(s.calls) == 1 and s.state['p'] == s.commands['verified_epoch']
s.fresh(s.now+600)
s.run()
assert len(s.calls) == 1  # no reminder even if still worn and Awake
s = Sim(stage=7)
s.run()
s.commands['pending'] = False
s.commands['last_instruction']['result'] = 'unverified'
s.fresh(s.now+120)
s.run()
assert len(s.calls) == 1  # uncertain cue never becomes a second vibration

# Manual edits pause the night; off cancels only this routine's own future cue.
s = Sim(stage=4)
s.run()
s.commands['manual'] = 1
s.data['band_stage'] = 7
s.run()
assert not s.calls and s.state['x'] == 2
s = Sim(stage=7)
s.run()
s.verify()
s.values[harness.builder.ENABLED] = 'off'
s.run()
assert s.calls[-1][0] == 'helio_smart_wake.cancel_alarm'
s.verify()
s.values[harness.builder.ENABLED] = 'on'
s.run()
assert len(s.calls) == 2
for change in ('offline', 'pending', 'off', 'foreign-owner'):
    s = Sim(stage=7)
    if change == 'offline': s.values['binary_sensor.helio_smart_wake_bridge_connected'] = 'off'
    elif change == 'pending': s.commands['pending'] = True
    elif change == 'off': s.values[harness.builder.ENABLED] = 'off'
    else: s.values['sensor.helio_smart_wake_owner'] = 'ESP32'
    s.run()
    assert not s.calls
print('Get-up cue: Awake OR, recorded 7h30 minus awake time, freshness, boundaries, restart, no duplicates/reminders, manual/off and transport guards passed')
