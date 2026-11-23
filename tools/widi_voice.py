#!/usr/bin/env python3
"""Laptop-side BLE-MIDI test tool for the WIDI Master -> Yamaha SH-TA setup.

Talks to the WIDI directly from a Linux laptop (BlueZ + bleak), which is much
faster for protocol experiments than reflashing the Sticky.

Only one central can be connected to the WIDI at a time: turn the Sticky off
(hold its power button) first. PipeWire's own BLE-MIDI support may also be
holding the WIDI connection; this tool reuses that link if so.

Usage:
  tools/.venv/bin/python tools/widi_voice.py scan
  tools/.venv/bin/python tools/widi_voice.py monitor         # decoded MIDI from the piano
  tools/.venv/bin/python tools/widi_voice.py note            # middle C (sanity check)
  tools/.venv/bin/python tools/widi_voice.py voice "Electric Piano 1"
  tools/.venv/bin/python tools/widi_voice.py cycle           # step through all voices
  tools/.venv/bin/python tools/widi_voice.py sweep           # a note on every channel 1-16
  tools/.venv/bin/python tools/widi_voice.py metronome --bpm 80 --seconds 20

Options: --address XX:XX:.. (or env WIDI_ADDRESS; default: first "WIDI Master"
found)  --ch N (1-16, default 1)
"""
import argparse
import asyncio
import os
import subprocess
import sys
import time

from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice

MIDI_SVC = "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
MIDI_CHAR = "7772e5db-3868-4112-a1a9-f2669d106bf3"
DEFAULT_ADDR = os.environ.get("WIDI_ADDRESS")  # optional; else scan for "WIDI Master"
BANK_MSB = 108

# SH-TA panel voices: (name, bank LSB, program); bank MSB is always 108.
VOICES = [
    ("Piano", 0, 0), ("Electric Piano 1", 0, 5), ("Electric Piano 2", 0, 4),
    ("Electric Piano 3", 1, 4), ("Harpsichord 1", 0, 6), ("Harpsichord 2", 1, 6),
    ("Vibraphone", 0, 11), ("Celesta", 0, 8), ("Pipe Organ 1", 1, 19),
    ("Pipe Organ 2", 0, 19), ("Pipe Organ 3", 2, 19), ("Pipe Organ 4", 3, 19),
    ("Jazz Organ", 0, 16), ("Strings", 0, 48), ("Choir", 0, 52), ("Synth Pad", 0, 89),
]

_t0 = time.monotonic()


def frame(*msgs: bytes) -> bytes:
    """Wrap complete MIDI messages in one BLE-MIDI 1.0 packet."""
    ms = int((time.monotonic() - _t0) * 1000) & 0x1FFF
    out = bytearray([0x80 | ((ms >> 7) & 0x3F)])  # header
    for m in msgs:
        out.append(0x80 | (ms & 0x7F))  # timestamp before each status byte
        out += m
    return bytes(out)


def unframe(pkt: bytes) -> list[bytes]:
    """Decode a BLE-MIDI packet into MIDI messages (handles running status)."""
    msgs, i, running = [], 1, 0
    while i < len(pkt):
        if pkt[i] & 0x80:  # timestamp
            i += 1
            if i >= len(pkt):
                break
        b = pkt[i]
        if b == 0xF0:  # SysEx: collect up to F7, skipping timestamps
            j, data = i + 1, bytearray([0xF0])
            while j < len(pkt) and pkt[j] != 0xF7:
                if not pkt[j] & 0x80:
                    data.append(pkt[j])
                j += 1
            msgs.append(bytes(data) + b"\xf7")
            i = j + 1
            continue
        if b & 0x80:
            i += 1
            if b >= 0xF0:
                msgs.append(bytes([b]))
                continue
            running = b
        if not running:
            i += 1
            continue
        n = 1 if running & 0xF0 in (0xC0, 0xD0) else 2
        msgs.append(bytes([running]) + pkt[i:i + n])
        i += n
    return msgs


def describe(m: bytes) -> str:
    s = m[0]
    kind = {0x80: "NoteOff", 0x90: "NoteOn", 0xA0: "PolyAT", 0xB0: "CC", 0xC0: "PC",
            0xD0: "ChanAT", 0xE0: "Bend"}.get(s & 0xF0)
    if kind and s < 0xF0:
        return f"ch{(s & 0x0F) + 1:<2} {kind:7} {' '.join(str(x) for x in m[1:])}"
    return {0xFE: "ActiveSensing", 0xF8: "Clock"}.get(s, m.hex(" "))


def bluez_device(address: str) -> BLEDevice | None:
    """Find an already-known BlueZ device (any adapter) — needed when the WIDI is
    already connected (e.g. by PipeWire) and therefore not advertising."""
    node = "dev_" + address.upper().replace(":", "_")
    try:
        tree = subprocess.run(["busctl", "tree", "org.bluez"], capture_output=True, text=True).stdout
    except FileNotFoundError:
        return None
    for line in tree.splitlines():
        path = line.strip().lstrip("├└─│ ").strip()
        if path.endswith("/" + node):
            return BLEDevice(address, None, {"path": path, "props": {}})
    return None


