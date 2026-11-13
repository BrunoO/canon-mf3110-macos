#!/bin/bash
# Clean slate: restart all printer components without rebooting
# Usage: sudo bash ./tmp/clean-slate.sh
set -euo pipefail

PRINTER="Canon_MF3110"

echo "=== Cancelling all jobs ==="
cancel -a "$PRINTER" 2>/dev/null || true

echo "=== Killing lingering backend processes ==="
pkill -f mf3110usb 2>/dev/null || true
pkill -f "backend/usb" 2>/dev/null || true

echo "=== Clearing CUPS cache and temp files ==="
rm -f /private/var/spool/cups/cache/${PRINTER}*
rm -f /private/var/spool/cups/tmp/* 2>/dev/null || true

echo "=== Restarting CUPS ==="
launchctl stop org.cups.cupsd 2>/dev/null || true
sleep 2
launchctl start org.cups.cupsd 2>/dev/null || true
sleep 2

echo "=== Re-enabling queue ==="
cupsenable "$PRINTER" 2>/dev/null || true
cupsaccept "$PRINTER" 2>/dev/null || true

echo "=== Checking USB device ==="
if ioreg -p IOUSB -l -w0 2>/dev/null | grep -qi "MF3110"; then
    echo "MF3110 found on USB bus."
else
    echo "WARNING: MF3110 not found on USB bus. Re-plug the cable."
fi

echo "=== Done. Re-plug USB cable if printer not detected. ==="