# 0001: Bring the software up against a QEMU device model

**Status:** accepted

**Context.** Platform software for an accelerator card (driver, firmware update, board
management, recovery, tooling) is usually written before silicon, against an FPGA prototype or a
simulator. This project has neither, and the goal is software that a real kernel can load, under
the same debug options a bring-up lab would use, with failures that can be produced on demand.

**Decision.** Model the card as a PCIe device in QEMU (`qemu/hw/misc/lacc.c`) and run the real
Linux driver against it in a guest built with KASAN, lockdep, DMA-API debug and kmemleak.
Compute is done for real so correctness can be checked bit for bit; time and energy are modelled.

**Consequences.** Driver, DMA, firmware, buses, the debug port and recovery are tested end to
end, in CI, in minutes. Performance numbers describe the model and are labelled as such; the
model cannot reveal timing bugs that only real hardware latency would expose, so a port to
hardware would add its own soak tests. The model's register map, descriptor formats and boot ROM
rules are one more spec to keep honest, which is why the register map is generated from one file.
