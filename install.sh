#!/bin/bash
# Install btusb-csr-clone via DKMS (preferred). The module lands in
# /lib/modules/<kver>/updates/dkms/ and takes precedence over stock btusb
# without overwriting the distribution file.
#
#   sudo ./install.sh
#
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
NAME=btusb-csr-clone
VER=1.0.0
DEST=/usr/src/${NAME}-${VER}
KVER=$(uname -r)

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0"
	exit 1
fi

if ! command -v dkms >/dev/null; then
	echo "dkms not found; install it first (e.g. apt install dkms / dnf install dkms)"
	exit 1
fi

if [ ! -e "/lib/modules/$KVER/build/Makefile" ] && [ ! -d "/usr/src/linux-headers-$KVER" ]; then
	echo "missing kernel build tree for $KVER (install linux-headers / kernel-devel)"
	exit 1
fi

# Drop a previous revision of this package
if dkms status -m "$NAME" 2>/dev/null | grep -q .; then
	dkms remove -m "$NAME" -v "$VER" --all 2>/dev/null || true
fi

rm -rf "$DEST"
mkdir -p "$DEST"
cp -a "$ROOT"/btusb.c "$ROOT"/Makefile "$ROOT"/dkms.conf \
	"$ROOT"/btbcm.h "$ROOT"/btintel.h "$ROOT"/btmtk.h "$ROOT"/btrtl.h \
	"$DEST/"

dkms add -m "$NAME" -v "$VER"
dkms build -m "$NAME" -v "$VER" -k "$KVER"
dkms install -m "$NAME" -v "$VER" -k "$KVER"

install -m 0644 "$ROOT/conf/btusb-csr.conf" /etc/modprobe.d/btusb-csr.conf

echo
dkms status -m "$NAME"
echo "btusb filename: $(modinfo -F filename btusb 2>/dev/null || echo unknown)"
echo
echo "Reload without reboot (after a real unplug/replug of the dongle if it was wedged):"
echo "  sudo $ROOT/scripts/reload.sh"
echo "Or: sudo modprobe -r btusb && sudo modprobe btusb"
