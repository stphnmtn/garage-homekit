#!/bin/bash
# One-time: create the OTA signing key and the HomeKit setup code, both kept in the Mac
# Keychain only. Writes the public verification key into keys/ (safe to commit).
set -euo pipefail
cd "$(dirname "$0")/.."
IMG=espressif/idf:$(cat IDF_VERSION)

if security find-generic-password -s garage-ota-signing-key >/dev/null 2>&1; then
  echo "signing key already in Keychain; refusing to overwrite" >&2
  exit 1
fi

mkdir -p "$HOME/.cache"; tmp=$(mktemp -d "$HOME/.cache/garage-key.XXXXXX")  # Docker sees only /Users
trap 'rm -rf "$tmp"' EXIT
chmod 700 "$tmp"
docker run --rm -v "$tmp":/k -v "$PWD/keys":/out "$IMG" bash -c '
  espsecure.py generate_signing_key --version 1 /k/key.pem >/dev/null &&
  espsecure.py extract_public_key --version 1 --keyfile /k/key.pem /out/ota_verify.bin >/dev/null'
[ -s "$tmp/key.pem" ] || { echo "key not generated" >&2; exit 1; }
# security -i reads the command from stdin, so the secret never appears in argv.
printf 'add-generic-password -s garage-ota-signing-key -a garage -w %s\n' \
  "$(base64 < "$tmp/key.pem" | tr -d '\n')" | security -i >/dev/null
[ "$(security find-generic-password -s garage-ota-signing-key -w | base64 -d | head -1)" = "-----BEGIN EC PRIVATE KEY-----" ] || { echo "key not stored" >&2; exit 1; }

if ! security find-generic-password -s garage-homekit-setup >/dev/null 2>&1; then
  # 8 random digits, avoiding the codes the HAP spec forbids (all equal, 12345678, 87654321).
  while :; do
    d=$(od -An -N8 -tu1 /dev/urandom | awk '{for(i=1;i<=NF;i++) printf "%d", $i%10}')
    case $d in 00000000|11111111|22222222|33333333|44444444|55555555|66666666|77777777|88888888|99999999|12345678|87654321) continue;; esac
    break
  done
  code="${d:0:3}-${d:3:2}-${d:5:3}"
  id=$(od -An -N4 -tu1 /dev/urandom | awk '{for(i=1;i<=NF;i++) printf "%s", substr("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", ($i%36)+1, 1)}')
  printf 'add-generic-password -s garage-homekit-setup -a garage -w %s\n' "$code/$id" | security -i >/dev/null
fi
echo "Keychain: garage-ota-signing-key, garage-homekit-setup. Back the signing key up off the Mac."
