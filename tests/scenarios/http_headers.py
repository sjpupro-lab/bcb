#!/usr/bin/env python3
"""http_headers.py — synthetic HTTP/1.1 request header blocks.

Emits a deterministic byte stream of realistic HTTP/1.1 request headers to
stdout. Used as a shared-prior corpus + test messages for bcb-msgbench.

Usage: http_headers.py [--bytes N] [--seed S]
"""
import argparse
import random
import sys

METHODS = ["GET", "GET", "GET", "POST", "PUT", "DELETE", "HEAD"]
HOSTS = [
    "api.example.com", "www.example.com", "cdn.example.net",
    "shop.example.com", "auth.example.org", "img.example.io",
]
PATHS = [
    "/", "/index.html", "/api/v1/users", "/api/v1/orders/{id}",
    "/static/app.js", "/static/style.css", "/login", "/logout",
    "/search?q={q}", "/products/{id}", "/cart", "/checkout",
]
UAS = [
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15",
    "Mozilla/5.0 (X11; Linux x86_64; rv:121.0) Gecko/20100101 Firefox/121.0",
    "curl/8.4.0", "okhttp/4.12.0", "python-requests/2.31.0",
]
ACCEPTS = ["*/*", "text/html,application/xhtml+xml", "application/json"]
ENCODINGS = ["gzip, deflate, br", "gzip, deflate", "br"]
LANGS = ["en-US,en;q=0.9", "ko-KR,ko;q=0.9,en;q=0.8", "en-GB,en;q=0.7"]


def gen_block(rng):
    method = rng.choice(METHODS)
    path = rng.choice(PATHS).replace("{id}", str(rng.randint(1, 99999))) \
                            .replace("{q}", rng.choice(["shoes", "laptop", "book", "coffee"]))
    lines = [f"{method} {path} HTTP/1.1"]
    lines.append(f"Host: {rng.choice(HOSTS)}")
    lines.append(f"User-Agent: {rng.choice(UAS)}")
    lines.append(f"Accept: {rng.choice(ACCEPTS)}")
    lines.append(f"Accept-Encoding: {rng.choice(ENCODINGS)}")
    lines.append(f"Accept-Language: {rng.choice(LANGS)}")
    lines.append("Connection: keep-alive")
    if rng.random() < 0.6:
        lines.append(f"Cookie: session={rng.getrandbits(64):016x}; uid={rng.randint(1000, 99999)}")
    if method in ("POST", "PUT"):
        lines.append("Content-Type: application/json")
        lines.append(f"Content-Length: {rng.randint(10, 4096)}")
    return ("\r\n".join(lines) + "\r\n\r\n").encode("ascii")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bytes", type=int, default=262144)
    ap.add_argument("--seed", type=int, default=1234)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    out = sys.stdout.buffer
    written = 0
    while written < args.bytes:
        b = gen_block(rng)
        out.write(b)
        written += len(b)


if __name__ == "__main__":
    main()
