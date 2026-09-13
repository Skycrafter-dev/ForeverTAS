#include "blocks/program_cuda_device.cuh"

namespace forevertas::blocks::cuda_program_detail {

template<bool OrderedEllipsoids>
__global__ void InitializePhysicsCursor(KernelParameters parameters,unsigned char *arenas,Value *stacks,
    CudaCandidateState *states,ScheduledLane<OrderedEllipsoids> *lanes,bool preserveState) {
  if (preserveState) {
    auto &context=lanes[0];
    context.parameters=parameters;
    auto &machine=context.machine;
    machine.memory=Memory{};
    machine.memory.bytes=arenas; machine.memory.capacity=parameters.arenaBytes;
    machine.memory.used=parameters.initialBytes; machine.globals=parameters.globals;
    auto &physics=context.physics;
    physics.inputs=machine.memory.copy(parameters.inputs);
    physics.tickIndex=0; physics.pendingTicks=0;
    physics.nativeVersions=0; physics.overflowDirtyCount=0;
    physics.snapshotExtension={}; physics.snapshotExtensionDirty=false;
    physics.rebuildControls(machine,physics.current.properties[0].x);
    return;
  }
  InitializeLane(parameters,arenas,stacks,states,lanes[0],0,false);
  lanes[0].physics.snapshotExtensionDirty=false;
}

const void *SelectCursorInitializeKernel(bool ordered) {
  return ordered ? reinterpret_cast<const void *>(InitializePhysicsCursor<true>)
      : reinterpret_cast<const void *>(InitializePhysicsCursor<false>);
}

const void *SelectCursorGenericKernel(bool ordered);
const void *SelectCursorLegacyKernel();
const void *SelectCursorGearedDryKernel();
const void *SelectCursorGearedWaterKernel();

const void *SelectCursorKernel(bool ordered,const CudaPackedStaticConfigurationHeader &configuration) {
  if (!ordered) return SelectCursorGenericKernel(false);
  switch (configuration.tuning.handlingModel) {
  case static_cast<std::uint32_t>(CSceneVehicleCarHandlingModel_Standard):
  case static_cast<std::uint32_t>(CSceneVehicleCarHandlingModel_Lateral):
    return SelectCursorLegacyKernel();
  case static_cast<std::uint32_t>(CSceneVehicleCarHandlingModel_GearedDrive):
    return configuration.water.present
      ? SelectCursorGearedWaterKernel() : SelectCursorGearedDryKernel();
  default: return SelectCursorGenericKernel(true);
  }
}

} // namespace forevertas::blocks::cuda_program_detail
