#!/usr/bin/env python3
"""iot_packets.py — synthetic 18-byte IoT sensor telemetry packets.

Binary packet layout (18 bytes, big-endian):
  magic(1) ver(1) dev_id(2) seq(2) ts(4) temp_c_x100(2,int16) humidity_x100(2,uint16)
  battery_mv(2) flags(1) crc8(1)

Sensor values walk slowly (autocorrelated), which is what a shared prior
exploits. Deterministic given --seed.

Usage: iot_packets.py [--bytes N] [--seed S]
"""
import argparse
import random
import struct
import sys

MAGIC = 0xA5
VERSION = 0x02


def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bytes", type=int, default=262144)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--devices", type=int, default=8,
                    help="number of interleaved devices (1 = single per-device stream)")
    args = ap.parse_args()
    rng = random.Random(args.seed)
    out = sys.stdout.buffer

    n_dev = max(1, args.devices)
    devs = []
    for d in range(n_dev):
        devs.append({
            "id": 0x1000 + d,
            "seq": rng.randint(0, 1000),
            "ts": 1_700_000_000 + rng.randint(0, 86400),
            "temp": rng.randint(1800, 2600),    # 18.00..26.00 C
            "hum": rng.randint(3000, 7000),     # 30.00..70.00 %
            "batt": rng.randint(3300, 4200),    # mV
        })

    written = 0
    while written < args.bytes:
        s = devs[rng.randrange(n_dev)]
        s["seq"] = (s["seq"] + 1) & 0xFFFF
        s["ts"] += rng.randint(1, 5)
        s["temp"] = max(1500, min(3000, s["temp"] + rng.randint(-3, 3)))
        s["hum"] = max(2000, min(9000, s["hum"] + rng.randint(-5, 5)))
        s["batt"] = max(3000, s["batt"] - (1 if rng.random() < 0.05 else 0))
        flags = rng.getrandbits(4)
        body = struct.pack(">BBHHIHHH B", MAGIC, VERSION, s["id"], s["seq"],
                           s["ts"] & 0xFFFFFFFF, s["temp"] & 0xFFFF,
                           s["hum"], s["batt"], flags)
        pkt = body + bytes([crc8(body)])
        out.write(pkt)
        written += len(pkt)


if __name__ == "__main__":
    main()
