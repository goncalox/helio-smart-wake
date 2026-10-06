"""Decode saved 594-byte Helio sleep records without discarding raw data."""
import re
import struct
ANSI = re.compile(r"\x1b\[[0-9;]*m")
MAX_BYTES = 594 * 32
STAGES = {4: "Light", 5: "Deep", 7: "Awake", 8: "REM"}

def decode_record(raw):
    """Preserve raw bytes separately; unknown formats remain inspectable."""
    u16 = lambda p: struct.unpack_from('<H', raw, p)[0]
    base = struct.unpack_from('<I', raw, 4)[0] - 86400
    result = dict(base=base, onset=base + u16(10) * 60,
                  end=base + u16(12) * 60,
                  sleep_score=raw[0x16] if raw[0x16] <= 100 else None,
                  totals=dict(zip(('REM', 'Light', 'Deep', 'Awake'),
                                  (u16(p) for p in (0x24a, 0x24c, 0x24e, 0x250)))))
    for block, name in enumerate(('night', 'day')):
        count = raw[0x54 + block]
        if count > 50:
            result[name + '_error'] = 'segment count exceeds block'
            continue
        result[name] = []
        for i in range(count):
            at = 0x56 + block * 250 + i * 5
            code = raw[at + 4]
            result[name].append(dict(start=base + u16(at) * 60,
                end=base + (u16(at + 2) + 1) * 60,
                code=code, stage=STAGES.get(code, 'Gap' if code == 128 else 'Unknown')))
    return result
