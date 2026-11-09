# Changelog

All notable changes to `btusb-csr-clone`. The driver source is a fork of the
kernel's `drivers/bluetooth/btusb.c`; this file tracks the local patch work.

## 1.0.0 — 2026-11-09

Initial release, packaged as DKMS.

### Driver changes (only for devices detected as fake CSR, upstream `btusb_setup_csr()` path)

* Skip the Barrot force-suspend workaround when the clone rejects USB suspend.
* Prefetch slow/broken opcodes (`Reset`, `Read Local Features`,
  `Read Buffer Size`) with a configurable timeout and cache the real payload.
* Answer locally the opcodes the firmware advertises but never completes
  (e.g. `Read Sync Train Parameters`).
* Filter stray event-code bytes on the interrupt endpoint and resync garbled
  inquiry/connection frames so pairing can finish.
* Hold runtime PM so the RX path does not go deaf (`reconstruct_records`,
  `suppress_dupes`, `csr_prefetch_timeout`, `csr_conn_probe`, `log_events`).

### Packaging

* `dkms.conf` + `Makefile` for out-of-tree/DKMS builds, module installed under
  `/lib/modules/$(uname -r)/updates/dkms/` without touching the distribution file.
* `install.sh` / `uninstall.sh` (DKMS lifecycle + `/etc/modprobe.d/btusb-csr.conf`).
* `scripts/`: `reload.sh`, `restore.sh`, `quiet.sh`, `trace.sh`, `conn-test.sh`.
* `conf/`: optional PipeWire / WirePlumber helpers for A2DP stability.
