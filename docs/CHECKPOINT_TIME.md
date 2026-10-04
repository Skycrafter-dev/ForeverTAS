# Checkpoint Time

The **Checkpoint time** target minimizes the canonical simulation tick
at which the game accepts the selected checkpoint or finish. It does not use
geometric volume intersection or infer acceptance from `car.cps`.

Selectors:

- **Event:** checkpoint or finish. Checkpoint number is ignored for finish.
- **Checkpoint number:** one-based accepted ordinal within the lap, taken from
  the engine's checkpoint index. The default is the first checkpoint.
- **Lap:** one-based lap of acceptance, before the finish increments completed laps.
- **Map slot:** the map-local zero-based checkpoint identity, or `-1` for any.
  The reserved finish slot equals the number of ordinary checkpoint slots.
- **Event index:** one-based global accepted checkpoint/finish sequence, or `0`
  for any. The sequence continues across laps and respawns. It is stored and
  edited as an exact 64-bit integer, never a floating-point identifier.

The result names the checkpoint number (or finish), lap, map slot and event
index, for example `Checkpoint 2 of lap 1 (map slot 5, event index 2): 0:12.34`.
Evaluate base shows these numbers for the base inputs, which is the easiest
way to find the values to enter; each selector also has an (i) explanation.
All selectors must match. Multiple events in one tick remain separate and are examined in engine
acceptance order; the first eligible matching event wins. Conditions are tested
on that exact tick. A rejected crossing, missing event, or condition-rejected
event produces no eligible evaluation, not a later substitute. Observation
starts at the first simulation tick, even when mutations begin later.

Reference, Optimized CPU, Multi-threaded CPU and CUDA searches are supported.
HIP and Vulkan searches are explicitly rejected for this target. Evaluate base
uses Optimized CPU and remains available.

CUDA has no event journal; its kernel names each accepted event from the race
counters before and after the tick (lap checkpoint count, global event count,
completed laps, finishes). Checkpoint numbers, laps, event indices and the
finish are therefore exact, as is the map slot of the finish and of the last
checkpoint accepted in a tick (from its block). If several ordinary
checkpoints are accepted in the same 10 ms tick, the map slots of the earlier
ones are unknown, so a target that sets a map slot does not match them on CUDA
(it never reports a wrong event). Every CUDA winner is also re-checked on the
optimized CPU. Resolution is the canonical 10 ms
tick; use Precise finish time for the separate sub-tick finish estimator.

## Validator Contract

`PhysicsSandboxStateView::acceptedCheckpointEvents` contains only events
accepted during the latest CPU tick, including all simultaneous events. An
event carries its slot, engine checkpoint index, lap, global event index, tick,
time and finish flag. `AdvanceTicks(n)` returns only the last tick's journal;
consumers needing every event must advance one tick at a time. GPU views have
no accepted-event journal; `PhysicsSandboxCudaCheckpointEvaluator` reconstructs
the selected event inside the CUDA (and generated HIP) search kernel as
described above, and Vulkan rejects it.

The journal is recorded at the engine's acceptance boundary, including the
Shortcut finish path. It is cleared before each tick and copied with runtime
snapshots together with its sequence counter. It is observational data, not a
change to checkpoint/finish acceptance, physical state hashing or GPU layouts.
