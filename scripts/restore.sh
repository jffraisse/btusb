#!/bin/bash
# Remove the DKMS patch and reload stock btusb.
# Prefer: sudo ./uninstall.sh
#
#   sudo ./scripts/restore.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
exec "$ROOT/uninstall.sh"
