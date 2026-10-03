import ctypes
from pathlib import Path
import subprocess
import tempfile

def openssl(*args):
    result = subprocess.run(['/usr/bin/openssl', *args], capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stderr.decode())
    return result.stdout

def private_der(number):
    content = bytes.fromhex('0201010415') + number.to_bytes(21, 'big') + bytes.fromhex('a00706052b8104000f')
    return bytes([0x30, len(content)]) + content

lib = ctypes.CDLL(str(Path('.test-build/libhelio_ecdh.dylib').resolve()))
Array24 = ctypes.c_ubyte * 24
Array48 = ctypes.c_ubyte * 48
for seed in (1, 7, 19):
    private = Array24(*[(i*13+seed)%256 for i in range(24)])
    public = Array48()
    assert lib.ecdh_generate_keys(public, private) == 1
    number = int.from_bytes(bytes(private), 'little')
    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        a, b, peer = (temp / name for name in ('a.der', 'b.der', 'peer.der'))
        a.write_bytes(private_der(number))
        b.write_bytes(private_der(number + 1234567))
        point = openssl('ec', '-inform', 'DER', '-in', str(a), '-pubout', '-outform', 'DER')[-43:]
        assert point[0] == 4
        expected = int.from_bytes(point[1:22], 'big').to_bytes(24, 'little') + int.from_bytes(point[22:], 'big').to_bytes(24, 'little')
        assert bytes(public) == expected
        remote_der = openssl('ec', '-inform', 'DER', '-in', str(b), '-pubout', '-outform', 'DER')
        peer.write_bytes(remote_der)
        remote = remote_der[-43:]
        remote_bytes = Array48.from_buffer_copy(int.from_bytes(remote[1:22], 'big').to_bytes(24, 'little') + int.from_bytes(remote[22:], 'big').to_bytes(24, 'little'))
        shared = Array48()
        assert lib.ecdh_shared_secret(private, remote_bytes, shared) == 1
        pem = temp / 'a.pem'
        pem.write_bytes(openssl('ec', '-inform', 'DER', '-in', str(a), '-outform', 'PEM'))
        expected_x = openssl('pkeyutl', '-derive', '-inkey', str(pem), '-peerform', 'DER', '-peerkey', str(peer))
        assert int.from_bytes(bytes(shared)[:24], 'little') == int.from_bytes(expected_x, 'big')
    assert lib.ecdh_shared_secret(private, Array48(), shared) == 0
print('B-163 public keys and shared secrets match independent OpenSSL implementation; zero point rejected')
