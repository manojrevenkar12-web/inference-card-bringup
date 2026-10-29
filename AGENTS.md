# Working in this repository

## Fast feedback

| What | Command | Time |
|---|---|---|
| Python tools and their tests | `ruff check fw tools tests && python3 -m pytest -q tests` | seconds |
| C++ runtime and reference datapath | `cmake -S . -B build/host -G Ninja -DLACC_SANITIZE=ON && ninja -C build/host && ctest --test-dir build/host` | under a minute |
| laccctl | `cd laccctl && cargo fmt --check && cargo clippy --release -- -D warnings && cargo test --release` | under a minute |
| Generated files are current | `python3 tools/gen_regs.py --check` | instant |
| Driver: W=1, sparse, checkpatch --strict | `make -C driver check KDIR=<linux build tree>` | a minute |
| The whole bring-up in QEMU | see "Running the bring-up" in README.md | about two minutes after the kernel and QEMU are built |

## Invariants (each has a check)

- **One register map.** Offsets, bits and values live in `regs/lacc.yaml`. Never edit
  `include/lacc_regs.h`, `laccctl/src/regs.rs` or `docs/REGISTERS.md` by hand; run
  `tools/gen_regs.py`. CI runs `--check`.
- **One definition of the arithmetic.** The LNS8 and INT8 datapaths are the inline functions in
  `include/lacc_hw.h`, used by the device model and the reference alike. Do not reimplement
  them; the bring-up compares the card with the reference bit for bit.
- **Wire structures are fixed.** `lacc_desc` is 64 bytes, `lacc_cqe` 16, the firmware header 56;
  multi-byte fields are `lacc_le*` so sparse checks byte order. `Firmware.LayoutIsFixed` pins it.
- **The driver stays clean.** `make -C driver check` fails on any W=1 or sparse warning and any
  checkpatch --strict error, warning or check.
- **The debug kernel stays quiet.** The run fails on any KASAN, lockdep, DMA-API debug, UBSAN or
  WARN report, and on a kmemleak report with driver frames.
- **Recovery is exact.** One injected outage is one counted outage and one reset; an aborted job
  fails its own fence only and the next job runs.
- **Modelled is never shown as measured.** Numbers from the device model's timing and energy
  model say "modelled"; wall-clock numbers from the guest say "emulated". `docs/MODEL.md` lists
  every constant.
- **Reports are generated.** `reports/bringup/` is written by `tools/bringup.py`; change the
  tools, not the report.

## Where things are

`qemu/hw/misc/lacc.c` the card. `driver/` the Linux driver. `runtime/` the C++ runtime.
`laccctl/` the service tool. `validate/` the guest suite. `fw/mkfw.py` firmware images.
`guest/` the guest kernel config, init and initramfs. `tools/` generators, the bring-up runner and
the report. `docs/` architecture, model, bring-up log, ADRs.

## Making a change

Device behaviour changes go with a check in `validate/` that would have failed before, and an
entry in `docs/BRINGUP_LOG.md` when the change fixes a defect the run found. New registers start
in `regs/lacc.yaml`. A new board part gets a kernel driver binding (prefer an in-kernel driver,
as the PMBus regulator, EEPROM and NOR flash do) rather than a private interface.
