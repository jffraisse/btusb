#!/bin/bash
# Connection test for the CSR clone. Run AFTER reload.sh, as the normal user
# (no sudo needed: it only talks to bluez and reads the journal).
#
# What this measures, in order:
#   1. whether bluez has an adapter for hci0 at all - without one, "pair" can
#      never reach the controller and the failure is not the clone's fault;
#   2. whether the controller is powered/connectable, because bluez only sends a
#      connection command after mgmt Set Connectable succeeds;
#   3. whether an HCI connection command is issued at all, and which one:
#      0x0405 Create Connection (BR/EDR), 0x200d LE Create Connection,
#      0x2043 LE Extended Create Connection, 0x200e LE Create Connection Cancel,
#      0x041b Read Remote Features (the command whose answer is event 0x0b, which
#      the old event whitelist dropped - the cause of the InProgress failure);
#   4. whether the events that pairing needs actually reach the core, which is
#      what the new csr_event_code_ok() whitelist and csr_log_conn_provenance()
#      show: a provenance line says whether an event names the address a
#      connection command was sent to. An event naming an address that was never
#      a command target cannot be a real connection.
#      The two feature events (0x0b answers 0x041b, 0x23 answers 0x041c) name a
#      connection handle, not an address, so they are compared with the handle the
#      connection complete reported. Their line also prints the time between the
#      command and the answer: a baseband transaction with a peer takes tens of
#      milliseconds, so an answer inside 5 ms is a canned one, not the peer's reply.
#   4b. the frames whose plen does not match their event code are the clone's buffer,
#      read as if its first record were the whole frame. csr_decode_garbled_conn_frame()
#      prints the whole payload and looks for the opcode of the command that is still
#      pending inside it. Read as records, the four identical frames seen on 2026-11-07
#      (13:24:38, 13:25:16, 13:26:20, 13:57:04) are:
#        03 81 | 00 05                       event 0x03, length byte 0x81, status 0x00
#        0f 04 | 00 01 19 04                 Command Status, status 0x00, opcode 0x0419
#        07 ff | 0c 1d fa d9 e1 f5 a8        event 0x07, length byte 0xff, status 0x0c,
#                                            then the peer address and no name bytes
#      So the clone does answer 0x0419, with status 0x00, and writes the buffer size where
#      the record length belongs. csr_reconstruct_record() rebuilds the one record that is
#      complete (the Command Status) and hands it to the core when reconstruct_records is
#      on, so the caller finishes with status 0x00 instead of the length error. 0x0419
#      HCI_OP_REMOTE_NAME_REQ is the command pairing stalls on, so it is counted below.
#   5. the probe: with the module loaded with csr_conn_probe=1, 0x2043 is sent to
#      02:11:22:33:44:55 during setup. No device can have that address, so any
#      event naming it is invented by the firmware.

set -e

# The device GNOME pairs is A8:F5:E1:D9:FA:1D. CC:22:37:11:22:9A (Terma Wireless)
# is the other Shokz identity seen during discovery, so pass it as an argument.
TARGET=${1:-A8:F5:E1:D9:FA:1D}
GREP='RX [0-9]+ ms|TX [0-9]+ ms|CSR|cache|send_frame|probe|unexpected|timeout|length|failed|dropping|continuation|unknown event|prefetch|cmd_cnt|req_status|handing|set config|reply for'
JOURNAL='journalctl -k -b -q'

echo "== module =="
echo "loaded srcversion: $(cat /sys/module/btusb/srcversion 2>/dev/null || echo none)"
echo "suppress_dupes : $(cat /sys/module/btusb/parameters/suppress_dupes 2>/dev/null || echo 'no such parameter')"
echo "log_events     : $(cat /sys/module/btusb/parameters/log_events 2>/dev/null || echo 'no such parameter')"
echo "csr_conn_probe : $(cat /sys/module/btusb/parameters/csr_conn_probe 2>/dev/null || echo 'no such parameter')"
echo "reconstruct_records : $(cat /sys/module/btusb/parameters/reconstruct_records 2>/dev/null || echo 'no such parameter')"

echo
echo "== is there an adapter? =="
if busctl tree org.bluez 2>/dev/null | grep -q hci0; then
	echo "bluez exposes /org/bluez/hci0"
else
	echo "bluez has NO adapter for hci0 - pairing cannot reach the controller."
	echo "Start bluetoothd (systemctl start bluetooth) and re-run this script."
fi
hciconfig -a hci0 2>/dev/null || true

echo
echo "== scan (15 s) =="
timeout 15 bluetoothctl scan on >/dev/null 2>&1 || true

echo
echo "== pair, then connect =="
timeout 30 bluetoothctl pair "$TARGET" 2>&1 || true
timeout 30 bluetoothctl connect "$TARGET" 2>&1 || true

echo
echo "== kernel trace for the attempt =="
$JOURNAL --since '3 min ago' | grep -E "$GREP" | tail -60 || true

echo
echo "== was a connection command ever sent? =="
for op in 0x0405 0x200d 0x2043 0x200e 0x041b 0x041c 0x0419; do
	printf 'cmd %s : %d\n' "$op" "$($JOURNAL | grep -c "cmd $op")"
done

echo
echo "== decoded events for this attempt =="
$JOURNAL --since '3 min ago' | grep 'CSR clone: event 0x' || true
echo
echo "connection event dumps this boot : $($JOURNAL | grep -c 'CSR conn event')"
echo "provenance lines this boot         : $($JOURNAL | grep -c 'CSR clone: event 0x')"
echo "  naming the command target        : $($JOURNAL | grep -c 'matches the command target')"
echo "  naming something else            : $($JOURNAL | grep -c 'does NOT match')"
echo "  probe address named (fabricated) : $($JOURNAL | grep -c 'PROBE ADDRESS')"
echo "  handle provenance (0x0b / 0x23)  : $($JOURNAL | grep -c 'handle 0x')"
echo "    handle is the connection made  : $($JOURNAL | grep -c 'handle is the connection made')"
echo "    answered too fast to be real   : $($JOURNAL | grep -c 'canned answer')"
echo "  garbled frames dropped           : $($JOURNAL | grep -c 'garbled frame for event')"
echo "    echoing the pending command    : $($JOURNAL | grep -c 'echoes cmd')"
echo "    naming no command              : $($JOURNAL | grep -c 'names no command')"
echo "    echoing a different command    : $($JOURNAL | grep -c 'does not echo the last command')"
echo "    records rebuilt and handed over: $($JOURNAL | grep -c 'rebuilding record')"
echo "  duplicate connection complete    : $($JOURNAL | grep -c 'was already delivered')"
echo "reply-vs-request lines this boot   : $($JOURNAL | grep -c 'reply for')"
echo "stray event codes dropped this boot: $($JOURNAL | grep -c 'dropping stray event code')"
echo "acl packets this boot              : $(hciconfig hci0 2>/dev/null | grep -o 'acl:[0-9]*' || echo 'n/a')"
