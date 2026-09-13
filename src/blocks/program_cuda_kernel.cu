#include "blocks/program_cuda_device.cuh"

namespace forevertas::blocks {
namespace {
using namespace forevervalidator::simulation;
using namespace vm;
using namespace cuda_program_detail;

#if CUDART_VERSION >= 12040
__global__ void SetScheduleCondition(cudaGraphConditionalHandle handle,const ScheduleStatus *pending) {
  cudaGraphSetConditional(handle,(pending->lanes!=0 || pending->copies!=0) && pending->hostMath==0);
}
#endif

__global__ void InitializeArenas(const unsigned char *initial,unsigned char *arenas,
    std::uint32_t initialBytes,std::uint32_t arenaBytes,std::uint32_t blocksPerLane) {
  const auto lane=blockIdx.x/blocksPerLane;
  const auto first=(blockIdx.x%blocksPerLane)*4096u;
  const auto end=min(first+4096u,initialBytes);
  auto *bytes=arenas+static_cast<std::size_t>(lane)*arenaBytes;
  for (auto i=first+threadIdx.x;i<end;i+=blockDim.x) bytes[i]=initial[i];
}

template<bool OrderedEllipsoids>
__global__ void ExecutePrograms(KernelParameters parameters,unsigned char *arenas,unsigned char *outputs,
    Value *stacks,CudaCandidateState *states,LaneOutput *results) {
  const auto lane=blockIdx.x*blockDim.x+threadIdx.x;
  if (lane>=parameters.count) return;
  auto *bytes=arenas+static_cast<std::size_t>(lane)*parameters.arenaBytes;
  const auto &storage=parameters.collisionStorage;
  cuda::collision::CudaCollisionSearchScratch scratch{0,0,0,false,true,
      storage.collisions,storage.shapeCollisions,storage.shapeWorld,storage.movingBounds,
      storage.surfaceHits,storage.meshRanges,storage.meshCells,lane,parameters.count,storage.shapeCount};
  scratch.responseOrderStorage=storage.responseOrder;
  DevicePhysics<OrderedEllipsoids> physics(parameters,states[lane],scratch);
  Machine<DevicePhysics<OrderedEllipsoids>> machine(parameters.program,physics);
  machine.memory.bytes=bytes; machine.memory.capacity=parameters.arenaBytes; machine.memory.used=parameters.initialBytes;
  machine.memory.sharedBytes=parameters.sharedArena;
  machine.globals=parameters.globals; machine.stack=stacks+lane*1024u; machine.stackCapacity=1024u;
  machine.tickMs=parameters.tickMs; machine.batchSize=parameters.batchSize;
  physics.initialize(machine);
  auto result=machine.execute(parameters.entry,machine.memory.copy(parameters.arguments[lane]),parameters.seeds[lane]);
  if (result.error==Error::None) {
    physics.flush(machine);
    result.error=machine.error;
  }
  Memory output;
  output.bytes=outputs+static_cast<std::size_t>(lane)*parameters.outputBytes; output.capacity=parameters.outputBytes;
  const auto sourceValue=result.value;
  if (result.error==Error::None) {
    result.value=CompactValue(machine.memory,output,result.value);
    if (output.error!=Error::None) result.error=output.error;
  }
  results[lane]={result,output.used,machine.memory.used,physics.stepped,physics.skipped,
      output.error==Error::Capacity,0,0,physics.nativeVersions,sourceValue};
}



template<bool OrderedEllipsoids>
__global__ void ResumePrograms(KernelParameters parameters,unsigned char *arenas,unsigned char *outputs,
    Value *stacks,CudaCandidateState *states,LaneOutput *results,ScheduledLane<OrderedEllipsoids> *lanes,
    ScheduleStatus *pending,bool initialize) {
  const auto lane=blockIdx.x*blockDim.x+threadIdx.x;
  if (lane>=parameters.count) return;
  const auto started=parameters.profile ? clock64() : 0;
  auto &context=lanes[lane];
  if (initialize) {
    InitializeLane(parameters,arenas,stacks,states,context,lane,false);
    auto &machine=context.machine;
    machine.begin(parameters.entry,machine.memory.copy(parameters.arguments[lane]),parameters.seeds[lane]);
  }
  if (context.finished) return;
  auto &machine=context.machine;
  auto &physics=context.physics;
  if (!context.returned) {
    context.returned=machine.resume([&](const Instruction &instruction) { return physics.pauseBefore(instruction,machine); });
    if (context.returned) context.result=machine.result();
  }
  if (machine.error==Error::None && physics.mathRequest->state==1) {
    if (parameters.profile) context.vmCycles+=clock64()-started;
    atomicAdd(&pending->hostMath,1u);
    return;
  }
  if (machine.error==Error::None && (physics.pendingTicks || physics.hasNativeCopies())) {
    if (parameters.profile) context.vmCycles+=clock64()-started;
    if (physics.pendingTicks) {
      atomicAdd(&pending->lanes,1u); atomicAdd(&pending->ticks,physics.pendingTicks);
    }
    if (physics.hasNativeCopies()) atomicAdd(&pending->copies,1u);
    return;
  }
  if (machine.error!=Error::None) {
    physics.releaseNativeCopies(machine);
    context.result={Value{},machine.error,machine.source,machine.operations};
  }
  Memory output;
  output.bytes=outputs+static_cast<std::size_t>(lane)*parameters.outputBytes; output.capacity=parameters.outputBytes;
  const auto sourceValue=context.result.value;
  if (context.result.error==Error::None) {
    context.result.value=CompactValue(machine.memory,output,context.result.value);
    if (output.error!=Error::None) context.result.error=output.error;
  }
  if (parameters.profile) context.vmCycles+=clock64()-started;
  results[lane]={context.result,output.used,machine.memory.used,physics.stepped,physics.skipped,
      output.error==Error::Capacity,context.vmCycles,context.physicsCycles,physics.nativeVersions,sourceValue};
  context.finished=true;
}



__global__ void RecompactPrograms(KernelParameters parameters,unsigned char *arenas,unsigned char *outputs,LaneOutput *results) {
  const auto lane=blockIdx.x*blockDim.x+threadIdx.x;
  if (lane>=parameters.count) return;
  auto &result=results[lane];
  if (result.result.error!=Error::None && !result.outputCapacityExceeded) return;
  const auto cancellation=*reinterpret_cast<const volatile std::uint32_t *>(parameters.cancellation);
  if (cancellation) {
    result.result.error=cancellation==2 ? Error::Interpreter : Error::Cancelled;
    result.outputCapacityExceeded=false;
    return;
  }
  Memory source;
  source.bytes=arenas+static_cast<std::size_t>(lane)*parameters.arenaBytes;
  source.sharedBytes=parameters.sharedArena;
  source.capacity=parameters.arenaBytes; source.used=result.workingBytes;
  // Forwarding links belong to the discarded output arena, not the source
  // values. Clear only visited objects; shared children are visited once.
  if (!ClearCompactionLinks(source,result.sourceValue)) {
    result.result.error=Error::Recursion; result.outputCapacityExceeded=false;
    return;
  }
  Memory output;
  output.bytes=outputs+static_cast<std::size_t>(lane)*parameters.outputBytes;
  output.capacity=parameters.outputBytes;
  result.result.value=CompactValue(source,output,result.sourceValue);
  result.result.error=output.error;
  result.bytes=output.used;
  result.outputCapacityExceeded=output.error==Error::Capacity;
}

template<bool OrderedEllipsoids>
__global__ void CopyNativeSnapshots(ScheduledLane<OrderedEllipsoids> *lanes,std::uint32_t count) {
  const auto lane=blockIdx.x;
  if (lane>=count || lanes[lane].finished) return;
  auto &physics=lanes[lane].physics;
  auto &machine=lanes[lane].machine;
  if (!physics.hasNativeCopies()) return;
  const auto request=*physics.nativeCopies;
  const auto copy=[&](std::uint32_t handle,const void *source,std::uint32_t bytes) {
    if (!handle) return;
    const auto *input=static_cast<const unsigned char *>(source);
    for (std::uint32_t i=threadIdx.x;i<bytes;i+=blockDim.x)
      machine.memory.bytes[handle+i]=input[i];
  };
  copy(request.extension,&physics.extended,sizeof(CudaCandidateState));
  copy(request.physical,&physics.state,sizeof(CudaCandidatePhysicsState));
  copy(request.overflow,&physics.extended.collisionReplacementOverflow,request.overflowBytes);
}

template<bool OrderedEllipsoids,CudaHandlingSpecialization Handling=CudaHandlingSpecialization::Generic>
__global__ void AdvancePrograms(ScheduledLane<OrderedEllipsoids> *lanes,std::uint32_t count) {
  const auto lane=blockIdx.x*blockDim.x+threadIdx.x;
  if (lane>=count || lanes[lane].finished) return;
  auto &physics=lanes[lane].physics;
  auto &machine=lanes[lane].machine;
  // The preceding copy kernel has finished all payload writes. Release its
  // retained handles here, with one thread per lane, before mutating physics.
  physics.releaseNativeCopies(machine);
  if (!physics.pendingTicks) return;
  const auto started=physics.parameters.profile ? clock64() : 0;
  if (machine.error==Error::None) physics.reusePrefix(machine);
  if (machine.error!=Error::None || !physics.pendingTicks) {
    if (physics.parameters.profile) lanes[lane].physicsCycles+=clock64()-started;
    return;
  }
  auto state=physics.state;
  auto scratch=lanes[lane].scratch;
  const auto parameters=physics.parameters;
  physics.template advanceTicks<Handling>(machine,state,scratch,parameters);
  physics.state=state;
  lanes[lane].scratch=scratch;
  if (parameters.profile) lanes[lane].physicsCycles+=clock64()-started;
}

const void *SelectAdvanceKernel(bool ordered,const CudaPackedStaticConfigurationHeader &configuration) {
  if (!ordered) return reinterpret_cast<const void *>(AdvancePrograms<false>);
  // These are immutable physics configuration facts, not properties of the
  // mapped source procedure. Keep the generic kernel for other layouts.
  switch (configuration.tuning.handlingModel) {
  case static_cast<std::uint32_t>(CSceneVehicleCarHandlingModel_Standard):
  case static_cast<std::uint32_t>(CSceneVehicleCarHandlingModel_Lateral):
    return reinterpret_cast<const void *>(AdvancePrograms<true,CudaHandlingSpecialization::Legacy>);
  case static_cast<std::uint32_t>(CSceneVehicleCarHandlingModel_GearedDrive):
    return configuration.water.present
      ? reinterpret_cast<const void *>(AdvancePrograms<true,CudaHandlingSpecialization::GearedDriveWater>)
      : reinterpret_cast<const void *>(AdvancePrograms<true,CudaHandlingSpecialization::GearedDriveDry>);
  default: return reinterpret_cast<const void *>(AdvancePrograms<true>);
  }
}



void Check(cudaError_t status,const char *operation) {
  if (status==cudaErrorMemoryAllocation) throw std::bad_alloc();
  if (status!=cudaSuccess) throw std::runtime_error(std::string(operation)+": "+cudaGetErrorString(status));
}
struct Buffer {
  void *data=nullptr; std::size_t bytes=0;
  ~Buffer() { if (data) cudaFree(data); }
  void reserve(std::size_t size) {
    if (size<=bytes) return;
    if (data) { Check(cudaFree(data),"Freeing CUDA program buffer"); data=nullptr; bytes=0; }
    Check(cudaMalloc(&data,size),"Allocating CUDA program buffer"); bytes=size;
  }
  template<class T> T *get() { return static_cast<T *>(data); }
};
}

struct CudaProgramKernel::Impl {
  Buffer code,procedures,parameters,initial,sharedInitial,arguments,seeds,origin,ticks,arenas,outputs,stacks,states,results,cancellation;
  Buffer historySizes,historyReachable,restoreReachable;
  Buffer collisions,shapeCollisions,shapeWorld,movingBounds,surfaceHits,meshRanges,meshCells,responseOrder;
  Buffer checkpoints,checkpointInputs,checkpointExtensions;
  CheckpointStaging checkpointStaging;
  Buffer scheduledLanes,pending,mathRequests,nativeCopies,cursorOutput,cursorViews;
  struct RestoreStorage {
    std::shared_ptr<const CudaProgramRestoreContext> source;
    Buffer origin,ticks,checkpoints,extensions,inputs;
    CheckpointStaging staging;
  };
  Buffer restoreContexts,restoreContextIds;
  std::vector<RestoreContext> restoreDescriptors;
  std::vector<std::unique_ptr<RestoreStorage>> restoreStorage;
  std::vector<std::unique_ptr<RestoreStorage>> retiredRestoreStorage;
  KernelParameters cursorParameters{};
  bool cursorActive=false;
  bool cursorFailed=false;
  std::uint32_t cursorTicks=0;
  const void *configuration=nullptr;
  CudaPackedStaticConfigurationHeader configurationHeader{};
  bool orderedEllipsoids=false;
  cudaStream_t stream=nullptr,cancelStream=nullptr;
  cudaEvent_t done=nullptr,started=nullptr;
  std::uint32_t *hostCancel=nullptr;
  Impl() {
    try {
    Check(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking),"Creating CUDA program stream");
    Check(cudaStreamCreateWithFlags(&cancelStream,cudaStreamNonBlocking),"Creating CUDA cancellation stream");
    if (std::getenv("FOREVERTAS_CUDA_VM_PROFILE"))
      Check(cudaEventCreate(&started),"Creating CUDA profiling event");
    Check(cudaEventCreateWithFlags(&done,started ? cudaEventDefault : cudaEventDisableTiming),"Creating CUDA completion event");
    Check(cudaHostAlloc(&hostCancel,sizeof(std::uint32_t),cudaHostAllocDefault),"Allocating CUDA cancellation flag");
    cancellation.reserve(sizeof(std::uint32_t));
    } catch (...) {
      release();
      throw;
    }
  }
  ~Impl() { release(); }
  void release() noexcept {
    if (stream) cudaStreamSynchronize(stream);
    if (hostCancel) cudaFreeHost(hostCancel);
    if (done) cudaEventDestroy(done);
    if (started) cudaEventDestroy(started);
    if (stream) cudaStreamDestroy(stream);
    if (cancelStream) cudaStreamDestroy(cancelStream);
  }
  template<class T> void upload(Buffer &buffer,const T *data,std::size_t count) {
    buffer.reserve(std::max<std::size_t>(1,count*sizeof(T)));
    if (count) Check(cudaMemcpyAsync(buffer.data,data,count*sizeof(T),cudaMemcpyHostToDevice,stream),"Uploading CUDA program data");
  }
  KernelParameters prepare(const CudaProgramInput &input);
  void initializeCursor(KernelParameters parameters,bool preserveState);
};

