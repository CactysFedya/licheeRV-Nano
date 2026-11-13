#!/bin/sh
# Experimental live H.265 passthrough from the *stock working* test_mmf mode 6.
# No patched MMF binary: stdout of netcat is the network socket, stdin is FIFO.
# Run on the Nano after a clean boot, with no other CSI/ISP users.
set -eu
PORT="${1:-8555}"
STOCK=/mnt/system/usr/bin/test_mmf
command -v nc >/dev/null 2>&1 || { echo "ERROR: nc/netcat is absent" >&2; exit 1; }
command -v mkfifo >/dev/null 2>&1 || { echo "ERROR: mkfifo is absent" >&2; exit 1; }
test -x "$STOCK" || { echo "ERROR: stock test_mmf missing: $STOCK" >&2; exit 1; }
case "$PORT" in *[!0-9]*|'') echo "Invalid port" >&2; exit 1 ;; esac
DIR="$(mktemp -d /tmp/nano-h265.XXXXXX)"
FIFO="$DIR/venc_stream.h265"
LOG="$DIR/camera.log"
mkfifo "$FIFO"
cd "$DIR"
camera_pid=
cleanup() {
  if [ -n "$camera_pid" ]; then
    kill "$camera_pid" 2>/dev/null || :
    wait "$camera_pid" 2>/dev/null || :
  fi
}
trap 'cleanup' EXIT
echo "[stock-h265] logs: $LOG"
echo "[stock-h265] using unmodified $STOCK, mode 6, FIFO (no file storage)"
"$STOCK" 6 >"$LOG" 2>&1 &
camera_pid=$!
echo "[stock-h265] starting TCP listener on 0.0.0.0:$PORT"
echo "[stock-h265] Mac: ffplay -f hevc tcp://BOARD_IP:$PORT"
# Keep server independent of FIFO open; netcat starts listening while cat
# waits until the stock encoder opens the named pipe after its first VI frame.
cat "$FIFO" | nc -l -p "$PORT"
rc=$?
echo "[stock-h265] socket closed (status=$rc)"
exit "$rc"