async def connect(address: str | None) -> BleakClient:
    if address:
        dev = await BleakScanner.find_device_by_address(address, timeout=5) or bluez_device(address)
    else:
        dev = await BleakScanner.find_device_by_filter(
            lambda d, adv: (d.name or "").startswith("WIDI") or MIDI_SVC in adv.service_uuids, timeout=8)
    if not dev:
        sys.exit(f"{address or 'WIDI'} not found (powered? held by the Sticky? "
                 "already connected? then pass --address)")
    address = dev.address
    client = BleakClient(dev, timeout=20)
    await client.connect()
    try:
        await client.pair()  # the WIDI drops unbonded links; no-op if already bonded
    except Exception as e:
        print(f"pair(): {e} (continuing)")
    print(f"connected to {address}")

    def on_notify(_, data: bytearray):
        for m in unframe(bytes(data)):
            if m and m[0] != 0xFE:  # hide Active Sensing
                print(f"  rx {describe(m)}")

    try:
        await client.start_notify(MIDI_CHAR, on_notify)
    except Exception as e:  # PipeWire's bluez_midi may already hold notifications
        print(f"notify unavailable ({e}); sending still works")
    return client


async def send(client: BleakClient, *msgs: bytes, label: str = ""):
    pkt = frame(*msgs)
    await client.write_gatt_char(MIDI_CHAR, pkt, response=False)
    print(f"tx {label:22} {pkt.hex(' ')}")


async def send_voice(client, ch, name, lsb, pc):
    c = ch - 1
    await send(client, bytes([0xB0 | c, 0x00, BANK_MSB]), bytes([0xB0 | c, 0x20, lsb]),
               bytes([0xC0 | c, pc]), label=f"{name} ch{ch}")


async def note(client, ch, n=60, length=0.6):
    c = ch - 1
    await send(client, bytes([0x90 | c, n, 100]), label=f"NoteOn {n} ch{ch}")
    await asyncio.sleep(length)
    await send(client, bytes([0x80 | c, n, 0]), label=f"NoteOff {n} ch{ch}")


async def metronome(client, bpm: int, seconds: float):
    c = 9  # ch10, XG Standard Kit: 21 = Metronome Click, 22 = Metronome Bell
    await send(client, bytes([0xB0 | c, 0, 127]), bytes([0xB0 | c, 0x20, 0]), bytes([0xC0 | c, 0]),
               label="ch10 drum kit")
    end, beat = time.monotonic() + seconds, 0
    while time.monotonic() < end:
        n = 22 if beat % 4 == 0 else 21
        await send(client, bytes([0x90 | c, n, 100]), bytes([0x80 | c, n, 0]), label=f"beat {beat + 1}")
        beat += 1
        await asyncio.sleep(60 / bpm)


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["scan", "monitor", "note", "voice", "cycle", "sweep", "metronome"])
    ap.add_argument("name", nargs="?", help="voice name for `voice`")
    ap.add_argument("--address", default=DEFAULT_ADDR)
    ap.add_argument("--ch", type=int, default=1, help="MIDI channel 1-16 (SH-TA uses 1)")
    ap.add_argument("--bpm", type=int, default=80)
    ap.add_argument("--seconds", type=float, default=20)
    a = ap.parse_args()

    if a.cmd == "scan":
        found = await BleakScanner.discover(6, return_adv=True)
        for d, adv in sorted(found.values(), key=lambda x: -x[1].rssi):
            midi = "MIDI" if MIDI_SVC in adv.service_uuids else ""
            print(f"{d.address}  {adv.rssi:4}  {midi:4}  {d.name or ''}")
        return

    client = await connect(a.address)
    try:
        if a.cmd == "monitor":
            print("listening — play keys / change voices on the piano; Ctrl-C to stop")
            await asyncio.Event().wait()
        elif a.cmd == "note":
            await note(client, a.ch)
        elif a.cmd == "sweep":
            for ch in range(1, 17):
                await note(client, ch)
                await asyncio.sleep(0.4)
        elif a.cmd == "voice":
            match = [v for v in VOICES if v[0].lower() == (a.name or "").lower()]
            if not match:
                sys.exit("voices: " + ", ".join(v[0] for v in VOICES))
            await send_voice(client, a.ch, *match[0])
            await asyncio.sleep(0.3)
            await note(client, a.ch)
        elif a.cmd == "cycle":
            for v in VOICES:
                await send_voice(client, a.ch, *v)
                await asyncio.sleep(0.3)
                await note(client, a.ch)
                await asyncio.sleep(1.5)
        elif a.cmd == "metronome":
            await metronome(client, a.bpm, a.seconds)
        await asyncio.sleep(0.5)
    finally:
        await client.disconnect()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