CudaProgramKernel::CudaProgramKernel() : impl_(std::make_unique<Impl>()) {}
CudaProgramKernel::~CudaProgramKernel()=default;

std::size_t CudaProgramKernel::suggestedWaveSize(std::uint32_t arenaCapacity,std::uint32_t outputCapacity) const {
  std::size_t available=0,total=0;
  Check(cudaMemGetInfo(&available,&total),"Querying CUDA program memory");
  const auto &p=*impl_;
  const auto retained=p.arenas.bytes+p.outputs.bytes+p.stacks.bytes+p.states.bytes+p.results.bytes+
      p.collisions.bytes+p.shapeCollisions.bytes+p.shapeWorld.bytes+p.movingBounds.bytes+
      p.surfaceHits.bytes+p.meshRanges.bytes+p.meshCells.bytes+p.responseOrder.bytes+p.scheduledLanes.bytes+p.nativeCopies.bytes;
  const std::size_t perLane=arenaCapacity+outputCapacity+1024u*sizeof(Value)+sizeof(CudaCandidateState)+
      128u*1024u+sizeof(LaneOutput)+sizeof(ScheduledLane<true>)+sizeof(NativeCopyRequest);
  // Leave half of the usable memory for the driver, physics and concurrent
  // viewer allocations. Wave splitting must not change the logical batch.
  auto lanes=std::min<std::size_t>(1024,(available+retained)/2/perLane);
  if (const auto *text=std::getenv("FOREVERTAS_CUDA_VM_MAX_LANES")) {
    std::size_t limit=0;
    const auto end=text+std::strlen(text);
    const auto parsed=std::from_chars(text,end,limit);
    if (parsed.ec==std::errc{} && parsed.ptr==end && limit) lanes=std::min(lanes,limit);
  }
  return lanes>=32 ? lanes/32*32 : std::max<std::size_t>(1,lanes);
}

