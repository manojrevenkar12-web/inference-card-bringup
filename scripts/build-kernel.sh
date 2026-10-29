#!/usr/bin/env bash
# Build the guest kernel: tinyconfig + kvm_guest.config + guest/kernel.config (KASAN, lockdep,
# DMA-API debug, kmemleak, the board's I2C/PMBus/at24/SPI NOR drivers).
#   scripts/build-kernel.sh <linux source tree>
# Leaves arch/x86/boot/bzImage and a tree that out-of-tree modules build against.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
src=$(cd "$1" && pwd)
cd "$src"
make -s tinyconfig
make -s kvm_guest.config
scripts/kconfig/merge_config.sh -m .config "$here/guest/kernel.config" >/dev/null
make -s olddefconfig
# every option the fragment asks for must have survived olddefconfig
missing=0
while IFS= read -r line; do
  case "$line" in CONFIG_*=*) ;; *) continue ;; esac
  key=${line%%=*}; want=${line#*=}
  if [ "$want" = n ]; then
    grep -q "^$key=" .config && { echo "kernel config: $key is set, wanted n"; missing=1; }
  else
    grep -qx "$line" .config || { echo "kernel config: wanted $line, got $(grep "^$key=" .config || echo unset)"; missing=1; }
  fi
done < "$here/guest/kernel.config"
[ "$missing" = 0 ] || exit 1
make -s -j"$(nproc)" bzImage modules
echo "built $src/arch/x86/boot/bzImage ($(make -s kernelrelease))"
