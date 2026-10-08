"""Run host regression checks without contacting the ESP32 or strap."""
from pathlib import Path
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
BUILD = ROOT / '.test-build'
BUILD.mkdir(exist_ok=True)
COMPONENT = ROOT / 'components/helio_bridge'

def run(*args):
    subprocess.run(list(args), check=True)

for name in ('protocol', 'alarms', 'sleep', 'activity', 'adaptive', 'read_health', 'sleep_score', 'remote'):
    output = BUILD / name
    run('c++', '-std=c++17', '-I' + str(COMPONENT),
        str(ROOT / f'tests/test_{name}.cpp'), '-o', str(output))
    run(str(output))

# Exercise actual HA command transport and owned-alarm readback with BLE/NVS mocks.
harness = ROOT / 'tests/transport-harness'
production = (COMPONENT / 'helio_transport.cpp').read_text()
production = production.replace('#include "helio_bridge.h"', '#include "fake_bridge.h"').replace('#include "esphome/core/log.h"', '')
remote = (COMPONENT / 'helio_remote.cpp').read_text()
remote = remote[:remote.index('void HelioBridge::controller_snapshot_')]
remote = remote.replace('#include "helio_bridge.h"', '#include "fake_bridge.h"').replace('#include "esphome/core/log.h"', '').replace('#include "esphome/core/hal.h"', '')
controller = BUILD / 'transport-controller.cpp'
controller.write_text(production + remote + '}\n')
bridge = (COMPONENT / 'helio_bridge.cpp').read_text()
alarms = BUILD / 'alarm-controller.cpp'
alarms.write_text('#include "fake_bridge.h"\nnamespace esphome::helio_bridge {\n' + bridge[bridge.index('void HelioBridge::request_alarms_'):])
run('c++', '-std=c++17', '-I'+str(COMPONENT), '-I'+str(harness), str(controller), str(alarms), str(harness/'test_transport.cpp'), '-o', str(BUILD/'transport-controller'))
run(str(BUILD/'transport-controller'))

# Compare B-163 public keys/shared secrets with independent OpenSSL results.
run('cc', '-shared', '-fPIC', str(COMPONENT / 'ecdh.c'), '-o', str(BUILD / 'libhelio_ecdh.dylib'))
for script in ('test_ecdh.py', 'test_shadow.py', 'test_stage_model.py',
               'test_activity_controller.py', 'test_learning_controller.py', 'test_adaptive_logging.py', 'test_onboard_logging.py',
               'test_model_firmware_parity.py', 'test_score_controller.py'):
    run(sys.executable, str(ROOT / 'tests' / script))

print('All firmware host regression checks passed.')
