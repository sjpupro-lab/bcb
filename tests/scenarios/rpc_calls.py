#!/usr/bin/env python3
"""rpc_calls.py — synthetic gRPC-like unary call frames.

Emits a deterministic byte stream of small RPC frames: an ASCII method path
(":path /pkg.Service/Method") followed by a length-delimited protobuf-ish
binary body (varint-tagged fields). Mixed text/binary small messages.

Usage: rpc_calls.py [--bytes N] [--seed S]
"""
import argparse
import random
import struct
import sys

SERVICES = [
    ("user.v1.UserService", ["GetUser", "CreateUser", "ListUsers", "DeleteUser"]),
    ("order.v1.OrderService", ["PlaceOrder", "GetOrder", "CancelOrder"]),
    ("chat.v1.ChatService", ["SendMessage", "StreamMessages", "Typing"]),
    ("auth.v1.AuthService", ["Login", "Refresh", "Logout"]),
]


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            break
    return bytes(out)


def field(tag, wire, payload):
    return varint((tag << 3) | wire) + payload


def gen_frame(rng):
    svc, methods = rng.choice(SERVICES)
    method = rng.choice(methods)
    path = ("/%s/%s" % (svc, method)).encode("ascii")

    body = bytearray()
    # field 1: id (varint)
    body += field(1, 0, varint(rng.randint(1, 10_000_000)))
    # field 2: a short string (len-delimited)
    s = rng.choice(["ok", "pending", "user@example.com", "session-token", "ack"]).encode()
    body += field(2, 2, varint(len(s)) + s)
    # field 3: timestamp (fixed64)
    body += field(3, 1, struct.pack("<Q", 1_700_000_000_000 + rng.randint(0, 1_000_000)))
    if rng.random() < 0.5:
        body += field(4, 0, varint(rng.randint(0, 255)))

    frame = bytearray()
    frame += struct.pack(">H", len(path)) + path
    frame += struct.pack(">I", len(body)) + bytes(body)
    return bytes(frame)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bytes", type=int, default=262144)
    ap.add_argument("--seed", type=int, default=1234)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    out = sys.stdout.buffer
    written = 0
    while written < args.bytes:
        f = gen_frame(rng)
        out.write(f)
        written += len(f)


if __name__ == "__main__":
    main()
