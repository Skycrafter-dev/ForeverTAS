#include "engine/scene/scene_vehicle_car_tuning_types.h"
#include "simulation/backends/cuda/cuda_static_configuration.h"

#include <cuda_runtime.h>

#include <array>
#include <iostream>
#include <stdexcept>

namespace forevertas::blocks::cuda_program_detail {
const void *SelectCursorInitializeKernel(bool ordered);
const void *SelectCursorKernel(bool ordered,
    const forevervalidator::simulation::CudaPackedStaticConfigurationHeader &configuration);
}

namespace {
void Check(bool condition,const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void Load(const void *kernel) {
  Check(kernel!=nullptr,"Missing CUDA cursor kernel.");
  cudaFuncAttributes attributes{};
  const auto status=cudaFuncGetAttributes(&attributes,kernel);
  if (status!=cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
  Check(attributes.maxThreadsPerBlock>0,"Invalid CUDA cursor module attributes.");
}
}

int main() {
  try {
    int count=0;
    const auto status=cudaGetDeviceCount(&count);
    if (status==cudaErrorNoDevice || status==cudaErrorInsufficientDriver || (status==cudaSuccess && !count)) {
      std::cout << "SKIP CUDA cursor module loading: no CUDA device\n";
      return 77;
    }
    if (status!=cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
    using namespace forevertas::blocks::cuda_program_detail;
    forevervalidator::simulation::CudaPackedStaticConfigurationHeader configuration{};
    configuration.tuning.handlingModel=CSceneVehicleCarHandlingModel_Standard;
    const auto legacy=SelectCursorKernel(true,configuration);
    configuration.tuning.handlingModel=CSceneVehicleCarHandlingModel_Lateral;
    Check(SelectCursorKernel(true,configuration)==legacy,"Lateral cursor selected a different legacy implementation.");
    configuration.tuning.handlingModel=CSceneVehicleCarHandlingModel_GearedDrive;
    const auto dry=SelectCursorKernel(true,configuration);
    configuration.water.present=1;
    const auto water=SelectCursorKernel(true,configuration);
    configuration.tuning.handlingModel=0xffffffffu;
    const auto generic=SelectCursorKernel(true,configuration);
    const auto unordered=SelectCursorKernel(false,configuration);
    const std::array<const void *,5> kernels{legacy,dry,water,generic,unordered};
    for (std::size_t i=0;i<kernels.size();++i) {
      Load(kernels[i]);
      for (std::size_t j=0;j<i;++j)
        Check(kernels[i]!=kernels[j],"Distinct CUDA cursor paths selected the same implementation.");
    }
    Load(SelectCursorInitializeKernel(false));
    Load(SelectCursorInitializeKernel(true));
    std::cout << "PASS CUDA cursor registration: five physics paths and both initialization paths\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
