#!/usr/bin/env bash
# One-command macOS install for Canon MF3110 CARPS driver
# Usage: ./install-macos.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Building ==="
make -f Makefile.macos build

echo ""
echo "=== Installing ==="
make -f Makefile.macos install

echo ""
echo "=== Status ==="
make -f Makefile.macos status

echo ""
echo "Done. Print a test page with: make -f Makefile.macos print-test"
echo "Or: echo 'test' | lp -d Canon_MF3110 -o PageSize=A4"