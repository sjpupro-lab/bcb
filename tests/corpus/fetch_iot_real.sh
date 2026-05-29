#!/bin/sh
# BCB — real IoT/sensor telemetry corpus (Intel Berkeley Lab sensor data).
#
# Source: Intel Lab Data — 54 motes, ~2.3M readings of temperature/humidity/
# light/voltage (2004), a classic public sensor-network research dataset.
#   http://db.csail.mit.edu/labdata/labdata.html  (publicly available for research)
#
# We emit TWO real encodings of the same readings, ordered per device
# (moteid, epoch) — each device's own ordered telemetry stream, the realistic
# single-device case BCB's structural codec targets:
#
#   build/real_iot.corpus       float32 wire format, 22-byte records:
#       epoch:u32 moteid:u16 temperature:f32 humidity:f32 light:f32 voltage:f32
#   build/real_iot_int.corpus   quantized-integer wire format, 10-byte records:
#       moteid:u16 temp_x100:i16 hum_x100:i16 light:u16 volt_x1000:u16
#       (how real Modbus/CAN/sensor packets actually transmit: scaled integers)
#
# The contrast matters: float32 mantissas are near-random (poor for any byte
# compressor), while quantized integers expose the slow per-field drift that
# position-delta modeling exploits. docs/benchmarks_real.md reports both.
#
# License/redistribution: the dataset is provided for research use by its authors;
# we do NOT commit the data, only this fetch+pack script. Re-run to reproduce.
set -e
cd "$(dirname "$0")/../.."
OUT=build; mkdir -p "$OUT"
TMP=$(mktemp -d)
URL="http://db.csail.mit.edu/labdata/data.txt.gz"
MAXREC="${MAXREC:-300000}"   # cap records for a manageable corpus (~6.6MB)

echo "fetching $URL ..."
for a in 1 2 3; do
  if curl -sSL -m 120 "$URL" -o "$TMP/data.txt.gz" && [ -s "$TMP/data.txt.gz" ]; then break; fi
  sleep $((a*3))
done
[ -s "$TMP/data.txt.gz" ] || { echo "fetch failed" >&2; rm -rf "$TMP"; exit 1; }
gunzip -f "$TMP/data.txt.gz"

python3 - "$TMP/data.txt" "$OUT/real_iot.corpus" "$OUT/real_iot_int.corpus" "$MAXREC" <<'PY'
import struct, sys
src, dst_f, dst_i, maxrec = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
recs = []
with open(src, "r", errors="ignore") as f:
    for line in f:
        t = line.split()
        if len(t) != 8:
            continue
        try:
            epoch = int(t[2]); mote = int(t[3])
            temp = float(t[4]); hum = float(t[5]); light = float(t[6]); volt = float(t[7])
        except ValueError:
            continue
        if not (0 < mote < 65536 and 0 <= epoch < 2**32):
            continue
        recs.append((mote, epoch, temp, hum, light, volt))
# per-device ordered stream: sort by (moteid, epoch)
recs.sort(key=lambda r: (r[0], r[1]))
if len(recs) > maxrec:
    recs = recs[:maxrec]

def clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)

with open(dst_f, "wb") as of, open(dst_i, "wb") as oi:
    for mote, epoch, temp, hum, light, volt in recs:
        of.write(struct.pack("<IHffff", epoch & 0xffffffff, mote, temp, hum, light, volt))
        oi.write(struct.pack("<HhhHH", mote,
                             int(clamp(round(temp*100),  -32768, 32767)),
                             int(clamp(round(hum*100),   -32768, 32767)),
                             int(clamp(round(light),          0, 65535)),
                             int(clamp(round(volt*1000),      0, 65535))))
print(f"real_iot:     {len(recs)} records x 22B = {len(recs)*22} bytes -> {dst_f}")
print(f"real_iot_int: {len(recs)} records x 10B = {len(recs)*10} bytes -> {dst_i}")
PY
rm -rf "$TMP"
