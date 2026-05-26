#!/bin/sh
# BCB — large corpus 조립 (대규모 학습 측정용). 결과 large.txt 는 git 에 안 올림(.gitignore).
# 기존 4권 + Project Gutenberg 4권을 이어붙여 ~11MB diverse 텍스트 생성.
set -e
cd "$(dirname "$0")"
TMP=$(mktemp -d)
fetch() { # id name
  for a in 1 2 3; do
    if curl -sL "https://www.gutenberg.org/cache/epub/$1/pg$1.txt" -o "$TMP/$2.txt" && [ -s "$TMP/$2.txt" ]; then return 0; fi
    sleep $((a*2))
  done
  echo "fetch failed: $2" >&2; return 1
}
fetch 2600 war_and_peace
fetch 1399 anna_karenina
fetch 2554 crime_and_punishment
fetch 996  don_quixote
cat pride_and_prejudice.txt frankenstein.txt alice_in_wonderland.txt moby_dick.txt \
    "$TMP/war_and_peace.txt" "$TMP/anna_karenina.txt" "$TMP/crime_and_punishment.txt" "$TMP/don_quixote.txt" \
    > large.txt
rm -rf "$TMP"
echo "large.txt: $(wc -c < large.txt) bytes"
