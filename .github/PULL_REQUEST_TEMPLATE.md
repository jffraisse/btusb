## What this changes

Which clone behaviour is affected, and why. Keep changes scoped to the
fake-CSR path (`btusb_setup_csr()`); upstream `btusb` behaviour must stay intact.

## Test environment

- Distribution and kernel (`uname -r`)
- Dongle (`lsusb -v -d <vid>:<pid>`: idVendor / idProduct / bcdDevice / strings)
- `dkms status btusb-csr-clone`

## Verification

Commands run and the relevant `dmesg` output before/after. If a module
parameter changed, state its default.
