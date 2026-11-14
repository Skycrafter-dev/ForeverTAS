#ifndef FOREVERTAS_COMPUTE_COMPUTE_LOCAL_H
#define FOREVERTAS_COMPUTE_COMPUTE_LOCAL_H

#include "compute/compute_module_api.h"
#include "physics_backend.h"

namespace forevertas::compute {

// The GPU backends compiled into this binary (a GPU module, or a developer
// build with CUDA/HIP): what the module exports, called directly.
bool LocalHas(PhysicsBackend backend) noexcept;
void LocalStatus(PhysicsBackend backend, GpuStatus *out);
void LocalRunSearch(const SearchRequest &request, const SearchRunControl *control,
                    SearchOutcome *out);

}  // namespace forevertas::compute

#endif
