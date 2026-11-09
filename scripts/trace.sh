#!/bin/bash
# Trace-only: assume the patched module is already loaded and the dongle was
# just unplugged/replugged. Does not rmmod.
#
#   sudo ./scripts/trace.sh
set -euo pipefail

GREP='RX [0-9]+ ms|TX [0-9]+ ms|CSR|cache|send_frame|probe|unexpected|timeout|length|failed|dropping|continuation|unknown event|prefetch|cmd_cnt|req_status|handing|set config'

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo $0"
	exit 1
fi

find_dev() {
	if [ -e /sys/class/bluetooth/hci0/device ]; then
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

if [ -z "$DEV" ] || [ ! -e "/sys/bus/usb/devices/$DEV" ]; then
	echo "device gone — plug it back in and run again"
	exit 1
fi
if [ ! -e "/sys/bus/usb/devices/$DEV:1.0" ]; then
	echo "$DEV present but no interface bound (config #1 failed). Power-cycle it."
	exit 1
fi

echo "== module =="
echo "loaded srcversion: $(cat /sys/module/btusb/srcversion 2>/dev/null || echo none)"
echo "filename: $(modinfo -F filename btusb 2>/dev/null || echo unknown)"
for p in csr_prefetch_timeout csr_conn_probe log_events; do
	[ -f "/sys/module/btusb/parameters/$p" ] && echo "$p: $(cat /sys/module/btusb/parameters/$p)"
done
for i in "$DEV:1.0" "$DEV:1.1"; do
	[ -e "/sys/bus/usb/devices/$i" ] || continue
	drv=$(readlink "/sys/bus/usb/devices/$i/driver" 2>/dev/null || true)
	echo "driver of $i : ${drv:-none}"
done

echo "== open hci0 =="
systemctl stop bluetooth 2>/dev/null || true
systemctl start bluetooth
for _ in $(seq 1 20); do
	journalctl -k -b -q --since '1 min ago' | grep -q 'setup exit' && break
	sleep 1
done

echo "== trace =="
journalctl -k -b -q --since '1 min ago' | grep -E "$GREP" || true

echo "== state =="
hciconfig hci0 2>/dev/null || bluetoothctl show 2>/dev/null || true
timeout 12 bluetoothctl scan on >/dev/null 2>&1 || true
journalctl -k -b -q --since '1 min ago' | grep -E "$GREP" | tail -20 || true
