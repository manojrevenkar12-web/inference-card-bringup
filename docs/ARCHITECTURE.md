# Architecture

```
 host (guest kernel and user space)                         the card (QEMU device model)
 ───────────────────────────────────────                    ─────────────────────────────────────
 lacc-validate, lacc-mlp     laccctl                        BAR0 registers ── regs/lacc.yaml
   │ C++ runtime               │ status, fw update,         BAR2 64 MiB device memory
   │ (runtime/)                │ trace, regs, fault         BAR4 MSI-X (completion, events)
   ▼                           ▼                            command engine ── SQ/CQ rings in host memory
 /dev/lacc0 ioctls       sysfs, debugfs, MTD, tracefs         copies, SG DMA, fill, INT8 and LNS8 GEMM
   │                           │                            power/thermal model and controller
 lacc.ko ─────────────────────────────────────── PCIe ───── fault injection
   ├─ rings, jobs, fences, watchdog, recovery               SMBus ── PMBus regulator @0x40, FRU EEPROM @0x50
   ├─ DMA: pin, sg_table, dma_map_sgtable                   SPI ──── W25Q128 NOR: slot A, slot B, config
   ├─ i2c adapter ──► pmbus (hwmon), at24 (nvmem)           boot ROM: CRC-32, SHA-256, anti-rollback, TLVs
   ├─ spi controller ──► spi-nor ──► MTD partitions         JTAG TAP ── remote_bitbang chardev ── OpenOCD
   └─ sysfs, debugfs, tracepoints
```

## One source for the register map

`regs/lacc.yaml` defines offsets, bit fields and values once. `tools/gen_regs.py` writes
`include/lacc_regs.h` (device model, driver, runtime), `laccctl/src/regs.rs` and
`docs/REGISTERS.md`; CI fails if any is stale. `include/lacc_hw.h` holds the descriptor,
completion and firmware formats and the LNS8 arithmetic, shared by the device model, the driver
and the runtime, so the reference GEMM and the card run the same code.

## Command path

1. User space allocates device memory (`LACC_IOC_ALLOC`, a first-fit extent allocator in
   `lacc_mem.c`) and owns it through its file; transfers and GEMMs may only name its own buffers.
2. A transfer pins the user pages (`pin_user_pages_fast`), builds an `sg_table` and maps it
   with `dma_map_sgtable`. In scatter-gather mode the driver writes one SG list and one
   descriptor; in per-segment mode one descriptor per DMA segment.
3. `lacc_submit` takes the next sequence number as the job's fence, writes the descriptors to
   the submission ring (flow control keeps at most 255 in flight), sets the interrupt flag on
   the job's last descriptor and rings the doorbell.
4. The card completes descriptors in order into the completion ring. Each entry carries a phase
   bit that flips when the ring wraps, so the driver finds new entries without reading the tail.
5. The threaded completion interrupt walks the ring, matches tags to jobs in an xarray, and when
   a job's last descriptor is done releases its pins and mappings and wakes `LACC_IOC_WAIT`.
6. Interrupt coalescing (`coalesce` in sysfs: count and microseconds) trades interrupts for
   latency; the watchdog also polls the ring, so a lost interrupt delays a job but never loses it.

## Recovery

A delayed work item runs every 200 ms. It treats all-ones from STATUS as a link outage (counted
once per outage), STATUS.HALTED as a dead engine, and jobs outstanding with no completion for
`hang_timeout_ms` as a hang. Any of them queues one recovery: snapshot the registers into
debugfs `last_crash`, drain what did complete, abort the rest with `LACC_WAIT_ABORTED`, wait for
the link, reset the card, re-arm the rings. Waiters see an aborted fence; the next job runs.

## Board management

The card's SMBus controller is registered as an i2c adapter, so the kernel's own drivers bind to
the parts on it: `pmbus` exposes the regulator as hwmon, `at24` the FRU EEPROM. The SPI
controller is a `spi_controller`, and `spi-nor` turns the NOR flash into MTD partitions
`lacc-fw-a`, `lacc-fw-b`, `lacc-config` and `lacc-spare`. Firmware updates go through MTD
(`MEMERASE`, write, read back) from `laccctl`; then `fw_reboot` makes the card re-run its boot
ROM, which picks the slot named in the config, falls back to the other valid slot, and refuses
images below the anti-rollback floor.

## Debug port

The JTAG TAP has IDCODE, DBG_ADDR and DBG_DATA instructions. DBG_DATA is 33 bits: a scan reads
the register at DBG_ADDR, and only a scan with bit 32 set writes. It reaches the register file
without going through PCIe, so it works while the link is down; `tools/bringup.py` uses it to
inject a link drop and to check that a value written over JTAG reads back over the BAR.

## The bring-up run

`tools/bringup.py` boots the guest kernel (KASAN, lockdep, DMA-API debug, kmemleak) with the
card behind a PCIe root port, answers the guest's JTAG stage through OpenOCD, and turns the
console into `REPORT.md`, `report.json` and a Perfetto trace. The guest side is
`validate/run.sh`: `lacc-validate` (C++), `lacc-mlp`, firmware updates with `laccctl`, a trace,
module reload under an open file, kmemleak and a scan of the kernel log.
