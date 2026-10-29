#!/usr/bin/env bash
# Build QEMU (x86_64 system emulator only) with the LACC card added.
#   scripts/build-qemu.sh <qemu source tree> [build dir]
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
src=$(cd "$1" && pwd)
build=${2:-$src/build-lacc}

cp "$here/qemu/hw/misc/lacc.c" "$src/hw/misc/lacc.c"
cp "$here/include/lacc_hw.h" "$here/include/lacc_regs.h" "$src/hw/misc/"
grep -q "CONFIG_LACC" "$src/hw/misc/meson.build" ||
  echo "system_ss.add(when: 'CONFIG_LACC', if_true: [files('lacc.c'), zlib])" >> "$src/hw/misc/meson.build"
grep -q "config LACC" "$src/hw/misc/Kconfig" || cat >> "$src/hw/misc/Kconfig" <<'KC'

config LACC
    bool
    default y if TEST_DEVICES
    depends on PCI && MSI_NONBROKEN
KC

# QEMU's own floating-point tests pull a subproject from gitlab; they are not needed here
grep -q "lacc: skipped" "$src/tests/fp/meson.build" ||
  sed -i '1i subdir_done()  # lacc: skipped, not needed to build the emulator' "$src/tests/fp/meson.build"

mkdir -p "$build"
cd "$build"
if [ ! -f build.ninja ]; then
  "$src/configure" --target-list=x86_64-softmmu --disable-docs --disable-werror --disable-tools \
    --disable-guest-agent --disable-gtk --disable-sdl --disable-vnc --disable-spice --disable-opengl \
    --disable-xen --disable-libusb --disable-usb-redir
fi
ninja qemu-system-x86_64
echo "built $build/qemu-system-x86_64"
