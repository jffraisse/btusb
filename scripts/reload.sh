#!/bin/bash
# Reload btusb and bind the CSR/Barrot clone. Prefer the DKMS-installed module;
# fall back to a local out-of-tree build for development.
#
#   sudo ./scripts/reload.sh
#   sudo CSR_DEBUG=1 ./scripts/reload.sh
#
# If the dongle is wedged (USB config timeout), unplug/replug it first —
# toggling authorized is not a real power cycle on these clones.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KO="$ROOT/btusb.ko"
GREP='RX [0-9]+ ms|TX [0-9]+ ms|CSR|cache|send_frame|probe|unexpected|timeout|length|failed|dropping|continuation|unknown event|prefetch|cmd_cnt|req_status|handing|set config|event 0x|target'

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0"
	exit 1
fi

find_dev() {
	if [ -e /sys/class/bluetooth/hci0/device ]; then
		# hci0/device is the USB interface (…:1.0); PM attrs live on the device.
		readlink -f /sys/class/bluetooth/hci0/device | sed 's|.*/||' | sed 's|:.*||'
		return
	fi
	for d in /sys/bus/usb/devices/*; do
		if [ -f "$d/idVendor" ] && [ "$(cat "$d/idVendor")" = "0a12" ] \
			&& [ -f "$d/idProduct" ] && [ "$(cat "$d/idProduct")" = "0001" ]; then
			basename "$d"
			return
		fi
		if [ -f "$d/product" ] && grep -qi 'bt' "$d/product"; then
			basename "$d"
			return
		fi
	done
	echo ""
}

DEV=$(find_dev)
echo "device path: ${DEV:-unknown}"

# Prefer the module already registered with the running kernel (DKMS updates/).
use_insmod=0
if [ -f "$KO" ]; then
	use_insmod=1
elif [ ! -e "/lib/modules/$(uname -r)/updates/dkms/btusb.ko" ] \
	&& [ ! -e "/lib/modules/$(uname -r)/updates/dkms/btusb.ko.zst" ] \
	&& [ ! -e "/lib/modules/$(uname -r)/updates/dkms/btusb.ko.xz" ]; then
	echo "no local $KO and no DKMS btusb — run make or sudo ./install.sh first"
	exit 1
fi

# Block udev from racing a stock btusb back in while we switch.
CONF=/etc/modprobe.d/btusb-csr-reload.conf
echo 'install btusb /bin/true' > "$CONF"
trap 'rm -f "$CONF"' EXIT

echo "== 0. Current module =="
echo "loaded     : $(cat /sys/module/btusb/srcversion 2>/dev/null || echo none)"
if [ "$use_insmod" = 1 ]; then
	echo "local ko   : $(modinfo -F srcversion "$KO")"
else
	echo "dkms/file  : $(modinfo -F filename btusb 2>/dev/null || echo unknown)"
	echo "dkms/srcv  : $(modinfo -F srcversion btusb 2>/dev/null || echo unknown)"
fi

echo "== 1. Stop bluetoothd and unload btusb =="
systemctl stop bluetooth 2>/dev/null || true
for _ in $(seq 1 15); do
	lsmod | grep -q '^btusb ' || break
	modprobe -r btusb 2>/dev/null || sleep 2
done
if lsmod | grep -q '^btusb '; then
	echo "btusb could not be unloaded (refcount $(awk '$1=="btusb"{print $3}' /proc/modules))."
	exit 1
fi
echo "btusb unloaded"

echo "== 2. Load dependencies + patched btusb =="
for m in bluetooth btintel btbcm btrtl btmtk; do
	modprobe "$m" 2>/dev/null || echo "  $m: not loadable"
done

PARAMS=(csr_prefetch_timeout=5000)
if [ "${CSR_DEBUG:-0}" = 1 ]; then
	PARAMS+=(csr_conn_probe=1 log_events=1)
else
	PARAMS+=(csr_conn_probe=0 log_events=0)
fi

# Temporarily allow the real install while the blacklist trap is active.
rm -f "$CONF"
if [ "$use_insmod" = 1 ]; then
	insmod "$KO" "${PARAMS[@]}"
else
	modprobe btusb "${PARAMS[@]}"
fi
echo 'install btusb /bin/true' > "$CONF"

echo "loaded srcversion: $(cat /sys/module/btusb/srcversion)"
for p in csr_prefetch_timeout csr_conn_probe log_events; do
	[ -f "/sys/module/btusb/parameters/$p" ] && echo "$p: $(cat /sys/module/btusb/parameters/$p)"
done

echo "== 3. Bind USB interfaces =="
if [ -z "$DEV" ] || [ ! -e "/sys/bus/usb/devices/$DEV" ]; then
	echo "dongle path unknown or gone — unplug/replug, then re-run"
	exit 1
fi
if ! ls /sys/bus/usb/devices/ | grep -q "^$DEV:"; then
	echo "no interface for $DEV (config #1 never accepted). Unplug, wait, replug."
	exit 1
fi

bound=0
for i in "$DEV:1.0" "$DEV:1.1"; do
	[ -e "/sys/bus/usb/devices/$i" ] || continue
	drv=$(readlink -f "/sys/bus/usb/devices/$i/driver" 2>/dev/null || true)
	if [ "$(basename "${drv:-none}")" = btusb ]; then
		echo "driver of $i : btusb (already bound)"
		bound=1
		continue
	fi
	if echo "$i" > /sys/bus/usb/drivers/btusb/bind; then
		echo "driver of $i : btusb (bound)"
		bound=1
	else
		echo "could not bind $i — firmware not answering; power-cycle the dongle"
	fi
done
if [ "$bound" -eq 0 ]; then
	echo "nothing bound to btusb"
	exit 1
fi
sleep 2

echo "== 4. Runtime PM =="
for a in control autosuspend_delay_ms runtime_status; do
	[ -f "/sys/bus/usb/devices/$DEV/power/$a" ] && \
		echo "power/$a: $(cat /sys/bus/usb/devices/$DEV/power/$a)"
done

echo "== 5. Start bluetoothd =="
systemctl start bluetooth
sleep 8

echo "== 6. Recent kernel log =="
journalctl -k -b -q --since '2 min ago' | grep -E "$GREP" || true

echo "== 7. Adapter =="
hciconfig hci0 2>/dev/null || bluetoothctl show 2>/dev/null || true
