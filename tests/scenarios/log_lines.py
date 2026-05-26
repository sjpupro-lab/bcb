#!/usr/bin/env python3
"""log_lines.py — synthetic syslog-style application log lines.

Emits RFC3164-ish syslog lines as a deterministic byte stream. Lines share
heavy structural redundancy (timestamps, hostnames, levels), the classic case
for a shared prior.

Usage: log_lines.py [--bytes N] [--seed S]
"""
import argparse
import random
import sys

MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
HOSTS = ["web01", "web02", "db01", "cache03", "worker-7", "lb-edge1"]
APPS = ["nginx", "sshd", "kernel", "systemd", "app", "cron", "postfix"]
LEVELS = ["INFO", "INFO", "INFO", "WARN", "ERROR", "DEBUG"]
MSGS = [
    "Accepted publickey for deploy from 10.0.{a}.{b} port {p} ssh2",
    "GET /api/v1/health 200 {ms}ms",
    "connection from 192.168.{a}.{b}:{p} closed",
    "request completed status={code} latency={ms}ms route=/users/{id}",
    "disk usage on / is {pct}%",
    "worker {id} processed job in {ms}ms",
    "failed to connect to upstream 10.0.{a}.{b}:{p}: timeout",
    "user {id} logged in from session {sid}",
]


def gen_line(rng):
    mon = rng.choice(MONTHS)
    day = "%2d" % rng.randint(1, 28)
    t = "%02d:%02d:%02d" % (rng.randint(0, 23), rng.randint(0, 59), rng.randint(0, 59))
    host = rng.choice(HOSTS)
    app = rng.choice(APPS)
    pid = rng.randint(100, 32000)
    lvl = rng.choice(LEVELS)
    msg = rng.choice(MSGS).format(
        a=rng.randint(0, 255), b=rng.randint(0, 255), p=rng.randint(1024, 65535),
        ms=rng.randint(1, 5000), code=rng.choice([200, 201, 301, 404, 500, 503]),
        id=rng.randint(1, 99999), pct=rng.randint(10, 99),
        sid="%012x" % rng.getrandbits(48),
    )
    return f"{mon} {day} {t} {host} {app}[{pid}]: {lvl} {msg}\n".encode("ascii")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bytes", type=int, default=262144)
    ap.add_argument("--seed", type=int, default=1234)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    out = sys.stdout.buffer
    written = 0
    while written < args.bytes:
        line = gen_line(rng)
        out.write(line)
        written += len(line)


if __name__ == "__main__":
    main()
