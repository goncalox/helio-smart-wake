"""Fixed firmware/Python parity on synthetic records, without private overnight data."""
from pathlib import Path
import json
import math
import random
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, 'diagnostics')
from download import decode_blob
from test_stage_model import StageModel

build = Path('.test-build')
build.mkdir(exist_ok=True)
runner = build / 'stage_prediction_runner'
subprocess.run(['c++', '-std=c++17', '-Icomponents/helio_bridge',
                'tests/stage_prediction_runner.cpp', '-o', str(runner)], check=True)
frozen = json.loads(Path('diagnostics/deployed-model.json').read_text())
model = StageModel()
model.mean = frozen['mean']
model.scale = frozen['scale']
model.w = frozen['weights']
model.classes = frozen['classes']

rng = random.Random(20261003)
cases = []
for i in range(300):
    count = rng.randint(5, 30)
    rows = [bytes([120, rng.randint(0, 48), rng.randint(0, 5), rng.randint(45, 80), 0, 0, 0, 0])
            for _ in range(count)]
    raw = b''.join(rows)
    start = 1791030000 + i * 60
    now = start + count * 60 + 7
    tail = rows[-5:]
    hrs = [r[3] for r in tail]
    mean = sum(hrs) / 5
    features = [tail[-1][3], mean,
                math.sqrt(max(0, sum(h*h for h in hrs)/5 - mean*mean)),
                sum(r[1] for r in tail)/5, sum(r[2] for r in tail)]
    transformed = features[:3] + [math.log1p(v) for v in features[3:]]
    scores = [sum(w*z for w,z in zip(weights, model.transform(transformed))) for weights in model.w]
    prediction = model.classes[max(range(4), key=lambda k: scores[k])]
    cases.append((start, now, raw, features, scores, prediction))
inputs = ''.join(f'{start} {now} {raw.hex()}\n' for start, now, raw, *_ in cases)
outputs = subprocess.run([str(runner)], input=inputs, text=True, capture_output=True, check=True).stdout.splitlines()
assert len(outputs) == len(cases)
codes = {4:'Light', 5:'Deep', 8:'REM', 7:'Awake'}
for case, line in zip(cases, outputs):
    start, now, raw, features, scores, prediction = case
    fields = line.split()
    assert fields[0] == '1' and int(fields[2]) == start + (len(raw)//8 - 1)*60
    assert codes[int(fields[1])] == prediction
    assert all(math.isclose(float(v), e, rel_tol=1e-9, abs_tol=1e-9)
               for v,e in zip(fields[3:8], features))
    assert all(math.isclose(float(v), e, rel_tol=1e-9, abs_tol=1e-9)
               for v,e in zip(fields[8:], scores))

# Check the exported model header matches the frozen JSON exactly.
exported = build / 'exported-stage-model.h'
subprocess.run([sys.executable, 'diagnostics/export_stage_model.py', '--output', str(exported)], check=True)
assert exported.read_bytes() == Path('components/helio_bridge/stage_model.h').read_bytes()

payload = bytes([1,1,4,0]) + struct.pack('<I9f',1000,52,51,1,0,0,1,0,0,-1)
gate = bytes([1,4,4,31]) + struct.pack('<5I',1060,1000,1067,2000,1187)
raw = struct.pack('<BIIH',11,1067,123,44) + payload + struct.pack('<BIIH',12,1067,123,24) + gate
packed = bytearray()
for b in raw:
    packed.extend(bytes([b]) if b else bytes([0,1]))
blob = struct.pack('<4sIII',b'HLG2',77,len(raw),zlib.crc32(raw)) + packed
decoded = decode_blob(blob,77)
assert decoded[0]['stage']=='Light' and decoded[0]['features']['heart_rate']==52
assert decoded[1]['earlier_alarm_selected'] and decoded[1]['model_light_ready']
print('300 synthetic Python/C++ feature/score/class parity checks, frozen export and log decoding passed.')

# Version 2 gate records distinguish Light-or-Awake policy from legacy Light-only records.
gate2=bytes([2,7,4,31])+struct.pack('<5I',1060,1000,1067,2000,1187)
raw2=struct.pack('<BIIH',12,1067,123,24)+gate2
packed2=bytearray()
for value in raw2: packed2.extend(bytes([value]) if value else bytes([0,1]))
blob2=struct.pack('<4sIII',b'HLG2',78,len(raw2),zlib.crc32(raw2))+packed2
new=decode_blob(blob2,78)[0]
assert new['strap_stage']=='Awake' and new['model_stage']=='Light'
assert new['model_wake_ready'] and new['gate_policy']=='both-Light-or-Awake'
assert 'model_light_ready' not in new
assert decoded[1]['gate_policy']=='both-Light' and decoded[1]['model_light_ready']
print('Legacy and Light-or-Awake gate policies decode distinctly.')

wear=bytes([1,2,115,0])+struct.pack('<2I',1100,1160)
follow=struct.pack('<5I4B',1,500,1000,1300,1300,0,0,0,0)
raw3=struct.pack('<BIIH',13,1160,123,12)+wear+struct.pack('<BIIH',14,1160,123,24)+follow
packed3=bytearray()
for value in raw3: packed3.extend(bytes([value]) if value else bytes([0,1]))
blob3=struct.pack('<4sIII',b'HLG2',79,len(raw3),zlib.crc32(raw3))+packed3
new3=decode_blob(blob3,79)
assert new3[0]['state']=='Removed' and new3[0]['sample_time']==1100
assert new3[1]['follow_up']['confirmed']==1300 and not new3[1]['follow_up']['uncertain']
print('Wear observations and persisted follow-up records decode correctly.')
