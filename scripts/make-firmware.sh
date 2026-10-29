#!/usr/bin/env bash
# Firmware images for the validation run and the flash image the card boots from.
#   slot A: version 2 (rollback index 1), slot B: version 1 (rollback index 0), boot newest
#   fw-3.bin: the update (version 3, rollback index 2); fw-bad.bin: a corrupt payload
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$here/build}
mk="python3 $here/fw/mkfw.py"
mkdir -p "$out/fw"
$mk image --version 1 --rollback 0 -o "$out/fw/fw-1.bin"
$mk image --version 2 --rollback 1 -o "$out/fw/fw-2.bin"
$mk image --version 3 --rollback 2 -o "$out/fw/fw-3.bin"
$mk image --version 9 --rollback 2 --corrupt payload -o "$out/fw/fw-bad.bin"
$mk flash --a "$out/fw/fw-2.bin" --b "$out/fw/fw-1.bin" --boot-slot 0 -o "$out/flash.img"
echo "firmware: $(ls "$out/fw" | tr '\n' ' ')and $out/flash.img"
