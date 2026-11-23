"""Sign an app image the way `espsecure.py sign_data --version 1` does, with openssl only.

usage: sign.py KEY.pem IN.bin OUT.bin VERIFY_KEY.bin

Appends a 4-byte zero version word and the raw ECDSA P-256 signature (r || s, 32 bytes each)
of SHA-256(image). Refuses a key whose public half differs from VERIFY_KEY.bin (raw x || y,
as espsecure extract_public_key writes it): the device would refuse every later update.
No third-party Python, so the CI job that holds the key runs nothing it downloaded.
"""
import subprocess
import sys
from pathlib import Path

key, src, dst, verify = sys.argv[1:5]


def openssl(*args, data=None):
    return subprocess.run(["openssl", *args], input=data, capture_output=True, check=True).stdout


def der_len(b, i):
    n = b[i]
    if n < 0x80:
        return n, i + 1
    k = n & 0x7F
    return int.from_bytes(b[i + 1:i + 1 + k], "big"), i + 1 + k


# Public key: the last 64 bytes of the uncompressed point in the DER SubjectPublicKeyInfo.
pub = openssl("ec", "-in", key, "-pubout", "-outform", "DER")
if pub[-65] != 0x04 or pub[-64:] != Path(verify).read_bytes():
    sys.exit("signing key does not match " + verify)

image = Path(src).read_bytes()
sig = openssl("dgst", "-sha256", "-sign", key, data=image)  # DER: SEQUENCE { INTEGER r, INTEGER s }
assert sig[0] == 0x30
_, i = der_len(sig, 1)
raw = b""
for _ in range(2):
    assert sig[i] == 0x02
    n, i = der_len(sig, i + 1)
    raw += int.from_bytes(sig[i:i + n], "big").to_bytes(32, "big")
    i += n
Path(dst).write_bytes(image + b"\0\0\0\0" + raw)
print(f"signed {len(image)} bytes -> {dst}")
