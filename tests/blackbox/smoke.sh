#!/bin/sh
# Black-box smoke + golden-snapshot test of the codicis binary.
#
# Launches the real codicis executable with a recording-shim storage helper,
# drives a fixed HTTP scenario, and asserts:
#   - the binary boots and logs its bound port (startup/wiring),
#   - it serves the REST requests (200s),
#   - it shuts down cleanly on SIGTERM (exit 0), and
#   - the storage wire traffic it emitted matches a committed golden snapshot
#     (the codicis->helper protocol regression guard).
#
# Volatile fields (correlation ids, UUIDs) are normalized before the diff.
# Re-bless the golden with UPDATE_SNAPSHOTS=1.
#
# Required env (set by ctest):
#   CODICIS_BIN             path to the codicis executable
#   CODICIS_STORAGE_HELPER  path to the reference storage helper
#   BLACKBOX_DIR            this test's source dir (holds record-helper.sh + golden/)
set -eu

BIN="${CODICIS_BIN:?CODICIS_BIN not set}"
HELPER="${CODICIS_STORAGE_HELPER:?CODICIS_STORAGE_HELPER not set}"
DIR="${BLACKBOX_DIR:?BLACKBOX_DIR not set}"
SHIM="$DIR/record-helper.sh"
GOLDEN="$DIR/golden/storage.txt"

# The scenario drives HTTP with curl; skip cleanly where it is unavailable so
# the test never breaks a minimal environment (it still runs wherever curl is).
if ! command -v curl >/dev/null 2>&1; then
  echo "SKIP: curl not available"
  exit 0
fi

WORK=$(mktemp -d)
CAP="$WORK/cap"
mkdir -p "$CAP"
PID=""
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

# --- launch on ephemeral ports, text codec, recording-shim storage helper -----
"$BIN" --net.http_port=0 --net.ws_port=0 \
       --storage.codec=text \
       --storage.helper_cmd="$SHIM $HELPER $CAP" \
       >"$WORK/out.log" 2>"$WORK/err.log" &
PID=$!

# --- discover the bound HTTP port from the startup log (stderr) ----------------
PORT=""
i=0
while [ "$i" -lt 100 ]; do
  PORT=$(sed -n 's/.*codicis listening: http 127\.0\.0\.1:\([0-9]*\).*/\1/p' \
    "$WORK/err.log" | head -1)
  [ -n "$PORT" ] && break
  kill -0 "$PID" 2>/dev/null || { echo "FAIL: codicis exited before listening"; cat "$WORK/err.log"; exit 1; }
  i=$((i + 1)); sleep 0.1
done
[ -n "$PORT" ] || { echo "FAIL: no listening port in log"; cat "$WORK/err.log"; exit 1; }
echo "codicis http port: $PORT"

# --- deterministic scenario: two non-crossing resting orders ------------------
submit() {
  curl -s -o /dev/null -w '%{http_code}' -X POST \
    "http://127.0.0.1:$PORT/orders" --data "$1"
}
c1=$(submit "symbol=BTC&side=sell&type=limit&price=105&qty=8")
[ "$c1" = 200 ] || { echo "FAIL: submit 1 returned HTTP $c1"; exit 1; }
c2=$(submit "symbol=BTC&side=buy&type=limit&price=101&qty=3")
[ "$c2" = 200 ] || { echo "FAIL: submit 2 returned HTTP $c2"; exit 1; }
echo "REST submits accepted (200)"

# --- let the storage traffic settle, then shut down cleanly -------------------
prev=-1
i=0
while [ "$i" -lt 50 ]; do
  cur=$(wc -c <"$CAP/to-storage.txt" 2>/dev/null || echo 0)
  [ "$cur" = "$prev" ] && [ "$cur" -gt 0 ] && break
  prev=$cur; i=$((i + 1)); sleep 0.1
done

# SIGTERM must drain and exit cleanly (0), not be killed (143).
kill -TERM "$PID" 2>/dev/null || true
rc=0
wait "$PID" || rc=$?
PID=""
[ "$rc" = 0 ] || { echo "FAIL: codicis did not shut down cleanly on SIGTERM (rc=$rc)"; cat "$WORK/err.log"; exit 1; }
echo "clean shutdown on SIGTERM (exit 0)"

# --- normalize volatile fields, then compare / bless the golden ---------------
# Normalize the capture into a deterministic snapshot:
#   - drop `type=commit` records: those are emitted by the periodic commit timer,
#     so whether/when they appear depends on wall-clock timing, not on the
#     request sequence we are pinning;
#   - redact volatile fields (correlation ids, UUIDs).
normalize() {
  awk 'BEGIN { RS = ""; ORS = "\n\n" } !/(^|\n)type=commit(\n|$)/' "$1" \
    | sed -E \
        -e 's/^req_id=[0-9]+/req_id=<id>/' \
        -e 's/[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}/<uuid>/g'
}
normalize "$CAP/to-storage.txt" >"$WORK/norm.txt"

if [ ! -s "$WORK/norm.txt" ]; then
  echo "FAIL: captured no storage traffic"; exit 1
fi

mkdir -p "$(dirname "$GOLDEN")"
if [ "${UPDATE_SNAPSHOTS:-0}" = 1 ] || [ ! -f "$GOLDEN" ]; then
  cp "$WORK/norm.txt" "$GOLDEN"
  echo "blessed golden: $GOLDEN"
  exit 0
fi

if diff -u "$GOLDEN" "$WORK/norm.txt"; then
  echo "PASS: storage wire traffic matches golden"
else
  echo "FAIL: storage wire traffic differs from golden (UPDATE_SNAPSHOTS=1 to re-bless)"
  exit 1
fi
