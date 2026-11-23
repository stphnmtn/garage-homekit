# garage-homekit

Native HomeKit firmware for a Shelly Pro 1 on Ethernet (`garage.local`): garage door opener +
"Chargeur Voiture" switch, no hub software in the path. It replaced an ESPHome config; pins and
behaviour are mapped 1:1 in `main/board.h`. The first install went over ESPHome's own OTA, which
accepts any image.

## Behaviour

- Door: the Shelly only presses the key fob of a shared, self-closing door. Every "open" (HomeKit
  or the button) pulses GPIO2 for 500 ms, even when the door already shows open, so a press the
  door ignored can be re-sent. The state then goes opening (15 s), open, and is modelled closing
  on its own 32 s later (15 s). "Close" never pulses; it only moves the model. The state is a
  guess: an obstructed door is not detected. In the Home app, tapping an open door shows
  "closing"; tapping again sends a new "open".
  No position sensor: the state is a model.
- Front button: short press = door, double press = charger, 4–8 s = restart,
  10–15 s = erase HomeKit pairings (re-pair needed).
- Charger relay state survives reboots; switched off above 80 °C (internal NTC).
- LEDs: red = no network, blue = network up, green = door pulse.

## Security

- HomeKit (HAP over IP) from Espressif's esp-homekit-sdk, vendored at a pinned commit
  (`components/VENDORED`); our code is `main/` only.
- Updates: `POST :8032/ota`. ESP-IDF refuses any image not signed with our ECDSA key
  (Keychain `garage-ota-signing-key`; public key in `keys/`); the firmware refuses older
  versions. A new image is kept only once a HomeKit controller has connected through it
  (immediately if nothing is paired); otherwise the bootloader reverts to the previous one,
  after 5 min or at the next reset.
- Bootloader: the Shelly kept the one from its first serial flash, which ignores rollback
  (v2 and v3 never logged "image confirmed"). From v4 each image carries this build's
  bootloader and writes it over the old one once, after the first HomeKit connection. The
  ESP32 has no backup bootloader: power lost during that write (under a second) needs a cable.
- Setup code: never in an image. A local build carries the SRP salt and verifier
  (`tools/setup_info.py`, from Keychain `garage-homekit-setup`, `code/ID`) and the firmware
  stores them once in NVS (`main/setup.c`); builds without them, as in CI, use what the device
  holds. The verifier allows an offline search of the 10^8 codes, so it is kept as secret as
  the code.
- No Wi-Fi, no Bluetooth, no web UI.

## Use

    tools/keygen.sh                   one time: signing key + setup code into the Keychain
    tools/build.sh                    build + sign -> build/garage-homekit-signed.bin
    tools/qemu-test.sh                emulator tests: boot, OTA accept/refuse, rollback, bootloader
                                      replacement, pairing from NVS setup info (no secrets)
    tools/ota-push.sh                 every later update (bump VERSION first)

Losing the signing key means the next update needs a serial cable; keep a copy off the Mac. `tools/build.sh` refuses to sign with a
key that does not match `keys/ota_verify.bin`.

## CI and updates

- `ci.yml` (push, pull request, manual): device build, then the emulator suite in the
  `espressif/idf` image named in `IDF_VERSION`. It uses a throwaway key and setup code: the
  signing key and the real setup code never leave the Mac.
- Espressif updates are applied by hand: bump `IDF_VERSION`, re-vendor `components/` from
  esp-homekit-sdk (record the commit in `components/VENDORED`), `idf.py update-dependencies`.
- Deploying stays manual: merge, then on the Mac `git pull && tools/build.sh && tools/ota-push.sh`.

## Diagnostics

`curl http://garage.local:8032/status` (uptime, reset reason, heap, open HomeKit sessions and
their peak, paired controllers, presses since boot, door model, charger, temperature) and
`curl http://garage.local:8032/log` (last 8 KB of log lines, timestamps in ms since boot).
Both live in RAM only: nothing is logged to flash, and a reboot clears them. The only flash
writes are the charger state on change, HomeKit pairing changes, the setup info when a local
build changes it, and the one-time bootloader replacement.
