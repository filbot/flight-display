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

# Kill our children on the way out. Without this, `pkill -f monitor.sh` reaps the
# wrapper but ORPHANS the `cat` reading the serial port. A leftover reader then
# competes with the next run for the same tty, the two split the byte stream
# between them, and BOTH captures come out corrupted — characters missing
# mid-line, which then fails log-reading tests for reasons that look like
# firmware faults.
trap 'kill 0 2>/dev/null' EXIT INT TERM

# Refuse to start on top of a stale reader.
stale=$(ps -ax -o pid=,command= | grep 'cat <&3' | grep -F "$PORT" | grep -v grep | awk '{print $1}')
if [ -n "$stale" ]; then
  echo "monitor: killing stale serial reader(s): $stale" >&2
  kill $stale 2>/dev/null || true
  sleep 1
fi

log_serial() {
  # `arduino-cli monitor` exits the moment stdin isn't a TTY, so it's useless
  # detached. Read the port directly instead: hold the fd open with exec so the
  # stty settings stick (each fresh open resets them back to 9600 on macOS).
  # Opening the port toggles DTR and reboots the ESP32 — expect one reset here.
  # Non-printable bytes (boot ROM output at a different baud) are squashed to '.'
  # so the log stays plain text; otherwise grep treats it as binary and silently
  # prints nothing for patterns that are really there.
  # chomp FIRST: the sanitising class excludes \n, so without it the newline
  # itself was replaced by '.', collapsing the whole log into one run-on line
  # with timestamps injected mid-stream. That looked exactly like dropped bytes.
  # One perl process does timestamping AND sanitising, line-buffered.
  # The previous pipeline was `tr | while read ... $(date)`, which had two
  # faults: tr block-buffers into a pipe, so lines arrived in 4 KB batches all
  # stamped with the flush time; and forking date(1) per line was too slow to
  # drain the port, so the serial FIFO overflowed and silently DROPPED BYTES.
  # Corrupted lines then broke tests that read this log.
  # Retry forever so a device reboot or USB re-enumeration can't end the capture.
  while :; do
    echo "$(date -u +%FT%TZ) [monitor] attaching to $PORT" >>"$LOGDIR/serial.log"
    sh -c 'exec 3<>"$1"; stty -f "$1" 115200 raw -echo; cat <&3' _ "$PORT" 2>/dev/null \
      | perl -MPOSIX -ne 'BEGIN { $| = 1 } chomp; s/[^\x09\x20-\x7e]/./g; print POSIX::strftime("%Y-%m-%dT%H:%M:%SZ", gmtime), " ", $_, "\n"' \
      >>"$LOGDIR/serial.log"
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
