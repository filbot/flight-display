#!/bin/sh
# Serialise everything that drives the device. probe/faults/harness all stage
# payloads into the SAME logs/mock.body and all switch the device's api_base, so
# two of them at once silently read each other's payloads — which is exactly how
# a clean firmware build produced 19 bogus "failures" on 2026-08-23.
#   tools/with-device-lock.sh ./tools/probe.py --mock-host 1.2.3.4
set -eu
LOCKDIR="${LOGDIR:-$(cd "$(dirname "$0")/.." && pwd)/logs}/device.lock"
if ! mkdir "$LOCKDIR" 2>/dev/null; then
  echo "device busy: $(cat "$LOCKDIR/owner" 2>/dev/null || echo unknown) holds the lock" >&2
  exit 3
fi
echo "$$ $*" > "$LOCKDIR/owner"
trap 'rm -rf "$LOCKDIR"' EXIT INT TERM
"$@"
