#!/bin/bash
# Remove the DKMS btusb-csr-clone module and restore stock btusb.
#
#   sudo ./uninstall.sh
#
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
NAME=btusb-csr-clone
VER=1.0.0
KVER=$(uname -r)

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0"
	exit 1
fi

systemctl stop bluetooth 2>/dev/null || true
modprobe -r btusb 2>/dev/null || true

if dkms status -m "$NAME" 2>/dev/null | grep -q .; then
	dkms remove -m "$NAME" -v "$VER" --all || true
fi

rm -rf "/usr/src/${NAME}-${VER}"
rm -f /etc/modprobe.d/btusb-csr.conf

depmod -a "$KVER"
modprobe btusb 2>/dev/null || true
systemctl start bluetooth 2>/dev/null || true

echo "removed $NAME; stock btusb should load on next plug/reboot"
echo "btusb filename: $(modinfo -F filename btusb 2>/dev/null || echo unknown)"
