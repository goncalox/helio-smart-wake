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

for name in ('protocol', 'alarms', 'sleep', 'activity', 'smart', 'dual_light', 'follow', 'adaptive', 'read_health', 'sleep_score', 'remote'):
    output = BUILD / name
    run('c++', '-std=c++17', '-I' + str(COMPONENT),
        str(ROOT / f'tests/test_{name}.cpp'), '-o', str(output))
    run(str(output))

# Compile the actual controller against the test clock/persistence/BLE mocks.
smart = ROOT / 'tests/smart-harness'
production = (COMPONENT / 'helio_smart.cpp').read_text()
production = production.replace('#include "helio_bridge.h"', '#include "fake_bridge.h"')
production = production.replace('#include "esphome/core/log.h"', '')
controller = BUILD / 'smart-controller.cpp'
controller.write_text(production)
run('c++', '-std=c++17', '-I' + str(COMPONENT), '-I' + str(smart),
    str(controller), str(smart / 'test_no_deadline.cpp'), '-o', str(BUILD / 'smart-controller'))
run(str(BUILD / 'smart-controller'))
run('c++', '-std=c++17', '-I' + str(COMPONENT), '-I' + str(smart),
    str(controller), str(smart / 'test_follow.cpp'), '-o', str(BUILD / 'follow-controller'))
run(str(BUILD / 'follow-controller'))
bridge=(COMPONENT / 'helio_bridge.cpp').read_text()
alarms=BUILD / 'alarm-controller.cpp'
alarms.write_text('#include "fake_bridge.h"\nnamespace esphome::helio_bridge {\n' +
                  bridge[bridge.index('void HelioBridge::request_alarms_'):])
run('c++', '-std=c++17', '-I' + str(COMPONENT), '-I' + str(smart),
    str(controller), str(alarms), str(smart / 'test_follow_alarms.cpp'), '-o', str(BUILD / 'follow-alarms'))
run(str(BUILD / 'follow-alarms'))


# Compare B-163 public keys/shared secrets with independent OpenSSL results.
run('cc', '-shared', '-fPIC', str(COMPONENT / 'ecdh.c'), '-o', str(BUILD / 'libhelio_ecdh.dylib'))
for script in ('test_ecdh.py', 'test_shadow.py', 'test_stage_model.py',
               'test_activity_controller.py', 'test_learning_controller.py', 'test_adaptive_logging.py', 'test_onboard_logging.py',
               'test_model_firmware_parity.py', 'test_score_controller.py'):
    run(sys.executable, str(ROOT / 'tests' / script))

# HA native worker uses the same policy source with a process/transport adapter.
run(sys.executable, str(ROOT/'homeassistant/native/build.py'))
run('c++', '-std=c++17', '-O2', '-I'+str(COMPONENT), '-I'+str(ROOT/'homeassistant/native'),
    str(BUILD/'helio-policy.cpp'), str(ROOT/'homeassistant/native/test_engine.cpp'), '-o', str(BUILD/'test-engine'))
run(str(BUILD/'test-engine'))
run(sys.executable, str(ROOT/'tests/test_ha_journal.py'))

run(sys.executable, str(ROOT/'tests/test_ha_controller.py'))
run(sys.executable, str(ROOT/'tests/test_automation_policy.py'))
print('All host regression checks passed.')
