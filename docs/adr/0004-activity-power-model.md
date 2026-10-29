# 0004: Board power from activity, not from energy over wall time

**Status:** accepted

**Context.** Under TCG emulation the host takes far longer to compute a GEMM than the modelled
card would. Energy over wall time would report a card with a full queue as nearly idle, and the
power cap could never be exercised.

**Decision.** Board power is idle power plus activity times active power, where activity is the
fraction of wall time the engine had work and active power is the modelled energy of that work
over its modelled duration (docs/MODEL.md). The energy counter integrates that board power; a
second counter holds the modelled energy of the work alone.

**Consequences.** The cap controller, throttling and the thermal path behave as on a busy card
whenever the queue is kept full, independent of host speed. The validation keeps its load queue
full for that reason. Comparing datapaths uses the active-energy counter, which does not depend
on host speed at all.
