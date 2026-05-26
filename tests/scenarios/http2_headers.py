#!/usr/bin/env python3
"""http2_headers.py — synthetic HTTP/2 header blocks (requests + responses).

Emits one JSON object per line to stdout:
    {"type": "req"|"resp", "headers": [["name","value"], ...]}

Models a realistic browser/API session: many requests to a few hosts, so most
header fields repeat (Host/:authority, user-agent, accept-*, cookie) while a
few vary (:path, referer, content-length, date, etag). This repetition is what
HPACK's dynamic table and BCB's shared prior both exploit — letting us compare
them fairly. Deterministic given --seed.

Usage: http2_headers.py [--blocks N] [--seed S]
"""
import argparse
import json
import random
import sys

AUTHORITIES = ["api.example.com", "www.example.com", "cdn.example.net", "shop.example.com"]
PATHS = ["/", "/index.html", "/api/v1/users", "/api/v1/orders", "/static/app.js",
         "/static/style.css", "/login", "/search", "/products", "/cart", "/checkout",
         "/api/v1/feed", "/assets/logo.png", "/favicon.ico"]
UAS = [
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15",
    "Mozilla/5.0 (X11; Linux x86_64; rv:121.0) Gecko/20100101 Firefox/121.0",
]
ACCEPT = ["text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8",
          "application/json", "*/*", "text/css,*/*;q=0.1", "image/avif,image/webp,*/*"]
CONTENT_TYPES = ["text/html; charset=utf-8", "application/json", "text/css",
                 "application/javascript", "image/png", "image/webp"]
SERVERS = ["nginx", "nginx/1.25.3", "cloudflare", "envoy", "gunicorn/21.2.0"]


def req_block(rng, sess):
    method = rng.choice(["GET", "GET", "GET", "POST", "GET", "GET"])
    h = [
        [":method", method],
        [":scheme", "https"],
        [":authority", sess["authority"]],
        [":path", rng.choice(PATHS) + ("?q=%d" % rng.randint(1, 9999) if rng.random() < 0.3 else "")],
        ["user-agent", sess["ua"]],
        ["accept", rng.choice(ACCEPT)],
        ["accept-encoding", "gzip, deflate, br"],
        ["accept-language", sess["lang"]],
        ["cookie", sess["cookie"]],
    ]
    if rng.random() < 0.5:
        h.append(["referer", "https://%s%s" % (sess["authority"], rng.choice(PATHS))])
    if method == "POST":
        h.append(["content-type", "application/json"])
        h.append(["content-length", str(rng.randint(8, 4096))])
    return h


def resp_block(rng, sess):
    code = rng.choice(["200", "200", "200", "304", "404", "301", "500"])
    h = [
        [":status", code],
        ["server", sess["server"]],
        ["date", "Mon, 26 May 2026 %02d:%02d:%02d GMT" % (rng.randint(0, 23), rng.randint(0, 59), rng.randint(0, 59))],
        ["content-type", rng.choice(CONTENT_TYPES)],
        ["content-length", str(rng.randint(0, 65535))],
        ["cache-control", rng.choice(["no-cache", "max-age=3600", "public, max-age=86400", "private"])],
        ["vary", "Accept-Encoding"],
    ]
    if rng.random() < 0.4:
        h.append(["etag", '"%012x"' % rng.getrandbits(48)])
    if rng.random() < 0.2:
        h.append(["set-cookie", "session=%016x; Path=/; HttpOnly; Secure" % rng.getrandbits(64)])
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--blocks", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=1234)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    out = sys.stdout

    # a handful of long-lived sessions (connection reuse → repeated headers)
    sessions = []
    for _ in range(6):
        sessions.append({
            "authority": rng.choice(AUTHORITIES),
            "ua": rng.choice(UAS),
            "lang": rng.choice(["en-US,en;q=0.9", "ko-KR,ko;q=0.9,en;q=0.8", "en-GB,en;q=0.7"]),
            "cookie": "uid=%d; session=%016x; theme=%s" % (
                rng.randint(1000, 99999), rng.getrandbits(64), rng.choice(["light", "dark"])),
            "server": rng.choice(SERVERS),
        })

    for _ in range(args.blocks):
        sess = rng.choice(sessions)
        if rng.random() < 0.5:
            out.write(json.dumps({"type": "req", "headers": req_block(rng, sess)}) + "\n")
        else:
            out.write(json.dumps({"type": "resp", "headers": resp_block(rng, sess)}) + "\n")


if __name__ == "__main__":
    main()
