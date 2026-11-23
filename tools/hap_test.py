"""HomeKit end-to-end check with aiohomekit (the library Home Assistant uses).

First run: pair with the setup code, then check the accessory database and drive both
services. --reuse: reconnect with the saved pairing (after an OTA) and read state.
--repair: remove the saved pairing, then pair again and read state.
"""
import asyncio
import json
import sys
import time
from pathlib import Path
from types import SimpleNamespace

from aiohomekit.characteristic_cache import CharacteristicCacheMemory
from aiohomekit.controller.ip.discovery import IpDiscovery
from aiohomekit.controller.ip.pairing import IpPairing
from aiohomekit.zeroconf import HomeKitService

HOST, PORT, PIN = sys.argv[1], int(sys.argv[2]), sys.argv[3]
REUSE = "--reuse" in sys.argv
REPAIR = "--repair" in sys.argv
SAVE = Path("build-qemu/pairing.json")

CUR_DOOR = "0000000E-0000-1000-8000-0026BB765291"
TGT_DOOR = "00000032-0000-1000-8000-0026BB765291"
ON = "00000025-0000-1000-8000-0026BB765291"


def fail(msg):
    print("FAIL", msg)
    sys.exit(1)


async def find(pairing):
    accs = await pairing.list_accessories_and_characteristics()
    ids = {}
    for acc in accs:
        for serv in acc["services"]:
            for ch in serv["characteristics"]:
                t = ch["type"].upper()
                full = t if "-" in t else f"{int(t, 16):08X}-0000-1000-8000-0026BB765291"
                ids[full] = (acc["aid"], ch["iid"])
    for need in (CUR_DOOR, TGT_DOOR, ON):
        if need not in ids:
            fail(f"characteristic {need} missing")
    return ids


async def retry(fn):
    for attempt in range(5):
        try:
            return await fn()
        except Exception:  # noqa: BLE001
            if attempt == 4:
                raise
            await asyncio.sleep(2)


async def read(pairing, key):
    res = await retry(lambda: pairing.get_characteristics([key]))
    return res[key]["value"]


async def write(pairing, key, val):
    await retry(lambda: pairing.put_characteristics([(key[0], key[1], val)]))


async def main():
    controller = SimpleNamespace(pairings={}, _char_cache=CharacteristicCacheMemory(),
                                 id_factory=lambda: "test")
    if REUSE:
        pairing = IpPairing(controller, json.loads(SAVE.read_text()))
    else:
        if REPAIR:
            old = IpPairing(controller, json.loads(SAVE.read_text()))
            await retry(lambda: old.remove_pairing(old.pairing_data["iOSPairingId"]))
            await old.close()
            print("ok   pairing removed")
        desc = HomeKitService(
            name="Garage", id="00:00:00:00:00:00", model="Pro 1", feature_flags=0,
            status_flags=1, config_num=1, state_num=1, category=4, protocol_version="1.1",
            type="_hap._tcp.local.", address=HOST, addresses=[HOST], port=PORT)
        # The emulated NIC drops frames; retry the first contact a few times.
        for attempt in range(1, 6):
            try:
                disc = IpDiscovery(controller, desc)
                finish = await disc.async_start_pairing("test")
                pairing = await finish(PIN)
                break
            except Exception as ex:  # noqa: BLE001
                print(f"     pairing attempt {attempt} failed: {type(ex).__name__}")
                await asyncio.sleep(5)
        else:
            fail("pairing never succeeded")
        SAVE.write_text(json.dumps(pairing.pairing_data))
        print("ok   paired")

    ids = await find(pairing)
    print("ok   accessory database: garage door + switch")
    door, tgt, sw = ids[CUR_DOOR], ids[TGT_DOOR], ids[ON]

    if REUSE or REPAIR:
        print("ok   reconnected, door state", await read(pairing, door))
        await pairing.close()
        return

    if await read(pairing, door) != 1:
        fail("door should start closed")
    await write(pairing, tgt, 0)
    await asyncio.sleep(2)
    if await read(pairing, door) != 2:
        fail("door should be opening")
    t0 = time.time()
    while await read(pairing, door) != 0:
        if time.time() - t0 > 25:
            fail("door never reported open")
        await asyncio.sleep(1)
    print(f"ok   door opening -> open after {time.time() - t0 + 2:.0f} s")
    await write(pairing, tgt, 0)
    await asyncio.sleep(2)
    if await read(pairing, door) != 2:
        fail("open while open should press again and restart opening")
    print("ok   open re-sent while open")
    await write(pairing, tgt, 1)
    await asyncio.sleep(2)
    if await read(pairing, door) != 3:
        fail("door should be closing")
    print("ok   door closing on request")

    await write(pairing, sw, True)
    await asyncio.sleep(1)
    if await read(pairing, sw) is not True:
        fail("charger should be on")
    await write(pairing, sw, False)
    await asyncio.sleep(1)
    if await read(pairing, sw) is not False:
        fail("charger should be off")
    print("ok   charger on/off")
    await pairing.close()


asyncio.run(main())
