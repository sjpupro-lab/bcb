#!/bin/sh
# BCB — real HTTP/2 header corpus (live capture from real servers).
#
# Captures the *actual* response header block from each of a diverse list of
# real public endpoints (CDNs, APIs, news, clouds). These are genuine HTTP/2
# responses — not a synthetic generator — so docs/benchmarks_real.md can report
# BCB vs brotli/zstd/gzip on real traffic.
#
# Output (gitignored; regenerate any time):
#   build/real_http.corpus   concatenated header blocks (bytes)
#   build/real_http.index    one byte-length per block (message boundaries)
#
# NOTE on representativeness/limits (documented honestly in the report):
#  - These are *response* header blocks (server output). True browser request
#    streams (HPACK dynamic table over one connection) would need a browser HAR;
#    use tests/scenarios/http2_real.py with a real .har for that case.
#  - Output varies run-to-run (servers change). The corpus is therefore NOT
#    committed; only this script is. Re-run to reproduce.
set -e
cd "$(dirname "$0")/../.."
OUT=build; mkdir -p "$OUT"
CORPUS="$OUT/real_http.corpus"; INDEX="$OUT/real_http.index"
: > "$CORPUS"; : > "$INDEX"
TMP=$(mktemp)

# Diverse real endpoints. Mix of CDNs/APIs/sites for varied real header shapes.
DOMAINS="
https://www.wikipedia.org https://en.wikipedia.org/wiki/HTTP
https://www.cloudflare.com https://developers.cloudflare.com
https://api.github.com https://github.com https://raw.githubusercontent.com/torvalds/linux/master/README
https://www.python.org https://pypi.org https://docs.python.org/3/
https://www.mozilla.org https://developer.mozilla.org
https://www.bbc.com https://www.bbc.co.uk/news https://www.theguardian.com
https://www.nytimes.com https://www.reuters.com https://apnews.com
https://www.google.com https://www.youtube.com https://www.bing.com
https://www.amazon.com https://aws.amazon.com https://azure.microsoft.com
https://www.microsoft.com https://learn.microsoft.com https://www.apple.com
https://www.npmjs.com https://registry.npmjs.org https://crates.io
https://www.rust-lang.org https://go.dev https://nodejs.org
https://www.debian.org https://www.ubuntu.com https://www.kernel.org
https://www.djangoproject.com https://flask.palletsprojects.com
https://www.nginx.com https://httpd.apache.org https://redis.io
https://www.postgresql.org https://www.mysql.com https://www.mongodb.com
https://www.docker.com https://kubernetes.io https://grafana.com
https://stackoverflow.com https://news.ycombinator.com https://www.reddit.com
https://www.cnn.com https://www.wsj.com https://www.bloomberg.com
https://www.imdb.com https://www.wikipedia.de https://fr.wikipedia.org
https://www.baidu.com https://www.yahoo.com https://duckduckgo.com
https://www.gnu.org https://www.fsf.org https://archive.org
https://www.w3.org https://whatwg.org https://datatracker.ietf.org
https://letsencrypt.org https://www.openssl.org https://curl.se
https://www.gitlab.com https://about.gitlab.com https://bitbucket.org
https://www.heroku.com https://vercel.com https://netlify.com
https://www.digitalocean.com https://www.linode.com https://fastly.com
https://www.akamai.com https://www.shopify.com https://stripe.com
https://www.paypal.com https://www.salesforce.com https://slack.com
https://www.atlassian.com https://www.jetbrains.com https://code.visualstudio.com
"

n=0
for url in $DOMAINS; do
  # -sS quiet, -D - dump response headers to stdout, -o discard body, follow no redirects
  if curl -sS -m 8 -D "$TMP" -o /dev/null "$url" >/dev/null 2>&1 && [ -s "$TMP" ]; then
    sz=$(wc -c < "$TMP" | tr -d ' ')
    [ "$sz" -ge 16 ] || continue
    cat "$TMP" >> "$CORPUS"
    echo "$sz" >> "$INDEX"
    n=$((n+1))
  fi
done
rm -f "$TMP"
echo "real_http: $n blocks, $(wc -c < "$CORPUS" | tr -d ' ') bytes -> $CORPUS (+ $INDEX)"
[ "$n" -ge 20 ] || { echo "WARN: too few blocks captured ($n); network restricted?" >&2; exit 1; }
