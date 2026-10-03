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

for name in ('protocol', 'alarms', 'sleep', 'activity', 'smart', 'dual_light'):
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

# Compare B-163 public keys/shared secrets with independent OpenSSL results.
run('cc', '-shared', '-fPIC', str(COMPONENT / 'ecdh.c'), '-o', str(BUILD / 'libhelio_ecdh.dylib'))
for script in ('test_ecdh.py', 'test_shadow.py', 'test_stage_model.py',
               'test_activity_controller.py', 'test_onboard_logging.py',
               'test_model_firmware_parity.py'):
    run(sys.executable, str(ROOT / 'tests' / script))
print('All host regression checks passed.')
