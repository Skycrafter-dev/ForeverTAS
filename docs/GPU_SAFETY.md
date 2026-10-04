# GPU Search Limits

Manual sample counts and automatic calibration share memory, occupancy, grid
and display-watchdog planning. Every session starts at one candidate, measures
the baseline and checks the requested simultaneous count before growing storage.
Manual CUDA/HIP counts above one also use a disposable two-candidate probe to separate
fixed scene storage from candidate storage; its winner is never published and
the real search retains its original candidate sequence and attempt count.
When a distant timing estimate rejects a count, the probe can grow through
individually checked intermediate capacities. Probe work is not an attempt.
Vulkan instead supplies a conservative per-candidate workspace/staging bound;
cached allocations and fixed scene buffers are not extrapolated as candidate
storage.
Remaining attempts are applied before reservation. Manual batches are rejected
with recovery guidance rather than silently partitioned, which could change
promotion and random-sequence semantics.

Limits are refreshed before every dispatch and promoted-baseline recreation.
CUDA/HIP search buffer allocations and Vulkan buffer allocations also check
available memory before allocation, preserving the larger of 512 MiB or 15%
of the heap, with a further 15% allowance on each requested allocation.
Vulkan search requires `VK_EXT_memory_budget`; heap size is never presented as
free memory. Driver budget/usage estimates can change concurrently and are not
allocation guarantees. See the [Vulkan memory-budget specification](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_memory_budget.html).

CUDA/HIP devices reporting an execution watchdog use a predicted kernel budget
and cooperative submission deadline, including the first probe: 250 ms during
calibration and 1000 ms for manual batches. The manual budget leaves margin below
the [default two-second Windows TDR timeout](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/timeout-detection-and-recovery),
but is not a guarantee for modified OS timeouts or arbitrary drivers.
Exceeded deadlines reject the search and suggest a shorter horizon, fewer
samples or Optimized CPU. Cancellation is checked by running kernels; this is
not hard GPU preemption and cannot recover a wedged driver or bound one physics
tick on all hardware. Vulkan retains its existing bounded per-tick submissions.
