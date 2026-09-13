#ifndef FOREVERTAS_BLOCKS_PROGRAM_CUDA_CURSOR_PREDICT_CUH
#define FOREVERTAS_BLOCKS_PROGRAM_CUDA_CURSOR_PREDICT_CUH

#include "blocks/program_cuda_device.cuh"

namespace forevertas::blocks::cuda_program_detail {

template<bool OrderedEllipsoids>
__device__ __noinline__ Value ExportPhysicsCursor(ScheduledLane<OrderedEllipsoids> &context,Memory &output,
    CudaPhysicsCursorState &view) {
  auto &physics=context.physics;
  auto &machine=context.machine;
  physics.observeCurrent(machine); physics.observePrevious(machine);
  auto record=output.allocate(Kind::List,4);
  Value extension;
  if (physics.snapshotExtensionDirty) {
    extension=output.allocate(Kind::Opaque,sizeof(CudaCandidateState),1);
    if (output.error!=Error::None) return {};
    *reinterpret_cast<CudaCandidateState *>(output.bytes+extension.handle)=physics.extended;
    physics.snapshotExtensionDirty=false;
    physics.overflowDirtyCount=0;
    ++physics.nativeVersions;
  }
  const auto physical=output.allocate(Kind::Opaque,sizeof(CudaCandidatePhysicsState),1);
  using Overflow=decltype(physics.extended.collisionReplacementOverflow);
  if (!machine.check(physics.overflowDirtyCount<=CudaCollisionReplacementOverflowCapacity,Error::Bounds)) return {};
  const auto overflowBytes=offsetof(Overflow,values)+physics.overflowDirtyCount*sizeof(GmVec3);
  const auto overflow=output.allocate(Kind::Opaque,static_cast<std::uint32_t>(overflowBytes),1);
  if (output.error!=Error::None) return {};
  *reinterpret_cast<CudaCandidatePhysicsState *>(output.bytes+physical.handle)=physics.state;
  memcpy(output.bytes+overflow.handle,&physics.extended.collisionReplacementOverflow,overflowBytes);
  auto *fields=output.items(record);
  fields[0]=number(physics.nativeVersions); fields[1]=extension; fields[2]=physical; fields[3]=overflow;
  for (std::uint32_t i=0;i<PropertyCount;++i) {
    view.current[i]=physics.current.properties[i];
    view.previous[i]=physics.previous.properties[i];
  }
  return record;
}

template<bool OrderedEllipsoids,CudaHandlingSpecialization Handling=CudaHandlingSpecialization::Generic>
__global__ void PredictPhysicsCursor(ScheduledLane<OrderedEllipsoids> *lanes,std::uint32_t ticks,
    CudaPhysicsCursorState *views,unsigned char *outputBytes,CursorOutput *result) {
  auto &context=lanes[0];
  auto &physics=context.physics;
  auto &machine=context.machine;
  const auto parameters=physics.parameters;
  auto state=physics.state;
  auto scratch=context.scratch;
  Memory output;
  output.bytes=outputBytes; output.capacity=parameters.outputBytes;
  const auto natives=output.allocate(Kind::List,ticks);
  std::uint32_t count=0;
  while (count<ticks && machine.error==Error::None && output.error==Error::None) {
    physics.apply(Op::Step,machine,nullptr,0);
    physics.template advanceTicks<Handling>(machine,state,scratch,parameters);
    physics.state=state;
    if (machine.error!=Error::None) break;
    const auto record=ExportPhysicsCursor(context,output,views[count]);
    if (machine.error!=Error::None || output.error!=Error::None) break;
    output.items(natives)[count]=record;
    ++count;
  }
  context.scratch=scratch;
  auto error=machine.error==Error::None ? machine.memory.error : machine.error;
  if (output.error!=Error::None) error=output.error;
  if (machine.memory.error!=Error::None || output.error!=Error::None) count=0;
  *result={natives,count,output.used,error};
}

} // namespace forevertas::blocks::cuda_program_detail

#endif
