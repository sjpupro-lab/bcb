#!/usr/bin/env python3
"""mqtt_messages.py — synthetic MQTT PUBLISH packets.

Emits MQTT v3.1.1 PUBLISH control packets (fixed header + topic + small JSON
payload) as a deterministic byte stream. Topics share a common prefix
structure, which a shared prior exploits.

Usage: mqtt_messages.py [--bytes N] [--seed S]
"""
import argparse
import random
import struct
import sys

TOPIC_ROOTS = ["sensors", "devices", "home", "factory", "fleet"]
SUBSYS = ["temperature", "humidity", "power", "status", "gps", "door"]
ROOMS = ["kitchen", "garage", "floor1", "zone-a", "unit42", "bay7"]


def encode_remaining_length(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n > 0:
            b |= 0x80
        out.append(b)
        if n == 0:
            break
    return bytes(out)


def gen_packet(rng):
    topic = "/".join([
        rng.choice(TOPIC_ROOTS), rng.choice(ROOMS), rng.choice(SUBSYS),
        str(rng.randint(0, 31)),
    ])
    payload = (
        '{"v":%.2f,"u":"%s","ts":%d,"ok":%s}' % (
            rng.uniform(0, 100),
            rng.choice(["C", "%", "W", "mm"]),
            1_700_000_000 + rng.randint(0, 100000),
            rng.choice(["true", "false"]),
        )
    ).encode("ascii")
    topic_b = topic.encode("ascii")
    var = struct.pack(">H", len(topic_b)) + topic_b
    body = var + payload
    qos = rng.choice([0, 0, 1])
    fixed = bytes([0x30 | (qos << 1)]) + encode_remaining_length(len(body))
    return fixed + body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bytes", type=int, default=262144)
    ap.add_argument("--seed", type=int, default=1234)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    out = sys.stdout.buffer
    written = 0
    while written < args.bytes:
        p = gen_packet(rng)
        out.write(p)
        written += len(p)


if __name__ == "__main__":
    main()
