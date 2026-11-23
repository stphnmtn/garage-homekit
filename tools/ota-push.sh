#!/bin/bash
# Push the signed image to the running custom firmware. The device verifies the signature
# and the version itself; this script only transports the file.
set -euo pipefail
cd "$(dirname "$0")/.."
HOST=${1:-garage.local}
curl -sS --fail-with-body --max-time 120 --data-binary @build/garage-homekit-signed.bin \
  "http://$HOST:8032/ota"
