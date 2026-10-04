# Redundant Steering Insertions

Insertion still samples all random counts, times, holds and values, removes
overwritten channel events and restores the original held value at the end.
After normalization, a newly inserted analog steering event matching the
retained held value can be removed. Existing matching events, nonzero-to-zero
releases, necessary restoration boundaries and structural actions are retained.
Mutation counts describe the retained event stream.

This optimization is enabled only for non-Stunts searches with no digital
steering actions anywhere in the baseline, including the prefix and tail.
Stunts observes steering-change timestamps; digital/analog steering arbitration
also observes timestamps. Those cases deliberately keep the old representation.
Low-level mutators default to the conservative, unpruned behavior unless the
caller establishes the same preconditions. A pass with noncanonical input also
retains the old behavior. CPU, CUDA/HIP sparse and materialized paths, and Vulkan use the
same rule without changing random-number progression.
