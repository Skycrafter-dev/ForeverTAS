#include "blocks/program_cuda_cursor_predict.cuh"

namespace forevertas::blocks::cuda_program_detail {
const void *SelectCursorGenericKernel(bool ordered) {
  return ordered ? reinterpret_cast<const void *>(PredictPhysicsCursor<true>)
      : reinterpret_cast<const void *>(PredictPhysicsCursor<false>);
}
}
