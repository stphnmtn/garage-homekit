#!/bin/bash
# Emulator test of the firmware's network behaviour: boot, signed OTA (accept/refuse),
# rollback confirmation and a real HomeKit pairing (aiohomekit, as used by Home Assistant).
# Uses the QEMU variant (OpenCores Ethernet); GPIO/ADC behaviour is not covered.
# Needs no secret: it signs with a throwaway key and pairs with a throwaway setup code, so the
# same run works on the Mac and in CI.
set -euo pipefail
cd "$(dirname "$0")/.."
docker run --rm -v "$PWD":/project -w /project "espressif/idf:$(cat IDF_VERSION)" \
  bash /project/tools/qemu-test-inner.sh
