#!/bin/sh
# Boot the ISO headless in QEMU and wait for the desktop to come up.
#   tools/smoke-test.sh [iso] [timeout-seconds]
ISO="${1:-build/radiant-x64.iso}"
TIMEOUT="${2:-90}"
LOG="$(mktemp)"

qemu-system-x86_64 -m 256 -cdrom "$ISO" -display none -no-reboot -serial "file:$LOG" &
QEMU=$!

i=0
status=1
while [ "$i" -lt "$TIMEOUT" ]; do
    if grep -q "STOP: 0x" "$LOG" 2>/dev/null; then break; fi
    if grep -q "WIN32K: desktop ready" "$LOG" 2>/dev/null; then status=0; break; fi
    sleep 1
    i=$((i + 1))
done

kill "$QEMU" 2>/dev/null
wait "$QEMU" 2>/dev/null
echo "----- kernel log -----"
cat "$LOG"
echo "----------------------"
rm -f "$LOG"
if [ "$status" -eq 0 ]; then echo "PASS: RadiaNT booted to the desktop in ${i}s"; else echo "FAIL: no desktop after ${i}s"; fi
exit "$status"
