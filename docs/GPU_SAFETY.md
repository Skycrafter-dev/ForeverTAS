# GPU Search Limits

Manual sample counts and automatic calibration share memory, occupancy and grid
planning. Every session starts at one candidate, measures
the baseline and checks the requested simultaneous count before growing storage.
Manual CUDA/HIP counts above one also use a disposable two-candidate probe to separate
fixed scene storage from candidate storage; its winner is never published and
the real search retains its original candidate sequence and attempt count.
Probe work is not an attempt.
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

ForeverTAS does not impose a GPU step time limit or reject batches based on
predicted duration. User cancellation remains available. Operating-system and
driver timeouts still apply; ForeverTAS does not change them. Cancellation is
not hard GPU preemption and cannot recover a wedged driver. Vulkan retains its
existing bounded per-tick submissions.