KernelParameters CudaProgramKernel::Impl::prepare(const CudaProgramInput &input) {
  auto &p=*this;
  p.cursorActive=false;
  const auto count=static_cast<std::uint32_t>(input.arguments.size());
  if (!count || input.seeds.size()!=count || !input.program || !input.context || input.arena.size()>input.arenaCapacity)
    throw std::invalid_argument("Invalid CUDA program batch.");
  if (!input.historySizes.empty() && input.historySizes.size()!=static_cast<std::size_t>(input.importedSnapshotCount)+1)
    throw std::invalid_argument("Invalid CUDA history contexts.");
  const auto &program=*input.program; const auto &context=*input.context;
  p.upload(p.code,program.code.data(),program.code.size()); p.upload(p.procedures,program.procedures.data(),program.procedures.size());
  p.upload(p.parameters,program.arguments.data(),program.arguments.size());
  p.upload(p.historySizes,input.historySizes.data(),input.historySizes.size());
  p.upload(p.historyReachable,input.historyReachable.data(),input.historyReachable.size());
  p.upload(p.restoreReachable,input.restoreReachable.data(),input.restoreReachable.size());
  p.upload(p.initial,input.arena.data(),input.arena.size()); p.upload(p.arguments,input.arguments.data(),count);
  if (input.sharedArena) p.upload(p.sharedInitial,input.sharedArena->data(),input.sharedArena->size());
  p.upload(p.seeds,input.seeds.data(),count); p.upload(p.origin,&context.physics.initialState,1);
  p.upload(p.ticks,context.ticks.data(),context.ticks.size());
  const auto checkpointCount=input.checkpoints ? input.checkpoints->size() : 0;
  p.checkpointStaging.assign(input.checkpoints.get(),input.checkpointInputs.get());
  p.upload(p.checkpoints,p.checkpointStaging.checkpoints.data(),p.checkpointStaging.checkpoints.size());
  p.upload(p.checkpointExtensions,p.checkpointStaging.extensions.data(),p.checkpointStaging.extensions.size());
  if (p.started && checkpointCount)
    std::fprintf(stderr,"CUDA_VM_PREFIX_UPLOAD_PROFILE checkpoints=%zu extensions=%zu bytes=%zu unpackedBytes=%zu\n",
        checkpointCount,p.checkpointStaging.extensions.size(),
        p.checkpointStaging.checkpoints.size()*sizeof(PackedCheckpoint)+p.checkpointStaging.extensions.size()*sizeof(CudaCandidateState),
        checkpointCount*sizeof(CudaProgramCheckpoint));
  p.upload(p.checkpointInputs,input.checkpointInputs ? input.checkpointInputs->data() : nullptr,
      input.checkpointInputs ? input.checkpointInputs->size() : 0);
  const auto outputBytes=input.outputCapacity;
  const auto size=static_cast<std::size_t>(count);
  p.arenas.reserve(size*input.arenaCapacity); p.outputs.reserve(size*outputBytes);
  p.stacks.reserve(size*1024*sizeof(Value)); p.states.reserve(size*sizeof(CudaCandidateState));
  p.results.reserve(size*sizeof(LaneOutput));
  // Opaque snapshots include native padding. Define arena storage once rather
  // than clearing each snapshot's payload on every interpreter iteration.
  Check(cudaMemsetAsync(p.arenas.data,0,size*input.arenaCapacity,p.stream),"Initializing CUDA working bytes");
  // Serialization includes allocator slack and descriptor padding.
  Check(cudaMemsetAsync(p.outputs.data,0,size*outputBytes,p.stream),"Initializing CUDA output bytes");
  Check(cudaMemsetAsync(p.results.data,0,size*sizeof(LaneOutput),p.stream),"Initializing CUDA output descriptors");
  if (p.configuration!=context.physics.deviceStaticConfiguration) {
    Check(cudaMemcpy(&p.configurationHeader,context.physics.deviceStaticConfiguration,sizeof(p.configurationHeader),cudaMemcpyDeviceToHost),"Reading CUDA collision layout");
    const auto &section=p.configurationHeader.collisionShapes;
    p.orderedEllipsoids=section.count==8 && section.stride==sizeof(CudaVehicleCollisionShape);
    if (p.orderedEllipsoids) {
      CudaVehicleCollisionShape shapes[8];
      Check(cudaMemcpy(shapes,static_cast<const unsigned char *>(context.physics.deviceStaticConfiguration)+section.offset,
          sizeof(shapes),cudaMemcpyDeviceToHost),"Reading CUDA collision shape order");
      for (std::uint32_t i=0;i<8;++i)
        p.orderedEllipsoids=p.orderedEllipsoids && shapes[i].traversalOrder==i &&
            shapes[i].surfaceType==static_cast<std::uint32_t>(GmSurf::EGmSurfType::Ellipsoid);
    }
    p.configuration=context.physics.deviceStaticConfiguration;
  }
  using namespace cuda::collision;
  const auto tiles=(size+CudaCollisionSearchTileWidth-1)/CudaCollisionSearchTileWidth;
  const auto shapeCount=static_cast<std::uint32_t>(p.configurationHeader.collisionShapes.count);
  p.collisions.reserve(tiles*CollisionCapacity*sizeof(CudaCollisionSearchTile));
  p.shapeCollisions.reserve(tiles*ShapeCollisionCapacity*sizeof(CudaCollisionSearchTile));
  p.shapeWorld.reserve(size*shapeCount*sizeof(GmIso4)); p.movingBounds.reserve(size*shapeCount*sizeof(GmBoxAligned));
  p.surfaceHits.reserve(size*SurfaceHitCapacity*sizeof(CudaCollisionSurfaceHit));
  p.meshRanges.reserve(size*SurfaceHitCapacity*sizeof(CudaCollisionMeshRange));
  p.meshCells.reserve(size*MeshCellHitCapacity*sizeof(std::uint32_t));
  p.responseOrder.reserve(size*CollisionCapacity*sizeof(std::uint16_t));
  *p.hostCancel=0; p.upload(p.cancellation,p.hostCancel,1);
  auto view=program.view(); view.code=p.code.get<Instruction>(); view.procedures=p.procedures.get<Procedure>(); view.arguments=p.parameters.get<std::uint32_t>();
  KernelParameters parameters{view,p.origin.get<CudaCandidateState>(),p.ticks.get<CudaControlTick>(),
    static_cast<const CudaPackedSceneHeader *>(context.physics.deviceScene),
    static_cast<const CudaPackedStaticConfigurationHeader *>(context.physics.deviceStaticConfiguration),
    p.initial.get<unsigned char>(),p.arguments.get<Value>(),p.seeds.get<std::uint64_t>(),
    input.globals,input.inputs,input.initialView,input.previousView,
    static_cast<std::uint32_t>(input.arena.size()),input.arenaCapacity,outputBytes,input.tickMs,context.prestartDurationMs,
    static_cast<std::uint32_t>(context.ticks.size()),count,program.entry,input.batchSize,context.state.timeMs,p.cancellation.get<std::uint32_t>(),
    p.checkpoints.get<PackedCheckpoint>(),p.checkpointExtensions.get<CudaCandidateState>(),
    p.checkpointInputs.get<Value>(),static_cast<std::uint32_t>(checkpointCount),
    {p.collisions.get<CudaCollisionSearchTile>(),p.shapeCollisions.get<CudaCollisionSearchTile>(),
     p.shapeWorld.get<GmIso4>(),p.movingBounds.get<GmBoxAligned>(),p.surfaceHits.get<CudaCollisionSurfaceHit>(),
     p.meshRanges.get<CudaCollisionMeshRange>(),p.meshCells.get<std::uint32_t>(),p.responseOrder.get<std::uint16_t>(),shapeCount}};
  parameters.profile=p.started!=nullptr;
  parameters.histories=input.histories;
  parameters.restartSnapshot=input.restartSnapshot;
  parameters.collectionLimit=input.collectionLimit;
  if (!input.historySizes.empty()) parameters.historySizes=p.historySizes.get<std::uint64_t>();
  if (!input.historyReachable.empty() && input.historyReachable.size()==program.code.size())
    parameters.historyReachable=p.historyReachable.get<std::uint8_t>();
  if (!input.restoreReachable.empty() && input.restoreReachable.size()==program.code.size())
    parameters.restoreReachable=p.restoreReachable.get<std::uint8_t>();
  if (input.sharedArena && !input.sharedArena->empty()) parameters.sharedArena=p.sharedInitial.get<unsigned char>();
  return parameters;
}

