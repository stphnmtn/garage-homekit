#!/bin/bash
# Build and sign. Output: build/garage-homekit-signed.bin (the only file ever pushed).
set -euo pipefail
cd "$(dirname "$0")/.."
IMG=espressif/idf:$(cat IDF_VERSION)

[ -s keys/ota_verify.bin ] || { echo "run tools/keygen.sh first" >&2; exit 1; }
# Setup info (salt + verifier, not the code) for the one-time NVS provisioning; a build
# without it (CI, releases) reads what the device already holds.
umask 077
security find-generic-password -s garage-homekit-setup -w | python3 tools/setup_info.py main/secrets.h
rm -rf build/esp-idf/main  # secrets.h is picked up by __has_include, which ninja cannot track

# sdkconfig is regenerated from sdkconfig.defaults every time, so the build is reproducible.
rm -f sdkconfig
docker run --rm -v "$PWD":/project -w /project "$IMG" idf.py --no-ccache build

mkdir -p "$HOME/.cache"; tmp=$(mktemp -d "$HOME/.cache/garage-key.XXXXXX")  # Docker sees only /Users
trap 'rm -rf "$tmp"' EXIT
security find-generic-password -s garage-ota-signing-key -w | base64 -d > "$tmp/key.pem"
docker run --rm -v "$PWD":/project -v "$tmp":/k -w /project "$IMG" bash -c '
  espsecure.py extract_public_key --version 1 --keyfile /k/key.pem /k/pub.bin >/dev/null &&
  espsecure.py sign_data --version 1 --keyfile /k/key.pem \
    --output build/garage-homekit-signed.bin build/garage-homekit.bin'
# A key that does not match the committed verification key would make the device refuse
# every later update, so stop here rather than push such an image.
cmp -s "$tmp/pub.bin" keys/ota_verify.bin || { echo "Keychain key does not match keys/ota_verify.bin" >&2; exit 1; }
ls -l build/garage-homekit-signed.bin
