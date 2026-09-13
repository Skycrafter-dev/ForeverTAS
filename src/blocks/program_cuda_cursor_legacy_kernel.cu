#include "blocks/program_cuda_cursor_predict.cuh"

namespace forevertas::blocks::cuda_program_detail {
const void *SelectCursorLegacyKernel() {
  return reinterpret_cast<const void *>(PredictPhysicsCursor<true,CudaHandlingSpecialization::Legacy>);
}
}