void CudaProgramKernel::startPhysicsCursor(const CudaProgramInput &input) {
  auto &p=*impl_;
  const auto started=p.started ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  struct Drain { cudaStream_t stream; ~Drain() { cudaStreamSynchronize(stream); } } drain{p.stream};
  if (input.arguments.size()!=1) throw std::invalid_argument("A CUDA physics cursor has exactly one lane.");
  auto parameters=p.prepare(input);
  p.scheduledLanes.reserve(std::max(sizeof(ScheduledLane<true>),sizeof(ScheduledLane<false>)));
  p.mathRequests.reserve(sizeof(MathRequest));
  p.cursorOutput.reserve(sizeof(CursorOutput));
  Check(cudaMemsetAsync(p.cursorOutput.data,0,sizeof(CursorOutput),p.stream),"Initializing CUDA cursor descriptor");
  p.cursorViews.reserve(CudaPhysicsCursorMaximumLookahead*sizeof(CudaPhysicsCursorState));
  parameters.mathRequests=p.mathRequests.get<MathRequest>();
  const auto prepared=p.started ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  p.initializeCursor(parameters,false);
  if (p.started) std::fprintf(stderr,"CUDA_SOURCE_START_PROFILE prepareMilliseconds=%.6f initializeMilliseconds=%.6f\n",
      std::chrono::duration<double,std::milli>(prepared-started).count(),
      std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-prepared).count());
}

void CudaProgramKernel::Impl::initializeCursor(KernelParameters parameters,bool preserveState) {
  auto &p=*this;
  Check(cudaMemcpyAsync(p.arenas.data,p.initial.data,parameters.initialBytes,cudaMemcpyDeviceToDevice,p.stream),
      "Initializing CUDA source values");
  // A single physics lane should not serially copy the large native extension.
  if (!preserveState)
    Check(cudaMemcpyAsync(p.states.data,p.origin.data,sizeof(CudaCandidateState),cudaMemcpyDeviceToDevice,p.stream),
        "Initializing CUDA source native state");
  auto *arenas=p.arenas.data, *stacks=p.stacks.data, *states=p.states.data, *lanes=p.scheduledLanes.data;
  void *arguments[]={&parameters,&arenas,&stacks,&states,&lanes,&preserveState};
  Check(cudaLaunchKernel(SelectCursorInitializeKernel(p.orderedEllipsoids),dim3(1),dim3(1),arguments,0,p.stream),
      "Initializing CUDA source physics");
  Check(cudaStreamSynchronize(p.stream),"Preparing CUDA source physics");
  p.cursorParameters=parameters;
  p.cursorTicks=0; p.cursorFailed=false; p.cursorActive=true;
}

