#!/usr/bin/env python3
"""http2_real.py — extract HTTP header blocks from a real HAR capture.

A HAR file (HTTP Archive, what browsers' devtools "Save all as HAR" produces)
contains every request/response with full headers. This converts it to the same
JSON-lines block format as http2_headers.py:
    {"type": "req"|"resp", "headers": [["name","value"], ...]}

so bcb_vs_hpack.py can measure BCB vs HPACK on *real* traffic.

Usage: http2_real.py <capture.har>        # writes blocks to stdout
       http2_real.py --help-data          # where to get a HAR

Pseudo-headers (:method/:scheme/:authority/:path/:status) are reconstructed
from the HAR request/response objects since HAR stores them out-of-band.
"""
import argparse
import json
import sys
from urllib.parse import urlsplit


def to_blocks(har):
    entries = har.get("log", {}).get("entries", [])
    for e in entries:
        req = e.get("request", {})
        url = req.get("url", "")
        parts = urlsplit(url)
        h = [
            [":method", req.get("method", "GET")],
            [":scheme", parts.scheme or "https"],
            [":authority", parts.netloc],
            [":path", (parts.path or "/") + (("?" + parts.query) if parts.query else "")],
        ]
        for hdr in req.get("headers", []):
            name = hdr.get("name", "").lower()
            if name.startswith(":") or name in ("host",):
                continue
            h.append([name, hdr.get("value", "")])
        yield {"type": "req", "headers": h}

        resp = e.get("response", {})
        rh = [[":status", str(resp.get("status", 200))]]
        for hdr in resp.get("headers", []):
            name = hdr.get("name", "").lower()
            if name.startswith(":"):
                continue
            rh.append([name, hdr.get("value", "")])
        yield {"type": "resp", "headers": rh}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("har", nargs="?", help="path to a .har capture")
    ap.add_argument("--help-data", action="store_true")
    args = ap.parse_args()

    if args.help_data or not args.har:
        sys.stderr.write(
            "Provide a HAR capture: in Chrome/Firefox devtools → Network tab →\n"
            "right-click → 'Save all as HAR'. Then:\n"
            "  python3 tests/scenarios/http2_real.py capture.har > blocks.jsonl\n"
            "  python3 tools/bcb_vs_hpack.py --blocks-file blocks.jsonl\n"
            "No HAR on hand? Use the synthetic generator instead:\n"
            "  python3 tests/scenarios/http2_headers.py > blocks.jsonl\n")
        return 0 if args.help_data else 2

    with open(args.har, encoding="utf-8") as f:
        har = json.load(f)
    out = sys.stdout
    n = 0
    for blk in to_blocks(har):
        out.write(json.dumps(blk) + "\n")
        n += 1
    sys.stderr.write("extracted %d header blocks from %s\n" % (n, args.har))
    return 0


if __name__ == "__main__":
    sys.exit(main())
