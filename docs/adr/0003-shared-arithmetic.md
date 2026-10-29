# 0003: One C definition of the arithmetic, shared by the card and the reference

**Status:** accepted

**Context.** "The card computed the right answer" needs a reference that is not the card. A
float reference cannot check a quantised datapath exactly, and two implementations of the same
rounding rules drift.

**Decision.** The LNS8 product, accumulation, rounding and saturation, and the INT8 GEMM, are
`static inline` C in `include/lacc_hw.h`. The QEMU model and the user-space reference
(`runtime/include/lacc/lns.hpp`) include the same header. The bring-up suite compares card
results with the reference bit for bit, and separately measures error against FP64.

**Consequences.** A bit-exact mismatch means a transport or driver bug (wrong buffer, wrong
offset, a stale cache line, a lost descriptor), not a numerics question. The format is a
textbook hybrid LNS datapath with an 8-entry table loaded from firmware; it is not any vendor's
format.
