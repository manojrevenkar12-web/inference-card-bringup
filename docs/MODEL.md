# The card model and its assumptions

LACC is a device model in QEMU (`qemu/hw/misc/lacc.c`), not a copy of any vendor's chip. It is
built to exercise the software a real inference card needs: a PCIe driver, DMA, a command
engine, firmware with a boot ROM, board management on SMBus and SPI, a debug port, power
management and failure handling. This page lists what is computed exactly, what is modelled,
and the constants behind the modelled numbers.

## Exact

- **Arithmetic.** INT8 and LNS8 GEMMs are computed for real, with the same C code the runtime
  uses as its reference (`include/lacc_hw.h`). The bring-up run checks the card's results
  against the reference bit for bit.
- **Data movement.** Copies, scatter-gather lists and fills move real bytes through QEMU's DMA
  API (`pci_dma_read`/`pci_dma_write`), so IOMMU-style mistakes in the driver (wrong address,
  wrong length, unmapped page) show up as wrong data or DMA errors.
- **Firmware.** Images are verified by the boot ROM model with CRC-32 over the header and
  SHA-256 over the payload (QEMU's crypto layer), and the anti-rollback floor is enforced.
  Flash writes persist to the image file.
- **Buses.** The SMBus controller implements the SMBus transaction types the kernel's `pmbus`
  and `at24` drivers use; the SPI controller and the W25Q128 command set are enough for the
  kernel's `spi-nor` driver to probe, read, erase and program. The JTAG TAP follows the IEEE
  1149.1 state machine and speaks OpenOCD's `remote_bitbang` protocol.

## Modelled

Times are on QEMU's virtual clock. Under TCG emulation the virtual clock follows wall time, and
the host takes far longer to compute a GEMM than the modelled card would, so **wall-clock
numbers from the guest describe the emulator, not the card**. The report labels every number as
modelled or emulated.

| Quantity | Model | Constant |
|---|---|---|
| Descriptor fetch and decode | fixed | 200 ns |
| Scatter-gather entry fetch | per entry | 50 ns |
| DMA bandwidth | bytes / rate | 16 GB/s (PCIe 4.0 x16 class) |
| Compute | MACs / (rate x clock) | 16,384 MACs per clock (a 128 x 128 array) |
| Clock states | P0, P1, P2 | 1500, 1000, 400 MHz (from the firmware's CLOCKS record) |
| Energy per INT8 MAC | x clock scale | 2.0 pJ |
| Energy per LNS8 MAC | x clock scale | 0.8 pJ |
| Energy per DMA byte | fixed | 60 pJ |
| Idle power | x (0.6 + 0.4 x clock scale) | 18 W (firmware POWER record) |
| Default power cap | | 75 W (firmware POWER record) |
| Thermal | first order | 35 C ambient, 0.18 C/W, 1.5 s time constant |
| Throttle point | | 95 C (firmware THERMAL record) |

The LNS8 energy advantage (0.8 against 2.0 pJ per MAC) is an **assumption**, chosen because a
logarithmic multiplier is an adder; it is not a measurement. The bring-up report's energy
comparison therefore shows how the software reads energy counters and attributes them to work,
not how much energy a real LNS datapath saves.

### Board power under emulation

Real board power is energy over time. Here the host spends much longer computing a GEMM than
the modelled card would, so energy over wall time would make a busy card look idle. The model
uses an activity factor instead:

```
board power = idle(clock) + activity x active power
activity    = fraction of the last 10 ms the engine had work queued or running (wall time)
active power = modelled energy of that work / its modelled duration
```

A queue kept full reads as a fully busy card, an empty queue as an idle card. Lower clocks
lower active power with the square of the clock scale (energy per MAC scales with the clock,
duration with its inverse). `ENERGY_UJ` integrates board power; `ACTIVE_ENERGY_UJ` holds the
modelled energy of the work alone, which is what to compare datapaths with.

### Power and thermal controller

Every 10 ms the controller compares board power with the cap and the die with the throttle
point. Over either, it lowers the clock ceiling by 100 MHz (not below P2); once power is under
90% of the cap and the die 5 C under the throttle point, it raises it by 50 MHz. The asymmetric
steps and the hysteresis keep the clock from oscillating. Entering a throttled state raises the
THERMAL interrupt.

## Faults

| Fault | What the card does |
|---|---|
| `DMA_ERROR` | The next descriptor completes with status DMA_ERROR and raises the ERROR interrupt |
| `HANG` | The engine stops after fetching the next descriptor and sets STATUS.HALTED; nothing completes until a reset |
| `LINK_DOWN` | Every MMIO read returns all-ones for 500 ms and the engine stalls; the JTAG debug port keeps working |
| `BAD_CQE` | The next completion carries tag 0xdeadbeef |
| `THERMAL` | The die jumps 3 C over the throttle point and the clock drops to P2 |

## The anti-rollback floor

The floor lives in modelled one-time-programmable fuses that survive resets for the life of the
VM; the config sector in flash only mirrors it for the host to read. Rewriting the sector cannot
lower it, and the boot ROM rewrites the mirror if it disagrees.

## Not modelled

PCIe link training and power states (the link always trains at what QEMU's root port offers),
AER error injection, real SRAM/HBM capacity effects, multiple command queues, the card's own
processors running firmware code (the firmware image is data: tables and limits), secure boot
with signatures (images are integrity-checked, not authenticated), and anything about the
performance of a real accelerator.
