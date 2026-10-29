# 0002: A misc character device, not (yet) the DRM accel subsystem

**Status:** accepted, to revisit

**Context.** Linux has a subsystem for compute accelerators (`drivers/accel`, built on DRM:
GEM buffer objects, DMA-BUF sharing, per-file contexts, scheduler integration). Upstream
drivers for inference cards use it.

**Decision.** For this card, a misc device with seven ioctls (info, alloc, free, transfer, GEMM,
fill, wait) and per-file ownership of device memory. The goal here is the bring-up path: rings,
DMA mapping, interrupts, recovery, board management and tooling, and a small interface keeps
every part of it visible and testable.

**Consequences.** No DMA-BUF import/export, no GEM, no GPU scheduler. Moving to `drivers/accel`
is the right step for a product driver: buffer objects would become GEM objects, the fence
becomes a `dma_fence`, and the submission path a DRM scheduler job. The ring, DMA, recovery and
board code would carry over unchanged.
