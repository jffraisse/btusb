#!/bin/bash
# Turn off diagnostic spam on the already-loaded patched btusb.
#
#   sudo ./scripts/quiet.sh
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0"
	exit 1
fi

[ -d /sys/module/btusb/parameters ] || { echo "btusb not loaded"; exit 1; }

for p in csr_conn_probe log_events; do
	[ -f "/sys/module/btusb/parameters/$p" ] || continue
	echo 0 > "/sys/module/btusb/parameters/$p"
	echo "$p=$(cat /sys/module/btusb/parameters/$p)"
done
