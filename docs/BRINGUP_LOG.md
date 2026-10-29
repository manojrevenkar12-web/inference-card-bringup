# Bring-up log: what the runs found

Each entry is a defect the validation suite, the debug kernel, a measurement or a design review
exposed while this project was brought up, with the evidence and the fix. They are kept because
finding them is the point of the suite.

| # | Found by | Symptom | Cause | Fix |
|---|---|---|---|---|
| 1 | Host unit test `Firmware.LayoutIsFixed` | `sizeof(struct lacc_desc)` was 56, not the 64 the ring layout and the documentation assume | Field sizes added up to 56; nothing checked it | Padded to 64 bytes; the test pins every wire structure's size and offsets |
| 2 | DMA-API debug in the guest kernel | `DMA-API: mapping sg segment longer than device claims to support [len=913408] [max=65536]` and a WARN from `debug_dma_map_sg` | `sg_alloc_table_from_pages()` merges physically contiguous pages into long segments, but the driver never told the DMA layer the card has no 64 KiB segment limit | `dma_set_max_seg_size(dev, UINT_MAX)` at probe (descriptors and SG entries carry 32-bit lengths) |
| 3 | The SG-versus-per-segment measurement | "13,985 descriptors" for a 16 MiB transfer, more than its 4,096 pages | `desc_submitted += i` added the running index once per ring batch, not the batch size | Count `batch`; the same measurement now reads 47 descriptors (segments merged after fix 2) |
| 4 | The JTAG stage's cross-check | One injected link drop counted as two outages and two resets | The watchdog kept seeing all-ones while the first recovery waited for the link, and re-queued the reset work | Count an outage on its first all-ones read only; at most one recovery queued or running (`reset_queued`) |
| 5 | Designing the OpenOCD read path | A plain `drscan` of DBG_DATA would write whatever was shifted in back into the register | The update-DR state wrote unconditionally | DBG_DATA is 33 bits and writes only with bit 32 set; a test models a port that writes on read and checks the session catches it |
| 6 | Working through the power-cap check before its first run | Board power would have read close to the idle floor under a full queue | Under emulation the host computes a GEMM far slower than the modelled card, so energy over wall time made the card look idle | Activity-factor power model (docs/MODEL.md, ADR 0004) |
| 7 | The power-cap check, later run | Uncapped load averaged 43.4 W in one run and 56 W in another | The load loop drained the queue between batches, so activity depended on emulator speed | Keep 16 jobs queued at all times; the check's uncapped reading must exceed the cap or the check fails as inconclusive |
| 8 | Writing the anti-rollback checks | The rollback floor lived only in the flash config sector, which the host can rewrite | The model trusted flash for a security property | The floor is held in modelled fuses; the sector only mirrors it, and the boot ROM repairs the mirror |
| 9 | `scripts/build-kernel.sh` verifying the merged config | `CONFIG_MTD_CHAR` and `CONFIG_STACKPROTECTOR_STRONG` in the fragment did not survive `olddefconfig` | The first no longer exists (MTD character devices are part of MTD core); the second depends on an option tinyconfig turns off | Removed from the fragment; the build now fails if any requested option is dropped |
| 10 | sparse (`make -C driver check`) | 20+ "cast to restricted __le32" warnings | The shared wire structures used plain integers in the kernel | `lacc_le16/32/64` map to `__le16/32/64` in the kernel and to plain integers in QEMU and user space |
