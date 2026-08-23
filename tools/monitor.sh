#!/bin/sh
# Long-running soak monitor: serial console + /healthz telemetry, both appended
# to timestamped logs under logs/. Run it, leave it, then use report.py.
#
#   tools/monitor.sh            # both loggers
#   tools/monitor.sh serial     # serial only
#   tools/monitor.sh health     # health poll only
#
# Note: the serial logger owns the USB port. Stop it before flashing over USB
# (OTA to $DEVICE_HOST works fine while it runs).
set -eu

PORT="${PORT:-/dev/cu.usbserial-0001}"
HOST="${DEVICE_HOST:-192.168.1.229}"
INTERVAL="${INTERVAL:-60}"
LOGDIR="${LOGDIR:-$(cd "$(dirname "$0")/.." && pwd)/logs}"
mkdir -p "$LOGDIR"

log_serial() {
  # `arduino-cli monitor` exits the moment stdin isn't a TTY, so it's useless
  # detached. Read the port directly instead: hold the fd open with exec so the
  # stty settings stick (each fresh open resets them back to 9600 on macOS).
  # Opening the port toggles DTR and reboots the ESP32 — expect one reset here.
  # Retry forever so a device reboot or USB re-enumeration can't end the capture.
  while :; do
    echo "$(date -u +%FT%TZ) [monitor] attaching to $PORT" >>"$LOGDIR/serial.log"
    sh -c 'exec 3<>"$1"; stty -f "$1" 115200 raw -echo; cat <&3' _ "$PORT" 2>/dev/null \
      | while IFS= read -r line; do
          printf '%s %s\n' "$(date -u +%FT%TZ)" "$line"
        done >>"$LOGDIR/serial.log"
    echo "$(date -u +%FT%TZ) [monitor] serial dropped, retrying in 5s" >>"$LOGDIR/serial.log"
    sleep 5
  done
}

log_health() {
  while :; do
    ts=$(date -u +%FT%TZ)
    # -m 10: a hung device must record a miss, not stall the whole poll loop.
    body=$(curl -s -m 10 "http://$HOST/healthz" 2>/dev/null || true)
    case "$body" in
      '{'*) printf '{"ts":"%s","ok":true,"health":%s}\n' "$ts" "$body" ;;
      *)    printf '{"ts":"%s","ok":false,"error":"unreachable"}\n' "$ts" ;;
    esac >>"$LOGDIR/health.jsonl"
    sleep "$INTERVAL"
  done
}

case "${1:-both}" in
  serial) log_serial ;;
  health) log_health ;;
  both)   log_health & log_serial ;;
  *) echo "usage: $0 [serial|health|both]" >&2; exit 2 ;;
esac
