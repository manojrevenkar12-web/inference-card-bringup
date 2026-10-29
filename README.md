# inference-card-bringup

Platform software for a PCIe AI inference card, brought up end to end before the hardware
exists: a Linux driver, firmware update over SPI flash, board management over SMBus, a JTAG
debug port, power and thermal control, fault recovery, and a validation suite that boots a
debug kernel in QEMU on every commit and writes a bring-up report.

The card ("LACC", `1234:7acc`) is a device model in QEMU. It computes for real (INT8 GEMM and a
log-number-system LNS8 GEMM whose products are additions of logarithms) and models time and
energy. Everything above it is the software a real card needs, running against real kernel
subsystems: the kernel's own `pmbus`, `at24` and `spi-nor` drivers bind to the card's board
parts, OpenOCD drives its TAP, and the guest kernel runs with KASAN, lockdep, DMA-API debug and
kmemleak.

**Latest run: all 48 checks pass, with no KASAN, lockdep, DMA-API, UBSAN or WARN reports, in
100 s under emulation.** The report is in [`reports/bringup/REPORT.md`](reports/bringup/REPORT.md);
CI regenerates it on every push.

## What the run shows

| Area | Result (from `reports/bringup/`) |
|---|---|
| Enumeration | PCIe endpoint behind a root port, link up, AER capability, 64 KiB BAR0, 64 MiB BAR2, 2 MSI-X vectors |
| DMA | Host-card-host round trips from 4 B to 9 MiB, aligned and misaligned, in scatter-gather and per-segment mode; BAR2 mmap and DMA agree; out-of-range transfers refused |
| Compute | LNS8 and INT8 GEMMs on the card equal the reference bit for bit, six shapes up to 320 x 288 x 256 |
| Number formats | Matmul error against FP64: Gaussian data LNS8 3.55%, INT8 1.51%; heavy-tailed data LNS8 3.12%, INT8 22.97% |
| Inference | A small classifier's matmuls on the card: FP32 85.69%, LNS8 85.60%, INT8 85.50% accuracy; LNS8 agrees with FP32 on 99.12% of predictions, INT8 on 98.24% |
| Board | PMBus regulator readings through hwmon; FRU EEPROM record and checksum, write-protected; NOR flash as MTD partitions |
| Firmware | Update slot B and boot it; the anti-rollback floor rises; a corrupt image is refused by the tool and, if forced, by the boot ROM, which falls back; an image below the floor is refused both ways |
| Power | Under a full INT8 queue 52.0 W uncapped; with a 40 W cap the controller settles at 1200 MHz and 39.6 W, and lifts the clock when the cap is removed (modelled) |
| Faults | DMA error fails one job only; a hung engine is reset and its job aborted in 51 ms; a link drop reads all-ones, is counted once, reset once, and jobs run again after 519 ms; corrupt completions and thermal events are handled |
| JTAG | IDCODE, register reads and writes over OpenOCD; reads have no side effects; the debug port still reads the card while PCIe is down; a link drop injected over JTAG is recovered by the driver |
| Driver lifecycle | `rmmod` refused while a client holds the device; three unload/reload cycles; kmemleak finds nothing |
| Tracing | Driver tracepoints to a Perfetto trace of 666 jobs (`reports/bringup/trace.json`) |
| Interrupts | Coalescing at 32 jobs / 1 ms cuts interrupts for a 512-job burst from 303 to 76, and costs a lone job about 0.8 ms of wake-up latency |

Numbers marked modelled come from the device model (`docs/MODEL.md` lists every constant,
including the assumed 0.8 pJ per LNS8 MAC against 2.0 pJ per INT8 MAC). They show that the
software reads and controls these quantities correctly; they are not claims about silicon.

## What the run found

The suite found real defects while the project was brought up, among them a missing
`dma_set_max_seg_size()` that DMA-API debug caught, a descriptor counter that reported 13,985
descriptors for a transfer that used 47, a watchdog that turned one link outage into two resets, and a 56-byte
descriptor that was meant to be 64. All ten are in [`docs/BRINGUP_LOG.md`](docs/BRINGUP_LOG.md)
with the evidence and the fix.

## Layout

```
regs/lacc.yaml         the register map; tools/gen_regs.py generates C, Rust and docs from it
include/               wire formats, firmware format and the LNS8/INT8 arithmetic (shared)
qemu/hw/misc/lacc.c    the card: engine, DMA, power/thermal, SMBus, SPI NOR, boot ROM, JTAG, faults
driver/                Linux driver: rings, DMA, MSI-X, recovery, i2c and spi controllers, sysfs, tracepoints
runtime/               C++20 runtime over /dev/lacc0, the reference datapath, unit tests
laccctl/               Rust service tool: status, firmware inspect/update over MTD, Perfetto traces, registers, faults
validate/              the guest suite (lacc-validate in C++, run.sh)
examples/              a classifier trained on the host, its matmuls run on the card
fw/mkfw.py             firmware images and flash images
guest/                 guest kernel config fragment, init, initramfs builder
tools/                 bring-up runner (QEMU + OpenOCD), report generator, register generator
docs/                  architecture, model, bring-up log, ADRs
```

## Running the bring-up

You need a Linux source tree (6.12), a QEMU source tree (10.1), a C++20 compiler, Rust,
`busybox-static`, `openocd` and `sparse`.

```sh
scripts/build-kernel.sh ~/linux                      # tinyconfig + debug options, ~10 min
scripts/build-qemu.sh ~/qemu                         # adds the card to QEMU
make -C driver KDIR=~/linux && make -C driver check KDIR=~/linux
cmake -S . -B build/guest -G Ninja -DLACC_STATIC=ON -DLACC_TESTS=OFF && ninja -C build/guest
(cd laccctl && RUSTFLAGS="-C target-feature=+crt-static" cargo build --release --target x86_64-unknown-linux-gnu)
scripts/make-firmware.sh build
guest/mkinitramfs.sh driver/lacc.ko build/initramfs.cpio.gz build/guest/lacc-validate build/guest/lacc-mlp \
  laccctl/target/x86_64-unknown-linux-gnu/release/laccctl
python3 tools/bringup.py --qemu ~/qemu/build-lacc/qemu-system-x86_64 --kernel ~/linux/arch/x86/boot/bzImage \
  --initrd build/initramfs.cpio.gz --flash build/flash.img --out reports/bringup
```

Host-only checks (no kernel or QEMU) are in [`AGENTS.md`](AGENTS.md).

## Limits

- The card is a model. Its timing and energy are assumptions, and the run measures the
  software, not an accelerator. Wall-clock numbers inside the guest describe TCG emulation.
- The driver is a misc device with ioctls, not a `drivers/accel` (DRM) driver; ADR 0002 says
  what moving there would change.
- Firmware images are integrity-checked (CRC-32, SHA-256, rollback floor) but not signed; the
  card's processors are not modelled, so "firmware" is tables and limits read by the boot ROM.
- One command queue, one card, x86_64 guests only.
