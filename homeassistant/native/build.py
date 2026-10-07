"""Build the identical ESP smart policy and models for a native HA runtime."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
native = root / 'homeassistant/native'
component = root / 'components/helio_bridge'
output = Path(sys.argv[1]) if len(sys.argv) > 1 else root / '.test-build/helio-engine'
output.parent.mkdir(parents=True, exist_ok=True)
policy = output.parent / 'helio-policy.cpp'
source = (component / 'helio_smart.cpp').read_text()
source = source.replace('#include "helio_bridge.h"', '#include "adapter.h"')
source = source.replace('#include "esphome/core/log.h"', '')
policy.write_text(source)
subprocess.run(['c++', '-std=c++17', '-O2', *(['-static'] if '--static' in sys.argv else []),
                '-I'+str(component), '-I'+str(native), str(policy), str(native/'engine.cpp'),
                '-o', str(output)+'.new'], check=True)
Path(str(output)+'.new').replace(output)
