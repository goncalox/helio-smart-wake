"""Reject corrupted, partial and mismatched replay data without importing HA."""
import importlib.util
from pathlib import Path
import struct
import zlib

path=Path(__file__).resolve().parents[1]/'homeassistant/custom_components/helio_smart_wake/journal.py'
spec=importlib.util.spec_from_file_location('journal',path);journal=importlib.util.module_from_spec(spec);spec.loader.exec_module(journal)
def blob(raw,seq=7,magic=b'HLG2'):
    packed=bytearray()
    for n in raw:
        packed.extend(bytes([n]) if n else b'\0\1')
    return struct.pack('<4sIII',magic,seq,len(raw),zlib.crc32(raw))+packed
raw=struct.pack('<BIIH',9,1791351000,124000,3)+b'abc'
assert journal.decode(blob(raw),7)==(b'HLG2',raw)
assert list(journal.events(raw))==[(9,1791351000,124000,b'abc')]
for broken,seq in [(blob(raw)[:-1],7),(blob(raw)[:-1]+b'x',7),(blob(raw),8),(blob(raw,magic=b'NOPE'),7)]:
    try:journal.decode(broken,seq)
    except (ValueError,struct.error):pass
    else:raise AssertionError('Corrupt transfer accepted')
for broken in (raw[:-1],raw+b'x'):
    try:list(journal.events(broken))
    except ValueError:pass
    else:raise AssertionError('Partial event accepted')
print('HA journal: lossless replay, CRC, sequence, bounds and incomplete events passed')
