# btusb-csr-clone

![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue)
![Build type: DKMS](https://img.shields.io/badge/build-DKMS-informational)
![Tested kernel: 7.0.x](https://img.shields.io/badge/tested_kernel-7.0.x-green)
![Device: 0a12:0001](https://img.shields.io/badge/device-0a12%3A0001-orange)

Patched Linux **`btusb`** driver for cheap **CSR / Barrot USB Bluetooth clones**
that squat VID/PID `0a12:0001`, packaged as **DKMS** so it rebuilds on every
kernel update.

Tested on Ubuntu with kernel **7.0.x** against a dongle that reports:

| Field | Value |
| --- | --- |
| USB ID | `0a12:0001` |
| Product | `USB2.0-BT` |
| `bcdDevice` | `0x8891` |
| Manufacturer (HCI) | **2279** (Barrot Technology Limited) |
| HCI / LMP rev | `0x0087` |

> Looking for manufacturer `10` / LMP `0x22bb` / product `CSR8510 A10`?
> That variant is covered by
> [hhsnake/csr8510-fix](https://github.com/hhsnake/csr8510-fix).
> This tree targets the **Barrot 2279 / rev 0x0087** class that needs different
> workarounds (event framing, opcode prefetch, fake `Read Buffer Size`).

## Symptoms on stock `btusb`

```
Bluetooth: hci0: CSR: Unbranded CSR clone detected; ...
Bluetooth: hci0: command 0x1005 tx timeout
Bluetooth: hci0: Opcode 0x1005 failed: -110
```

or the adapter comes up but pairing stalls / audio glitches because the clone
emits stray interrupt bytes and never answers some HCI commands in time.

## What this patch does

Only devices already detected as **fake CSR** (same path as upstream
`btusb_setup_csr()`) are affected:

* skip the Barrot force-suspend workaround when the device rejects USB suspend
* **prefetch** slow/broken opcodes (`Reset`, `Read Local Features`,
  `Read Buffer Size`) with a longer timeout and cache the real payload
* locally answer opcodes the firmware advertises but never completes
  (e.g. `Read Sync Train Parameters`)
* filter stray event-code bytes on the interrupt endpoint and resync
  malformed inquiry / connection frames so pairing can finish
* hold runtime PM so the RX path does not go deaf

Optional PipeWire helper (A2DP-only, no HFP autoswitch) lives under
[`conf/`](conf/) and is documented in the install notes below.

## Install

### Ubuntu / Debian

```bash
sudo apt install git dkms linux-headers-$(uname -r) build-essential
git clone https://github.com/jffraisse/btusb.git
cd btusb-csr-clone
sudo ./install.sh
sudo ./scripts/reload.sh   # or unplug/replug the dongle
```

### Fedora

```bash
sudo dnf install git dkms kernel-devel-$(uname -r) make gcc
git clone https://github.com/jffraisse/btusb.git
cd btusb-csr-clone
sudo ./install.sh
```

### Arch / CachyOS

```bash
sudo pacman -S git dkms linux-headers base-devel
git clone https://github.com/jffraisse/btusb.git
cd btusb-csr-clone
sudo ./install.sh
```

The module is installed to `/lib/modules/$(uname -r)/updates/dkms/` (takes
precedence over stock; the distribution file is not overwritten) and is
rebuilt by DKMS on kernel upgrades.

Verify:

```bash
dkms status btusb-csr-clone
modinfo -F filename btusb    # should contain updates/dkms
modinfo -F parm btusb | grep csr_
```

Uninstall:

```bash
sudo ./uninstall.sh
```

## Layout

```
.
├── btusb.c                 # patched driver (from drivers/bluetooth/btusb.c)
├── bt*.h                   # local includes required by btusb.c
├── Makefile / dkms.conf
├── install.sh / uninstall.sh
├── conf/
│   ├── btusb-csr.conf      # → /etc/modprobe.d (quiet defaults)
│   ├── pipewire-*.conf     # optional A2DP stability
│   └── wireplumber-*.conf  # optional A2DP-only BlueZ
├── scripts/                # reload, restore, quiet, trace, conn-test
└── .github/                # issue / PR templates
```

## Troubleshooting

* **Module not taking precedence** — `modinfo -F filename btusb` must show
  `updates/dkms`. If it still shows the distribution path, DKMS did not build for
  the running kernel: `dkms status`, then `dkms autoinstall`.
* **Build fails** — the kernel build tree must be present for the *running*
  kernel (`linux-headers-$(uname -r)` / `kernel-devel-$(uname -r)`).
* **Secure Boot:** out-of-tree modules are unsigned. Either disable Secure Boot
  or sign `btusb.ko` with a MOK (`mokutil --import`).
* **Dongle on a hub / front-panel header** — clones are sensitive to USB jitter;
  use a root port. `usbcore.autosuspend=-1` also helps when the RX path goes deaf.
* **Wedged dongle** — a `set config timeout` is not fixed by reloading the module;
  physically unplug/replug, then `sudo ./scripts/reload.sh`.
* **Audio dropouts only under browsers** — small PipeWire quantums force HFP
  switching; see [`conf/`](conf/) and keep HFP off.
* **Other clone variants** — manufacturer `10` / LMP `0x22bb` is a different
  class, see [hhsnake/csr8510-fix](https://github.com/hhsnake/csr8510-fix).

## Diagnostic output

`scripts/trace.sh` prints the per-command/per-event timings that the patch
relies on; that output is what to paste in an issue.

## Module parameters

| Parameter | Default | Meaning |
| --- | --- | --- |
| `csr_prefetch_timeout` | `5000` | ms to wait for prefetch answers |
| `csr_conn_probe` | `0` | diagnostic fake LE create-conn (debug only) |
| `log_events` | `0` | verbose CSR event dumps |
| `reconstruct_records` | `1` | rebuild garbled connection frames when possible |
| `suppress_dupes` | `1` | drop duplicate connection completes |

Debug load:

```bash
sudo CSR_DEBUG=1 ./scripts/reload.sh
```

## Audio (PipeWire / WirePlumber)

If A2DP works but stutters, prefer a **root-port USB** connection (not a
front-panel / onboard hub), keep HFP off, and optionally:

```bash
mkdir -p ~/.config/wireplumber/wireplumber.conf.d \
         ~/.config/pipewire/pipewire.conf.d
cp conf/wireplumber-51-bluez-csr-clone.conf \
   ~/.config/wireplumber/wireplumber.conf.d/
cp conf/pipewire-51-bt-stable.conf \
   ~/.config/pipewire/pipewire.conf.d/
systemctl --user restart wireplumber pipewire pipewire-pulse
```

## License

**GPL-2.0-or-later** (same as the Linux kernel). `btusb.c` is derived from
kernel sources — see [LICENSE](LICENSE) and the SPDX header in the file.

## Credits

* Marcel Holtmann, Luiz Augusto von Dentz and linux-bluetooth contributors
* Upstream CSR-clone detection / Barrot suspend work (Ismael Ferreras Morezuelas,
  Hans de Goede and others)
* [csr8510-fix](https://github.com/hhsnake/csr8510-fix) for the DKMS packaging
  pattern used by this repository