void CudaProgramKernel::updatePhysicsCursor(const CudaProgramInput &input,bool preserveState) {
  auto &p=*impl_;
  struct Drain { cudaStream_t stream; ~Drain() { cudaStreamSynchronize(stream); } } drain{p.stream};
  if (!p.cursorActive || !input.context) throw std::invalid_argument("Invalid CUDA source input update.");
  const auto &context=*input.context;
  auto parameters=p.cursorParameters;
  if (parameters.scene!=context.physics.deviceScene || parameters.configuration!=context.physics.deviceStaticConfiguration)
    throw std::invalid_argument("CUDA source input update changed the scene.");
  if (!preserveState) p.upload(p.origin,&context.physics.initialState,1);
  p.upload(p.ticks,context.ticks.data(),context.ticks.size());
  p.upload(p.initial,input.arena.data(),input.arena.size());
  p.arenas.reserve(input.arenaCapacity);
  parameters.ticks=p.ticks.get<CudaControlTick>();
  parameters.initialArena=p.initial.get<unsigned char>();
  parameters.initialBytes=static_cast<std::uint32_t>(input.arena.size());
  parameters.arenaBytes=input.arenaCapacity;
  parameters.globals=input.globals; parameters.inputs=input.inputs;
  parameters.initialView=input.initialView; parameters.previousView=input.previousView;
  parameters.tickCount=static_cast<std::uint32_t>(context.ticks.size());
  parameters.initialTime=context.state.timeMs;
  p.initializeCursor(parameters,preserveState);
}

CudaPhysicsCursorBatch CudaProgramKernel::predictPhysicsCursor(std::uint32_t ticks) {
  auto &p=*impl_;
  if (!p.cursorActive || p.cursorFailed || !ticks || ticks>CudaPhysicsCursorMaximumLookahead ||
      ticks>p.cursorParameters.tickCount-p.cursorTicks)
    throw std::invalid_argument("Invalid CUDA source physics advance.");
  p.cursorFailed=true;
  auto *lanes=p.scheduledLanes.data;
  auto *views=p.cursorViews.data, *output=p.outputs.data, *header=p.cursorOutput.data;
  void *arguments[]={&lanes,&ticks,&views,&output,&header};
  Check(cudaLaunchKernel(SelectCursorKernel(p.orderedEllipsoids,p.configurationHeader),dim3(1),dim3(1),arguments,0,p.stream),
      "Predicting CUDA source physics");
  CursorOutput descriptor;
  Check(cudaMemcpyAsync(&descriptor,p.cursorOutput.data,sizeof(descriptor),cudaMemcpyDeviceToHost,p.stream),"Reading CUDA source prediction");
  Check(cudaStreamSynchronize(p.stream),"Finishing CUDA source prediction");
  if (descriptor.count>ticks || descriptor.bytes>p.cursorParameters.outputBytes)
    throw std::logic_error("Invalid CUDA source prediction output.");
  CudaPhysicsCursorBatch result;
  result.error=descriptor.error; result.native=descriptor.native;
  result.states.resize(descriptor.count); result.arena.resize(descriptor.bytes);
  if (descriptor.count) {
    Check(cudaMemcpyAsync(result.states.data(),p.cursorViews.data,descriptor.count*sizeof(CudaPhysicsCursorState),cudaMemcpyDeviceToHost,p.stream),
        "Transferring CUDA source views");
    Check(cudaMemcpyAsync(result.arena.data(),p.outputs.data,descriptor.bytes,cudaMemcpyDeviceToHost,p.stream),"Transferring CUDA source snapshots");
  }
  Check(cudaStreamSynchronize(p.stream),"Finishing CUDA source prediction transfer");
  p.cursorTicks+=descriptor.count; p.cursorFailed=descriptor.error!=Error::None;
  return result;
}

