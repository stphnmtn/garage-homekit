#!/bin/bash
# Runs inside the ESP-IDF container; called by qemu-test.sh.
set -euo pipefail
. "$IDF_PATH/export.sh" >/dev/null 2>&1
B=build-qemu
V=$(cat VERSION)
mkdir -p "$B"
rm -f "$B/sdkconfig" "$B/test.pem"  # stale sdkconfig overrides the defaults; espsecure will not overwrite a key

# Throwaway secrets: a test signing key baked in as the verification key, a test setup code.
espsecure.py generate_signing_key --version 1 "$B/test.pem" >/dev/null
espsecure.py extract_public_key --version 1 --keyfile "$B/test.pem" "$B/test_verify.bin" >/dev/null
echo "CONFIG_SECURE_BOOT_VERIFICATION_KEY=\"$B/test_verify.bin\"" > "$B/sdkconfig.testkey"
SETUP_CODE=$(python3 -c 'import random
while True:
    d = "".join(random.choice("0123456789") for _ in range(8))
    if len(set(d)) > 1 and d not in ("12345678", "87654321"):
        break
print(f"{d[:3]}-{d[3:5]}-{d[5:]}")')
trap 'rm -f main/secrets.h' EXIT

build() {  # $1 = version, $2 = 1 for an image that never confirms itself
  rm -rf "$B/esp-idf/main"  # secrets.h comes and goes (__has_include), which neither ninja nor ccache tracks
  idf.py -B "$B" -D SDKCONFIG="$B/sdkconfig" -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu;$B/sdkconfig.testkey" \
    -D PROJECT_VER_OVERRIDE="$1" -D GARAGE_NO_CONFIRM="${2:-0}" --no-ccache build >/dev/null
  espsecure.py sign_data --version 1 --keyfile "$B/test.pem" \
    --output "$B/v$1-signed.bin" "$B/garage-homekit.bin" >/dev/null
  cp "$B/garage-homekit.bin" "$B/v$1-unsigned.bin"
}

# The Shelly's situation: a bootloader that ignores rollback (here: ours built without it).
echo "== build a bootloader without rollback, v$V (provisions setup info), v$((V + 1)), v$((V + 2)) (never confirms)"
rm -rf "$B-oldbl"
echo "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n" > "$B/sdkconfig.norollback"
idf.py -B "$B-oldbl" -D SDKCONFIG="$B-oldbl.sdkconfig" \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu;$B/sdkconfig.testkey;$B/sdkconfig.norollback" \
  bootloader >/dev/null
rm -f "$B-oldbl.sdkconfig"
build "$((V + 1))"
build "$((V + 2))" 1
echo "$SETUP_CODE/TEST" | python3 tools/setup_info.py main/secrets.h
build "$V"
rm -f main/secrets.h
for v in "$V" "$((V + 1))"; do  # the code never enters an image, not even a provisioning one
  strings "$B/v$v-unsigned.bin" | grep -q "$SETUP_CODE" && { echo "FAIL: setup code in v$v"; exit 1; }
done
espsecure.py generate_signing_key --version 1 /tmp/other.pem >/dev/null
espsecure.py sign_data --version 1 --keyfile /tmp/other.pem \
  --output "$B/v$((V + 1))-foreign.bin" "$B/v$((V + 1))-unsigned.bin" >/dev/null

esptool.py --chip esp32 merge_bin --fill-flash-size 4MB -o "$B/flash.bin" \
  0x1000 "$B-oldbl/bootloader/bootloader.bin" 0x8000 "$B/partition_table/partition-table.bin" \
  0x9000 "$B/ota_data_initial.bin" 0x10000 "$B/v$V-signed.bin" >/dev/null

# Detached from the terminal: with -nographic, later commands touching stdin killed QEMU.
start_qemu() {  # $1 = log file
  qemu-system-xtensa -display none -monitor none -serial file:"$1" \
    -machine esp32 -m 4M -drive file="$B/flash.bin",if=mtd,format=raw \
    -nic user,model=open_eth,hostfwd=tcp::18080-:80,hostfwd=tcp::18032-:8032 \
    -global driver=timer.esp32.timg,property=wdt_disable,value=true \
    < /dev/null > "$B/qemu.err" 2>&1 &
  QPID=$!
}
# QEMU cannot reboot cleanly (no peripheral reset): emulate a power cycle instead.
power_cycle() {  # $1 = new log file
  kill "$QPID"; wait "$QPID" 2>/dev/null || true
  start_qemu "$1"
}
start_qemu "$B/qemu.log"
trap 'kill $QPID 2>/dev/null || true; rm -f main/secrets.h' EXIT

wait_version() {  # $1 = expected
  for _ in $(seq 1 90); do
    v=$(curl -s --max-time 2 http://127.0.0.1:18032/version || true)
    [ "$v" = "$1" ] && return 0
    sleep 1
  done
  echo "FAIL: version $1 not reached (last: '$v')"; for f in "$B"/qemu*.log; do tail -n 40 "$f"; done; exit 1
}

push() {  # $1 = file, $2 = expected HTTP status
  # The emulated NIC drops frames (opencores "RX frame dropped"): retry transport failures.
  for _ in 1 2 3; do
    rm -f /tmp/resp
    s=$(curl -s -o /tmp/resp -w '%{http_code}' --max-time 120 --data-binary @"$1" \
      http://127.0.0.1:18032/ota || true)
    [ "$s" != 000 ] && break
    sleep 5
  done
  if [ "$s" = "$2" ]; then echo "ok   $(basename "$1") -> $s $(head -c 60 /tmp/resp)"
  else echo "FAIL $(basename "$1") -> $s (want $2) $(cat /tmp/resp)"; for f in "$B"/qemu*.log; do tail -n 30 "$f"; done; exit 1; fi
}

wait_version "$V"; sleep 5
echo "== booted v$V"
grep -E "ip |HAP Started|version" "$B/qemu.log" | head -5

echo "== HomeKit pairing and control"
pip install -q aiohomekit==4.0.1 >/dev/null 2>&1
# The emulated CPU needs well over aiohomekit's fixed 30 s for SRP-3072; test-only patch.
sed -i 's/loop.time() + 30,/loop.time() + 900,/' \
  "$(python -c 'import aiohomekit.controller.ip.connection as c; print(c.__file__)')"
python tools/hap_test.py 127.0.0.1 18080 "$SETUP_CODE"
n=$(grep -c "door pulse" "$B/qemu.log" || true)
[ "$n" = 2 ] && echo "ok   2 presses (open, re-open); close pressed nothing" \
  || { echo "FAIL: $n door presses, want 2"; exit 1; }
curl -s --max-time 5 http://127.0.0.1:18032/status > /tmp/status
grep -q "^pulses 2$" /tmp/status && grep -q "^hk_sessions_max [1-9]" /tmp/status \
  && echo "ok   /status: $(tr '\n' ' ' < /tmp/status)" || { echo "FAIL /status"; cat /tmp/status; exit 1; }
curl -s --max-time 5 http://127.0.0.1:18032/log | grep -E "hk connect|door open from" | head -3 | sed 's/^/     /'
curl -s --max-time 5 http://127.0.0.1:18032/log | grep -q "door open from" && echo "ok   /log has the writes" \
  || { echo "FAIL /log"; exit 1; }
grep -q "setup info written" "$B/qemu.log" && echo "ok   setup info provisioned into NVS" \
  || { echo "FAIL: setup info not written"; exit 1; }
grep -q "bootloader replaced" "$B/qemu.log" && echo "ok   bootloader replaced after the first HomeKit connection" \
  || { echo "FAIL: bootloader not replaced"; grep -i bootloader "$B/qemu.log"; exit 1; }

echo "== OTA checks"
push "$B/v$((V + 1))-unsigned.bin" 403
push "$B/v$((V + 1))-foreign.bin" 403
push "$B/v$((V + 1))-signed.bin" 200
power_cycle "$B/qemu2.log"
wait_version "$((V + 1))"
sleep 3
grep -q "image confirmed" "$B/qemu2.log" && { echo "FAIL: confirmed before any HomeKit connection"; exit 1; }
echo "== HomeKit pairing survives the update; the connection confirms the image"
python tools/hap_test.py 127.0.0.1 18080 "$SETUP_CODE" --reuse
sleep 2
grep -q "image confirmed" "$B/qemu2.log" && echo "ok   v$((V + 1)) confirmed (the new bootloader marked it pending)" \
  || { echo "FAIL: no confirmation"; exit 1; }
push "$B/v$V-signed.bin" 409
echo "== after another power cycle the confirmed v$((V + 1)) stays"
power_cycle "$B/qemu3.log"
wait_version "$((V + 1))"
echo "ok   v$((V + 1)) still running"

echo "== rollback: v$((V + 2)) never confirms; a reset while it is on trial reverts to v$((V + 1))"
push "$B/v$((V + 2))-signed.bin" 200
power_cycle "$B/qemu4.log"
wait_version "$((V + 2))"
power_cycle "$B/qemu5.log"
wait_version "$((V + 1))"
echo "ok   rolled back to v$((V + 1))"

echo "== a build without secrets pairs from the setup info in NVS"
python tools/hap_test.py 127.0.0.1 18080 "$SETUP_CODE" --repair
echo "ALL TESTS PASSED"
