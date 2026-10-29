#!/usr/bin/env bash
# Pack the validation guest: busybox, the driver module, the validation binaries and scripts.
#   guest/mkinitramfs.sh <lacc.ko> <out.cpio.gz> [binaries...]
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
ko=$1; out=$2; shift 2
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
mkdir -p "$root"/{bin,sbin,lib/modules,proc,sys,dev,tmp,etc,validate,fw,lib64,usr/lib}
cp "$(command -v busybox)" "$root/bin/busybox"
for a in $("$root/bin/busybox" --list); do [ "$a" = busybox ] || ln -sf busybox "$root/bin/$a"; done
cp "$here/guest/init" "$root/init"
cp "$ko" "$root/lib/modules/lacc.ko"
cp "$here"/validate/*.sh "$root/validate/" 2>/dev/null || true
cp "$here"/build/fw/*.bin "$root/fw/" 2>/dev/null || true
for b in "$@"; do
  cp "$b" "$root/bin/"
  # dynamic binaries take their libraries along
  if ldd "$b" >/dev/null 2>&1; then
    for lib in $(ldd "$b" | grep -o '/[^ ]*'); do
      mkdir -p "$root$(dirname "$lib")"
      cp -L "$lib" "$root$lib"
    done
  fi
done
(cd "$root" && find . | busybox cpio -o -H newc 2>/dev/null | gzip -9) > "$out"
echo "initramfs: $out ($(du -h "$out" | cut -f1))"