std::vector<CudaProgramOutput> CudaProgramKernel::execute(const CudaProgramInput &input,const std::function<bool()> &cancelled) {
  auto &p=*impl_;
  // Uploads reference caller-owned storage. Drain them even when allocation,
  // launch, cancellation or result decoding preparation throws.
  struct Drain { cudaStream_t stream; ~Drain() { cudaStreamSynchronize(stream); } } drain{p.stream};
  auto parameters=p.prepare(input);
  const auto &program=*input.program;
  const auto count=parameters.count;
  const auto size=static_cast<std::size_t>(count);
  auto outputBytes=input.outputCapacity;
  const auto checkpointCount=input.checkpoints ? input.checkpoints->size() : 0;
  const bool hasRestore=std::any_of(program.code.begin(),program.code.end(),
      [](const Instruction &instruction) { return instruction.op==Op::Restore || instruction.op==Op::Restart; });
  const bool hasSetHorizon=std::any_of(program.code.begin(),program.code.end(),
      [](const Instruction &instruction) { return instruction.op==Op::SetHorizon; });
  p.restoreStorage.clear(); p.retiredRestoreStorage.clear(); p.restoreDescriptors.clear();
  if (hasRestore || hasSetHorizon) {
    if (input.importedSnapshotCount==UINT32_MAX) throw std::invalid_argument("Too many imported CUDA snapshots.");
    const auto contexts=static_cast<std::size_t>(input.importedSnapshotCount)+1;
    p.restoreStorage.resize(contexts); p.restoreDescriptors.resize(contexts);
    p.restoreDescriptors[0]={parameters.origin,parameters.ticks,parameters.checkpoints,
        parameters.checkpointExtensions,parameters.checkpointInputs,parameters.initialTime,
        parameters.tickCount,parameters.prestartMs,parameters.checkpointCount};
    p.upload(p.restoreContexts,p.restoreDescriptors.data(),contexts);
    p.restoreContextIds.reserve(size*sizeof(std::uint32_t));
    Check(cudaMemsetAsync(p.restoreContextIds.data,0,size*sizeof(std::uint32_t),p.stream),"Initializing CUDA restore context IDs");
    parameters.restoreContexts=p.restoreContexts.get<RestoreContext>();
    parameters.restoreContextIds=p.restoreContextIds.get<std::uint32_t>();
    parameters.restoreContextCount=static_cast<std::uint32_t>(contexts);
  }
  const auto initializeArenas=[&] {
    // Adjacent threads copy adjacent bytes, rather than one thread walking
    // each lane's entire imported collection with arena-strided warp stores.
    if (parameters.initialBytes) {
      const auto blocksPerLane=(parameters.initialBytes+4095u)/4096u;
      InitializeArenas<<<count*blocksPerLane,256,0,p.stream>>>(parameters.initialArena,p.arenas.get<unsigned char>(),
          parameters.initialBytes,parameters.arenaBytes,blocksPerLane);
    }
    constexpr auto nativeBytes=static_cast<std::uint32_t>(sizeof(CudaCandidateState));
    constexpr auto nativeBlocks=(nativeBytes+4095u)/4096u;
    InitializeArenas<<<count*nativeBlocks,256,0,p.stream>>>(p.origin.get<unsigned char>(),p.states.get<unsigned char>(),
        nativeBytes,nativeBytes,nativeBlocks);
    Check(cudaGetLastError(),"Initializing CUDA program arenas");
  };
  bool cancelSent=false;
  std::exception_ptr pendingError;
  const auto wait=[&] {
    Check(cudaGetLastError(),"Launching block-program CUDA kernel");
    Check(cudaEventRecord(p.done,p.stream),"Recording CUDA program completion");
    for (;;) {
      bool stop=false;
      try { stop=cancelled && cancelled(); } catch (...) { pendingError=std::current_exception(); stop=true; }
      if (stop && !cancelSent) {
        *p.hostCancel=1;
        Check(cudaMemcpyAsync(p.cancellation.data,p.hostCancel,sizeof(std::uint32_t),cudaMemcpyHostToDevice,p.cancelStream),"Cancelling CUDA program");
        Check(cudaStreamSynchronize(p.cancelStream),"Publishing CUDA cancellation");
        cancelSent=true;
      }
      const auto status=cudaEventQuery(p.done);
      if (status==cudaSuccess) break;
      if (status!=cudaErrorNotReady) Check(status,"Executing CUDA program");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (pendingError) std::rethrow_exception(pendingError);
  };
  bool scheduled=std::getenv("FOREVERTAS_CUDA_VM_MONOLITHIC")==nullptr;
  const bool hasHostMath=std::any_of(program.code.begin(),program.code.end(),
      [](const Instruction &instruction) { return requiresHostMath(instruction.op); });
  scheduled=scheduled || hasHostMath || hasRestore || hasSetHorizon;
  const bool hasNativeSnapshots=std::any_of(program.code.begin(),program.code.end(),
      [](const Instruction &instruction) { return instruction.op==Op::Snapshot || instruction.op==Op::Publish; });
  const bool adaptive=std::getenv("FOREVERTAS_CUDA_VM_SCHEDULED")==nullptr;
  bool schedulerFallback=false;
  bool graphScheduled=false;
  bool graphUnavailable=false;
  const auto advanceKernel=SelectAdvanceKernel(p.orderedEllipsoids,p.configurationHeader);
  if (scheduled) {
    p.scheduledLanes.reserve(size*std::max(sizeof(ScheduledLane<true>),sizeof(ScheduledLane<false>)));
    p.pending.reserve(sizeof(ScheduleStatus));
    p.mathRequests.reserve(size*sizeof(MathRequest));
    // Host math reads the entire request array, including inactive lanes and
    // padding that typed device initialization need not write.
    Check(cudaMemsetAsync(p.mathRequests.data,0,size*sizeof(MathRequest),p.stream),"Initializing CUDA math descriptors");
    parameters.mathRequests=p.mathRequests.get<MathRequest>();
    const auto *copyPolicy=std::getenv("FOREVERTAS_CUDA_VM_COOPERATIVE_SNAPSHOTS");
    if (hasNativeSnapshots && (!copyPolicy || std::strcmp(copyPolicy,"0")!=0)) {
      p.nativeCopies.reserve(size*sizeof(NativeCopyRequest));
      parameters.nativeCopies=p.nativeCopies.get<NativeCopyRequest>();
    }
  }
  const auto copyKernel=p.orderedEllipsoids ? reinterpret_cast<const void *>(CopyNativeSnapshots<true>) :
      reinterpret_cast<const void *>(CopyNativeSnapshots<false>);
  std::vector<MathRequest> mathRequests;
  std::uint64_t hostMathWaves=0,hostMathOperations=0;
  const auto resolveMath=[&]() {
    ++hostMathWaves;
    mathRequests.resize(count);
    Check(cudaMemcpy(mathRequests.data(),p.mathRequests.data,size*sizeof(MathRequest),cudaMemcpyDeviceToHost),"Reading CUDA math requests");
    std::vector<std::uint32_t> horizons(p.restoreDescriptors.size());
    for (const auto &request : mathRequests) {
      if (request.state!=1 || (request.op!=Op::SetHorizon && request.op!=Op::Restart)) continue;
      const auto id=static_cast<std::uint32_t>(request.arguments[0].x);
      if (id<horizons.size()) horizons[id]=std::max(horizons[id],static_cast<std::uint32_t>(request.arguments[1].x));
    }
    bool contextsChanged=false;
    for (auto &request : mathRequests) {
      if (request.state!=1) continue;
      ++hostMathOperations;
      if (request.op==Op::Restore || request.op==Op::SetHorizon || request.op==Op::Restart) {
        const auto id=static_cast<std::uint32_t>(request.arguments[0].x);
        const bool extending=request.op==Op::SetHorizon || request.op==Op::Restart;
        request.result={}; request.error=Error::Unsupported;
        if (id<p.restoreDescriptors.size() && (extending ? bool(input.horizonContext) : id && bool(input.restoreContext))) {
          const auto &available=p.restoreDescriptors[id];
          if (!available.origin || (extending && request.arguments[1].x>
              available.initialTime+static_cast<double>(available.tickCount)*input.tickMs)) {
            if (cancelled && cancelled()) { request.error=Error::Cancelled; request.state=2; continue; }
            auto source=extending ? input.horizonContext(id,horizons[id]) : input.restoreContext(id-1);
            if (source && source->reuseBaseline) p.restoreDescriptors[id]=p.restoreDescriptors[0];
            else if (source && source->context) {
              const auto &context=*source->context;
              if (context.physics.deviceScene!=input.context->physics.deviceScene ||
                  context.physics.deviceStaticConfiguration!=input.context->physics.deviceStaticConfiguration)
                throw std::invalid_argument("CUDA restore context belongs to another scene.");
              auto storage=std::make_unique<Impl::RestoreStorage>();
              storage->source=std::move(source);
              // Keep upload sources alive in the kernel owner even if a later
              // allocation or upload throws before the execution drain runs.
              // Other lanes may still be executing the previous plan version.
              if (p.restoreStorage[id]) p.retiredRestoreStorage.push_back(std::move(p.restoreStorage[id]));
              p.restoreStorage[id]=std::move(storage);
              auto &saved=*p.restoreStorage[id];
              saved.staging.assign(saved.source->checkpoints.get(),saved.source->checkpointInputs.get());
              p.upload(saved.origin,&context.physics.initialState,1);
              p.upload(saved.ticks,context.ticks.data(),context.ticks.size());
              p.upload(saved.checkpoints,saved.staging.checkpoints.data(),saved.staging.checkpoints.size());
              p.upload(saved.extensions,saved.staging.extensions.data(),saved.staging.extensions.size());
              p.upload(saved.inputs,saved.source->checkpointInputs ? saved.source->checkpointInputs->data() : nullptr,
                  saved.source->checkpointInputs ? saved.source->checkpointInputs->size() : 0);
              p.restoreDescriptors[id]={saved.origin.get<CudaCandidateState>(),saved.ticks.get<CudaControlTick>(),
                  saved.checkpoints.get<PackedCheckpoint>(),saved.extensions.get<CudaCandidateState>(),saved.inputs.get<Value>(),
                  context.state.timeMs,static_cast<std::uint32_t>(context.ticks.size()),context.prestartDurationMs,
                  static_cast<std::uint32_t>(saved.staging.checkpoints.size())};
            }
            contextsChanged=contextsChanged || p.restoreDescriptors[id].origin;
          }
          if (p.restoreDescriptors[id].origin) request.error=Error::None;
        }
      } else {
        const auto result=EvaluateCudaProgramHostMath(request.op,request.arguments);
        request.result=result.value;
        request.error=result.error;
      }
      request.state=2;
    }
    // Upload once after all changes so asynchronous DMA never reads a host
    // descriptor while another lane's horizon request is replacing it.
    if (contextsChanged)
      Check(cudaMemcpyAsync(p.restoreContexts.data,p.restoreDescriptors.data(),p.restoreDescriptors.size()*sizeof(RestoreContext),
          cudaMemcpyHostToDevice,p.stream),"Uploading CUDA restore contexts");
    Check(cudaMemcpyAsync(p.mathRequests.data,mathRequests.data(),size*sizeof(MathRequest),cudaMemcpyHostToDevice,p.stream),"Returning exact host math");
  };
  if (p.started) Check(cudaEventRecord(p.started,p.stream),"Recording CUDA program start");
#if CUDART_VERSION >= 12040
  const auto *graphPolicy=std::getenv("FOREVERTAS_CUDA_VM_GRAPH");
  if (scheduled && (!graphPolicy || std::strcmp(graphPolicy,"0")!=0)) {
    struct GraphUnavailable {};
    try {
    const auto graphCheck=[](cudaError_t status,const char *operation) {
      if (status==cudaErrorNotSupported || status==cudaErrorCallRequiresNewerDriver) throw GraphUnavailable{};
      Check(status,operation);
    };
    // The same resumable VM/physics scheduler runs entirely on the device.
    // A conditional graph removes a CPU round trip at every dynamic read.
    struct Graph {
      cudaGraph_t graph=nullptr;
      cudaGraphExec_t executable=nullptr;
      cudaStream_t stream;
      ~Graph() {
        cudaStreamSynchronize(stream);
        if (executable) cudaGraphExecDestroy(executable);
        if (graph) cudaGraphDestroy(graph);
      }
    } graph{nullptr,nullptr,p.stream};
    graphCheck(cudaGraphCreate(&graph.graph,0),"Creating CUDA program graph");
    cudaGraphConditionalHandle handle{};
    graphCheck(cudaGraphConditionalHandleCreate(&handle,graph.graph,0,cudaGraphCondAssignDefault),"Creating CUDA scheduler condition");
    const auto addKernel=[&](cudaGraph_t target,cudaGraphNode_t dependency,const void *function,void **arguments,
                             std::uint32_t blocks,std::uint32_t threads) {
      cudaKernelNodeParams params{};
      params.func=const_cast<void *>(function); params.kernelParams=arguments;
      params.gridDim=dim3(blocks); params.blockDim=dim3(threads);
      cudaGraphNode_t node{};
      graphCheck(cudaGraphAddKernelNode(&node,target,dependency ? &dependency : nullptr,dependency ? 1 : 0,&params),"Adding CUDA scheduler kernel");
      return node;
    };
    const auto reset=[&](cudaGraph_t target) {
      cudaMemsetParams params{};
      params.dst=p.pending.data; params.elementSize=1; params.width=sizeof(ScheduleStatus); params.height=1;
      cudaGraphNode_t node{};
      graphCheck(cudaGraphAddMemsetNode(&node,target,nullptr,0,&params),"Adding CUDA scheduler reset");
      return node;
    };
    const auto resumeKernel=p.orderedEllipsoids ? reinterpret_cast<const void *>(ResumePrograms<true>) :
        reinterpret_cast<const void *>(ResumePrograms<false>);
    auto *arenas=p.arenas.data, *outputs=p.outputs.data, *stacks=p.stacks.data, *states=p.states.data;
    auto *results=p.results.data, *lanes=p.scheduledLanes.data, *pending=p.pending.data;
    bool initialize=true;
    auto activeCount=count;
    void *resumeArguments[]={&parameters,&arenas,&outputs,&stacks,&states,&results,&lanes,&pending,&initialize};
    void *conditionArguments[]={&handle,&pending};
    void *advanceArguments[]={&lanes,&activeCount};
    auto node=reset(graph.graph);
    const auto rootResume=addKernel(graph.graph,node,resumeKernel,resumeArguments,(count+31)/32,32);
    node=rootResume;
    node=addKernel(graph.graph,node,reinterpret_cast<const void *>(SetScheduleCondition),conditionArguments,1,1);
    cudaGraphNodeParams condition{};
    condition.type=cudaGraphNodeTypeConditional;
    condition.conditional.handle=handle; condition.conditional.type=cudaGraphCondTypeWhile; condition.conditional.size=1;
    cudaGraphNode_t loop{};
#if CUDART_VERSION >= 13000
    graphCheck(cudaGraphAddNode(&loop,graph.graph,&node,nullptr,1,&condition),"Adding CUDA scheduler loop");
#else
    graphCheck(cudaGraphAddNode(&loop,graph.graph,&node,1,&condition),"Adding CUDA scheduler loop");
#endif
    const auto body=condition.conditional.phGraph_out[0];
    node=reset(body);
    if (parameters.nativeCopies) node=addKernel(body,node,copyKernel,advanceArguments,count,256);
    node=addKernel(body,node,advanceKernel,advanceArguments,(count+31)/32,32);
    initialize=false;
    node=addKernel(body,node,resumeKernel,resumeArguments,(count+31)/32,32);
    addKernel(body,node,reinterpret_cast<const void *>(SetScheduleCondition),conditionArguments,1,1);
    graphCheck(cudaGraphInstantiateWithFlags(&graph.executable,graph.graph,0),"Instantiating CUDA scheduler graph");
    initializeArenas();
    for (;;) {
      graphCheck(cudaGraphLaunch(graph.executable,p.stream),"Launching CUDA scheduler graph");
      wait();
      ScheduleStatus status{};
      Check(cudaMemcpy(&status,p.pending.data,sizeof(status),cudaMemcpyDeviceToHost),"Reading CUDA graph scheduler");
      if (!status.hostMath) break;
      resolveMath();
      // Other lanes may have paused for physics while this wave requested math.
      if (parameters.nativeCopies)
        Check(cudaLaunchKernel(copyKernel,dim3(count),dim3(256),advanceArguments,0,p.stream),"Copying CUDA math-wave snapshots");
      Check(cudaLaunchKernel(advanceKernel,dim3((count+31)/32),dim3(32),advanceArguments,0,p.stream),"Advancing CUDA math wave");
      cudaKernelNodeParams resumeParams{};
      resumeParams.func=const_cast<void *>(resumeKernel); resumeParams.kernelParams=resumeArguments;
      resumeParams.gridDim=dim3((count+31)/32); resumeParams.blockDim=dim3(32);
      graphCheck(cudaGraphExecKernelNodeSetParams(graph.executable,rootResume,&resumeParams),"Resuming CUDA math wave");
    }
    graphScheduled=true;
    } catch (const GraphUnavailable &) {
      graphUnavailable=true;
    }
  }
#endif
  if (scheduled && !graphScheduled) {
    initializeArenas();
    bool initialize=true;
    for (;;) {
      Check(cudaMemsetAsync(p.pending.data,0,sizeof(ScheduleStatus),p.stream),"Resetting CUDA program scheduler");
      if (p.orderedEllipsoids)
        ResumePrograms<true><<<(count+31)/32,32,0,p.stream>>>(parameters,p.arenas.get<unsigned char>(),p.outputs.get<unsigned char>(),
          p.stacks.get<Value>(),p.states.get<CudaCandidateState>(),p.results.get<LaneOutput>(),p.scheduledLanes.get<ScheduledLane<true>>(),p.pending.get<ScheduleStatus>(),initialize);
      else
        ResumePrograms<false><<<(count+31)/32,32,0,p.stream>>>(parameters,p.arenas.get<unsigned char>(),p.outputs.get<unsigned char>(),
          p.stacks.get<Value>(),p.states.get<CudaCandidateState>(),p.results.get<LaneOutput>(),p.scheduledLanes.get<ScheduledLane<false>>(),p.pending.get<ScheduleStatus>(),initialize);
      wait();
      ScheduleStatus pending{};
      Check(cudaMemcpy(&pending,p.pending.data,sizeof(pending),cudaMemcpyDeviceToHost),"Reading CUDA program scheduler");
      if (!pending.lanes && !pending.hostMath && !pending.copies) break;
      if (pending.hostMath) resolveMath();
      // Replay an uncommitted wave once when its first observation is too
      // frequent to amortize launches. Arguments and RNG seeds are unchanged.
      if (initialize && adaptive && !hasHostMath && !hasRestore && !hasSetHorizon && pending.ticks<pending.lanes*32u) {
        scheduled=false; schedulerFallback=true; break;
      }
      auto *laneStorage=p.scheduledLanes.data;
      auto activeCount=count;
      void *arguments[]={&laneStorage,&activeCount};
      if (parameters.nativeCopies)
        Check(cudaLaunchKernel(copyKernel,dim3(count),dim3(256),arguments,0,p.stream),"Copying scheduled CUDA snapshots");
      Check(cudaLaunchKernel(advanceKernel,dim3((count+31)/32),dim3(32),arguments,0,p.stream),"Launching scheduled CUDA physics");
      wait();
      initialize=false;
    }
  }
  if (!scheduled) {
    initializeArenas();
    if (p.orderedEllipsoids)
      ExecutePrograms<true><<<(count+31)/32,32,0,p.stream>>>(parameters,p.arenas.get<unsigned char>(),p.outputs.get<unsigned char>(),
        p.stacks.get<Value>(),p.states.get<CudaCandidateState>(),p.results.get<LaneOutput>());
    else
      ExecutePrograms<false><<<(count+31)/32,32,0,p.stream>>>(parameters,p.arenas.get<unsigned char>(),p.outputs.get<unsigned char>(),
        p.stacks.get<Value>(),p.states.get<CudaCandidateState>(),p.results.get<LaneOutput>());
    wait();
  }
  std::vector<LaneOutput> headers(count);
  Check(cudaMemcpy(headers.data(),p.results.data,size*sizeof(LaneOutput),cudaMemcpyDeviceToHost),"Reading CUDA program results");
  std::uint32_t recompactions=0;
  bool outputBudgetLimited=false;
  const bool workingCapacityExceeded=std::any_of(headers.begin(),headers.end(),[](const auto &header) {
    return header.result.error==Error::Capacity && !header.outputCapacityExceeded;
  });
  // Output exhaustion does not invalidate completed VM state. Only a working
  // arena failure or a reduced VRAM wave budget still needs wave execution.
  const auto outputLimit=std::min(input.outputCapacityLimit,64u*1024u*1024u);
  while (!cancelSent && !workingCapacityExceeded && outputBytes<outputLimit &&
      std::any_of(headers.begin(),headers.end(),[](const auto &header) { return header.outputCapacityExceeded; })) {
    const auto nextCapacity=std::min(std::max(outputBytes,32u)*2,outputLimit);
    if (suggestedWaveSize(input.arenaCapacity,nextCapacity)<count) {
      outputBudgetLimited=true;
      break;
    }
    outputBytes=nextCapacity;
    p.outputs.reserve(size*outputBytes);
    Check(cudaMemsetAsync(p.outputs.data,0,size*outputBytes,p.stream),"Initializing expanded CUDA output bytes");
    parameters.outputBytes=outputBytes;
    RecompactPrograms<<<(count+31)/32,32,0,p.stream>>>(parameters,p.arenas.get<unsigned char>(),
        p.outputs.get<unsigned char>(),p.results.get<LaneOutput>());
    wait();
    ++recompactions;
    Check(cudaMemcpy(headers.data(),p.results.data,size*sizeof(LaneOutput),cudaMemcpyDeviceToHost),"Reading recompact results");
  }
  if (p.started) {
    float milliseconds=0;
    std::fprintf(stderr,"CUDA_VM_MATH_PROFILE hostWaves=%llu hostOperations=%llu\n",
        static_cast<unsigned long long>(hostMathWaves),static_cast<unsigned long long>(hostMathOperations));
    std::fprintf(stderr,"CUDA_VM_OUTPUT_PROFILE initialCapacity=%u capacity=%u recompactions=%u budgetLimited=%d\n",
        input.outputCapacity,outputBytes,recompactions,outputBudgetLimited);
    Check(cudaEventElapsedTime(&milliseconds,p.started,p.done),"Reading CUDA program duration");
    cudaFuncAttributes attributes{};
    if (scheduled)
      Check(cudaFuncGetAttributes(&attributes,advanceKernel),"Reading CUDA program resources");
    else
      Check(cudaFuncGetAttributes(&attributes,p.orderedEllipsoids ? ExecutePrograms<true> : ExecutePrograms<false>),"Reading CUDA program resources");
    std::uint32_t workingBytes=0,compactBytes=0;
    std::uint64_t stepped=0,skipped=0,vmCycles=0,physicsCycles=0,nativeVersions=0;
    for (const auto &header : headers) {
      workingBytes=std::max(workingBytes,header.workingBytes);
      compactBytes=std::max(compactBytes,header.bytes);
      stepped+=header.stepped; skipped+=header.skipped;
      vmCycles+=header.vmCycles; physicsCycles+=header.physicsCycles;
      nativeVersions+=header.nativeVersions;
    }
    std::fprintf(stderr,"CUDA_VM_PROFILE lanes=%u milliseconds=%.6f arena=%u used=%u output=%u registers=%d local=%zu ordered=%d checkpoints=%zu stepped=%llu skipped=%llu scheduled=%d handling=%u water=%d schedulerFallback=%d graph=%d graphUnavailable=%d vmCycles=%llu physicsCycles=%llu nativeVersions=%llu\n",
        count,milliseconds,input.arenaCapacity,workingBytes,compactBytes,attributes.numRegs,attributes.localSizeBytes,p.orderedEllipsoids,
        checkpointCount,static_cast<unsigned long long>(stepped),static_cast<unsigned long long>(skipped),scheduled,
        p.configurationHeader.tuning.handlingModel,p.configurationHeader.water.present,schedulerFallback,graphScheduled,graphUnavailable,
        static_cast<unsigned long long>(vmCycles),static_cast<unsigned long long>(physicsCycles),
        static_cast<unsigned long long>(nativeVersions));
  }
  std::vector<CudaProgramOutput> output(count);
  for (std::uint32_t i=0;i<count;++i) {
    output[i].result=headers[i].result;
    output[i].outputCapacityExceeded=headers[i].outputCapacityExceeded;
    output[i].outputCapacity=outputBytes;
    if (headers[i].result.error==Error::None) {
      output[i].arena.resize(headers[i].bytes);
      Check(cudaMemcpyAsync(output[i].arena.data(),p.outputs.get<unsigned char>()+static_cast<std::size_t>(i)*outputBytes,
        headers[i].bytes,cudaMemcpyDeviceToHost,p.stream),"Reading CUDA result values");
    }
  }
  Check(cudaStreamSynchronize(p.stream),"Finishing CUDA result transfer");
  return output;
}
} // namespace forevertas::blocks
