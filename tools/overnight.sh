#!/bin/sh
# Unattended overnight test loop. Alternates deliberate testing with stretches of
# ordinary live operation, so the run produces both fault coverage and a clean
# soak baseline instead of only stress data.
#
#   tools/overnight.sh &        # runs until killed
#
# tools/monitor.sh must already be running (serial + /healthz capture).
set -u
cd "$(dirname "$0")/.."
LOGDIR="${LOGDIR:-$(pwd)/logs}"
MOCK="${MOCK_HOST:-192.168.1.250}"
LIVE_MINUTES="${LIVE_MINUTES:-30}"
mkdir -p "$LOGDIR"
say() { echo "$(date -u +%FT%TZ) $*" >>"$LOGDIR/overnight.log"; }

say "=== overnight run starting ==="
# The mock must be up for the probe and fault passes.
pgrep -f mockapi.py >/dev/null 2>&1 || { ./tools/mockapi.py >>"$LOGDIR/mock.log" 2>&1 & sleep 3; }

pass=0
while :; do
  pass=$((pass + 1))
  say "--- pass $pass: data-path probe ---"
  ./tools/with-device-lock.sh ./tools/probe.py --mock-host "$MOCK" >>"$LOGDIR/overnight.log" 2>&1
  say "probe exit=$?"

  say "--- pass $pass: fault injection ---"
  ./tools/with-device-lock.sh ./tools/faults.py --mock-host "$MOCK" run >>"$LOGDIR/overnight.log" 2>&1
  say "faults exit=$?"

  say "--- pass $pass: display harness ---"
  ./tools/with-device-lock.sh ./tools/harness.py run --dwell 2 >>"$LOGDIR/overnight.log" 2>&1
  say "harness exit=$?"

  # Hand the device back to the real API so the soak keeps collecting genuine
  # traffic, heap and latency data between test bursts.
  ./tools/with-device-lock.sh ./tools/faults.py --mock-host "$MOCK" restore >>"$LOGDIR/overnight.log" 2>&1
  say "--- pass $pass done; live soak for ${LIVE_MINUTES}m ---"
  sleep $((LIVE_MINUTES * 60))
done
