"""Observed minute summaries; bytes 4–7 are retained but are not model inputs."""
import base64
import struct

def decode_activity(payload):
    if len(payload) < 17 or payload[0] != 1 or (len(payload)-17) % 8:
        raise ValueError('invalid activity snapshot')
    requested, start = struct.unpack_from('<II', payload, 1)
    rows = []
    for at in range(17, len(payload), 8):
        raw = payload[at:at+8]
        rows.append(dict(timestamp=start+(at-17)//8*60, kind=raw[0], intensity=raw[1],
                         steps=raw[2], heart_rate=raw[3] if raw[3] not in (0, 255) else None,
                         raw_hex=raw.hex()))
    return dict(requested_since=requested, start=start, header_hex=payload[9:17].hex(),
                raw_base64=base64.b64encode(payload).decode(), rows=rows)
