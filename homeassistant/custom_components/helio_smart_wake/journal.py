"""Strict lossless journal decoding; complete CRC-valid transfers only."""
import struct
import zlib

def decode(blob, sequence):
    if len(blob) < 16:
        raise ValueError("Short journal")
    magic, seq, length, crc = struct.unpack_from("<4sIII", blob)
    if magic not in (b"HLG2", b"HLP2", b"HLS1") or seq != sequence or length > 200000:
        raise ValueError("Invalid journal header")
    raw = bytearray()
    at = 16
    while at < len(blob):
        value = blob[at]
        at += 1
        if value:
            raw.append(value)
        else:
            if at == len(blob) or not blob[at]:
                raise ValueError("Invalid zero run")
            raw.extend(b"\0" * blob[at])
            at += 1
        if len(raw) > length:
            raise ValueError("Oversized journal")
    if len(raw) != length or zlib.crc32(raw) != crc:
        raise ValueError("Journal CRC mismatch")
    return magic, bytes(raw)

def events(raw):
    at = 0
    while at < len(raw):
        if len(raw) - at < 11:
            raise ValueError("Incomplete event")
        kind, stamp, uptime, size = struct.unpack_from("<BIIH", raw, at)
        at += 11
        payload = raw[at:at+size]
        if len(payload) != size:
            raise ValueError("Incomplete event payload")
        at += size
        yield kind, stamp, uptime, payload
