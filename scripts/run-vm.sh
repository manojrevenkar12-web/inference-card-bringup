#!/usr/bin/env bash
# Boot the guest kernel in QEMU with the LACC card attached.
#   scripts/run-vm.sh <qemu> <bzImage> <initramfs> <flash.img> <mode> [jtag-port]
# The serial console is printed; the flash image is updated in place by the guest.
# QEMU_BIOS: directory with QEMU's firmware blobs, for a qemu-system binary copied out of its tree.
set -euo pipefail
qemu=$1; kernel=$2; initrd=$3; flash=$4; mode=${5:-validate}; jtag=${6:-3335}
bios=()
[ -n "${QEMU_BIOS:-}" ] && bios=(-L "$QEMU_BIOS")
exec "$qemu" "${bios[@]}" -machine q35 -m 1536 -smp 2 -nographic -no-reboot \
  -kernel "$kernel" -initrd "$initrd" \
  -append "console=ttyS0 panic=-1 oops=panic loglevel=5 kasan.fault=panic lacc.mode=$mode" \
  -chardev socket,id=jtag0,host=127.0.0.1,port="$jtag",server=on,wait=off \
  -device pcie-root-port,id=rp0,chassis=1,slot=1 \
  -device lacc,bus=rp0,flash="$flash",jtag=jtag0,serial=LACC2026A0001 \
  -nic none -monitor none
