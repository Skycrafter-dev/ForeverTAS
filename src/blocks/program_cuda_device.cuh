#ifndef FOREVERTAS_BLOCKS_PROGRAM_CUDA_DEVICE_CUH
#define FOREVERTAS_BLOCKS_PROGRAM_CUDA_DEVICE_CUH

#include "blocks/program_cuda_kernel.h"
#include "blocks/program_cuda_checkpoint.h"

#include "simulation/backends/cuda/cuda_finish_time_refinement.cuh"
#include "simulation/backends/cuda/cuda_search_branch_state.cuh"
#include "simulation/backends/cuda/cuda_stunts.cuh"
#include "simulation/backends/cuda/cuda_vehicle_transitions.cuh"

#include <cuda_runtime.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>

namespace forevertas::blocks::cuda_program_detail {
using namespace forevervalidator::simulation;
using namespace vm;

constexpr std::uint32_t PropertyCount=CudaProgramStatePropertyCount;
struct PublicState { Value properties[PropertyCount]; };
struct PublicAuxiliary { std::uint32_t stunts=0; bool wheelSurface[4]{}; };
struct LaneOutput {
  Result result;
  std::uint32_t bytes,workingBytes,stepped,skipped;
  bool outputCapacityExceeded;
  std::uint64_t vmCycles=0,physicsCycles=0;
  std::uint32_t nativeVersions=0;
  // The original arena value survives output compaction and buffer growth.
  Value sourceValue;
};
struct MathRequest {
  Op op=Op::None;
  Value arguments[3]{},result;
  Error error=Error::None;
  std::uint32_t state=0;
};
struct ScheduleStatus { std::uint32_t lanes,ticks,hostMath,copies; };
struct NativeCopyRequest { std::uint32_t extension=0,physical=0,overflow=0,overflowBytes=0; };
struct CursorOutput {
  Value native;
  std::uint32_t count=0,bytes=0;
  Error error=Error::None;
};
struct CollisionStorage {
  cuda::collision::CudaCollisionSearchTile *collisions,*shapeCollisions;
  GmIso4 *shapeWorld;
  GmBoxAligned *movingBounds;
  cuda::collision::CudaCollisionSurfaceHit *surfaceHits;
  cuda::collision::CudaCollisionMeshRange *meshRanges;
  std::uint32_t *meshCells;
  std::uint16_t *responseOrder;
  std::uint32_t shapeCount;
};
struct RestoreContext {
  const CudaCandidateState *origin=nullptr;
  const CudaControlTick *ticks=nullptr;
  const PackedCheckpoint *checkpoints=nullptr;
  const CudaCandidateState *checkpointExtensions=nullptr;
  const Value *checkpointInputs=nullptr;
  std::uint64_t initialTime=0;
  std::uint32_t tickCount=0,prestartMs=0,checkpointCount=0;
};
struct KernelParameters {
  Program program;
  const CudaCandidateState *origin;
  const CudaControlTick *ticks;
  const CudaPackedSceneHeader *scene;
  const CudaPackedStaticConfigurationHeader *configuration;
  const unsigned char *initialArena;
  const Value *arguments;
  const std::uint64_t *seeds;
  Value globals,inputs,initialView,previousView;
  std::uint32_t initialBytes,arenaBytes,outputBytes,tickMs,prestartMs,tickCount,count,entry,batchSize;
  std::uint64_t initialTime;
  const std::uint32_t *cancellation;
  const PackedCheckpoint *checkpoints;
  const CudaCandidateState *checkpointExtensions;
  const Value *checkpointInputs;
  std::uint32_t checkpointCount;
  CollisionStorage collisionStorage;
  bool profile=false;
  MathRequest *mathRequests=nullptr;
  NativeCopyRequest *nativeCopies=nullptr;
  const unsigned char *sharedArena=nullptr;
  const RestoreContext *restoreContexts=nullptr;
  std::uint32_t *restoreContextIds=nullptr;
  std::uint32_t restoreContextCount=0;
  Value histories;
  Value restartSnapshot;
  std::uint64_t collectionLimit=1000000;
  const std::uint64_t *historySizes=nullptr;
  const std::uint8_t *historyReachable=nullptr;
  const std::uint8_t *restoreReachable=nullptr;
};

__device__ inline Value Vector(const GmVec3 &v) { return {Kind::Vector,0,v.x,v.y,v.z}; }
__device__ inline Value Speed(const GmVec3 &v) {
  const double x=::fabs(static_cast<double>(v.x)),y=::fabs(static_cast<double>(v.y)),z=::fabs(static_cast<double>(v.z));
  const double scale=maximum(maximum(x,y),z);
  return number(scale ? scale*::sqrt((x/scale)*(x/scale)+(y/scale)*(y/scale)+(z/scale)*(z/scale)) : 0);
}

__device__ inline void ReadPublicState(const KernelParameters &parameters,const CudaCandidatePhysicsState &state,
    const PublicAuxiliary &extra,std::uint64_t time,PublicState &output,const PublicState &metadata) {
  auto *v=output.properties;
  const auto &body=state.body.current;
  const auto &car=state.vehicle;
  const auto &physics=car.frameHistory.physicsCurrent;
  const auto &race=state.race.progress;
  v[0]=number(static_cast<double>(time)); v[1]=number(static_cast<double>(time/parameters.tickMs));
  v[2]=metadata.properties[2]; v[3]=Vector(body.position); v[4]=Vector(body.linearSpeed);
  v[5]=Vector(physics.localLinearSpeed); v[6]=Vector(body.angularSpeed);
  v[7]=Vector(body.force); v[8]=Vector(body.torque);
  v[9]={Kind::Rotation,0,body.rotationQuat.x,body.rotationQuat.y,body.rotationQuat.z,body.rotationQuat.w};
  v[10]=number(body.rotationQuat.x); v[11]=number(body.rotationQuat.y); v[12]=number(body.rotationQuat.z); v[13]=number(body.rotationQuat.w);
  v[14]=Speed(body.linearSpeed);
  v[15]=number(physics.forwardSpeed);
  v[16]=number(car.controls.lowSpeedGateA); v[17]=number(car.controls.lowSpeedGateB); v[18]=number(car.controls.steeringControl);
  v[19]=number(car.engine.useLowSpeedGateB ? -1 : static_cast<std::int32_t>(car.engine.gearIndex)); v[20]=number(car.engine.engineInputMemory);
  v[21]=number(car.radiusSteering.steerAngle);
  bool sliding=false;
  v[23]=boolean(car.controls.forcedLowSpeedFriction); v[24]=boolean(car.contacts.lateralSlowDownContactActive);
  v[25]=number(physics.turboActive ? 1 : 0); v[26]=number(car.turbo.type);
  v[27]=number(car.turbo.type==CSceneVehicleCar::ETurboType_Roulette ? static_cast<float>(static_cast<double>(car.turbo.type2Phase)+1.0) : car.turbo.impulseScale);
  v[28]=boolean(false); v[29]=boolean(physics.engineControlState==CSceneVehicleCarEngineControlState_GearShift);
  for (std::uint32_t property=30;property<=33;++property) {
    double values[4];
    for (std::uint32_t i=0;i<4;++i) {
      const auto &wheel=car.wheels.values[i];
      const bool contact=wheel.realTime.contactPresent;
      const bool slip=contact && wheel.realTime.slipping;
      sliding=sliding || slip;
      values[i]=property==30 ? contact : property==31 ? (contact ? static_cast<std::uint16_t>(wheel.realTime.contactMaterial) : 0xffff)
                : property==32 ? extra.wheelSurface[i] : slip;
    }
    // Lists in this fixed-size view use inline components, not arena handles.
    v[property]={Kind::List,0,values[0],values[1],values[2],values[3]};
  }
  v[22]=boolean(sliding); v[34]={Kind::Vector,0,0,1,0}; v[35]=number(0);
  v[36]=number(race.checkpointCount); v[37]=number(race.requiredCheckpointCount);
  v[38]=number(race.completedLapCount); v[39]=number(race.requiredLapCount); v[40]=boolean(race.raceCompleted);
  for (std::uint32_t i=41;i<=44;++i) v[i]={};
  if (state.finishTime.present) {
    const auto offset=static_cast<std::uint64_t>(parameters.prestartMs)*1000000u;
    const auto subtract=[&](std::uint64_t value) { return value>=offset ? value-offset : 0; };
    const auto ns=subtract(state.finishTime.value.estimatedNs);
    v[41]=number(static_cast<double>(ns/1000000u)); v[42]=number(static_cast<double>(ns)/1000000.0);
    v[43]=number(static_cast<double>(subtract(state.finishTime.value.lowerBoundNs)));
    v[44]=number(static_cast<double>(subtract(state.finishTime.value.upperBoundNs)));
  } else if (race.raceCompleted) {
    v[41]=number(race.lastPrepareTimeMs>=parameters.prestartMs ? race.lastPrepareTimeMs-parameters.prestartMs : 0);
  }
  v[45]=number(state.incrementalRespawnCount);
  v[46]=state.stuntsEnabled ? number(extra.stunts) : Value{};
  for (std::uint32_t i=47;i<PropertyCount;++i) v[i]=metadata.properties[i];
}

template<bool OrderedEllipsoids,bool Scheduled=false> class DevicePhysics {
public:
  static constexpr bool exactMathExecution=true;
  MathRequest *mathRequest=nullptr;
  NativeCopyRequest *nativeCopies=nullptr;
  KernelParameters &parameters;
  CudaCandidatePhysicsState state,previousPhysics;
  CudaCandidateState &extended;
  cuda::collision::CudaCollisionSearchScratch &scratch;
  PublicState current,previous,metadata;
  PublicAuxiliary previousAuxiliary;
  bool currentDirty=false,previousDirty=false;
  Value inputs;
  Value horizonChanges;
  Value history;
  Value snapshotExtension;
  bool snapshotExtensionDirty=true;
  std::uint32_t nativeVersions=0;
  std::uint32_t overflowDirtyCount=0;
  std::uint32_t checkpointGeneration=0;
  CheckpointInputValidation checkpointInputValidation;
  std::uint32_t tickIndex=0,eventCursor=0,pendingTicks=0;
  std::uint32_t stepped=0,skipped=0;
  cuda_search_detail::DeviceControlState controls;

  __device__ DevicePhysics(KernelParameters &parameters,CudaCandidateState &extended,cuda::collision::CudaCollisionSearchScratch &scratch)
      : parameters(parameters),state(extended),previousPhysics(extended),extended(extended),scratch(scratch) {
    const auto *physical=static_cast<const CudaCandidatePhysicsState *>(&extended);
    memcpy(&state,physical,sizeof(state));
    memcpy(&previousPhysics,physical,sizeof(previousPhysics));
  }
  __device__ PublicAuxiliary auxiliary() const {
    PublicAuxiliary result; result.stunts=extended.stunts.stuntsScore;
    for (std::uint32_t i=0;i<4;++i) result.wheelSurface[i]=extended.vehiclePassthrough.wheels[i].currentAsync.contactPresent;
    return result;
  }
  __device__ void invalidateSnapshotExtension(Machine<DevicePhysics> &machine) {
    machine.memory.assign(snapshotExtension,{});
    snapshotExtensionDirty=true;
    checkpointGeneration=0;
  }
  __device__ bool hasNativeCopies() const { return nativeCopies && nativeCopies->physical; }
  __device__ void releaseNativeCopies(Machine<DevicePhysics> &machine) {
    if (!hasNativeCopies()) return;
    const auto request=*nativeCopies;
    *nativeCopies={};
    for (const auto handle : {request.extension,request.physical,request.overflow})
      if (handle) machine.memory.release({Kind::Opaque,handle});
  }
  __device__ void finishNativeCopies(Machine<DevicePhysics> &machine) {
    if (!hasNativeCopies()) return;
    const auto request=*nativeCopies;
    if (request.extension)
      *reinterpret_cast<CudaCandidateState *>(machine.memory.bytes+request.extension)=extended;
    *reinterpret_cast<CudaCandidatePhysicsState *>(machine.memory.bytes+request.physical)=state;
    memcpy(machine.memory.bytes+request.overflow,&extended.collisionReplacementOverflow,request.overflowBytes);
    releaseNativeCopies(machine);
  }
  template<CudaHandlingSpecialization Handling=CudaHandlingSpecialization::Generic>
  __device__ __forceinline__ Value advancePending(Machine<DevicePhysics> &machine);
  template<CudaHandlingSpecialization Handling>
  __device__ __forceinline__ Value advanceTicks(Machine<DevicePhysics> &machine,CudaCandidatePhysicsState &workingState,
      cuda::collision::CudaCollisionSearchScratch &workingScratch,const KernelParameters &configuration);
  __device__ __noinline__ Value flush(Machine<DevicePhysics> &machine) {
    if constexpr (!Scheduled) return advancePending(machine);
    return {};
  }
  __device__ Value exactMath(Machine<DevicePhysics> &machine) {
    if (!mathRequest || mathRequest->state!=2) { machine.error=Error::Interpreter; return {}; }
    machine.check(mathRequest->error==Error::None,mathRequest->error);
    mathRequest->state=0;
    return mathRequest->result;
  }
  __device__ bool pauseBefore(const Instruction &instruction,Machine<DevicePhysics> &machine) {
    if (instruction.op==Op::Restart) {
      if (pendingTicks || hasNativeCopies()) return true;
      if (mathRequest && mathRequest->op==Op::Restart && mathRequest->state==2) return false;
      const auto id=snapshotContext(machine,parameters.restartSnapshot);
      if (machine.error!=Error::None || !parameters.restoreContexts || id>=parameters.restoreContextCount) return false;
      const auto &context=parameters.restoreContexts[id];
      const auto horizon=current.properties[2];
      if ((context.origin && horizon.x<=context.initialTime+static_cast<double>(context.tickCount)*parameters.tickMs) || !mathRequest) return false;
      mathRequest->op=Op::Restart; mathRequest->arguments[0]=number(id); mathRequest->arguments[1]=horizon;
      mathRequest->state=1;
      return true;
    }
    if (instruction.op==Op::SetHorizon) {
      if (pendingTicks || hasNativeCopies()) return true;
      if (mathRequest && mathRequest->op==Op::SetHorizon && mathRequest->state==2) return false;
      if (!machine.stackSize || !parameters.restoreContexts || !parameters.restoreContextIds) return false;
      const auto value=machine.stack[machine.stackSize-1];
      if (value.kind!=Kind::Number || !::isfinite(value.x) || value.x<parameters.tickMs ||
          value.x>2147481040.0 || value.x<current.properties[0].x ||
          ::fmod(value.x,static_cast<double>(parameters.tickMs))!=0) return false;
      const auto id=parameters.restoreContextIds[scratch.slot];
      const auto &context=parameters.restoreContexts[id];
      if (value.x<=context.initialTime+static_cast<double>(context.tickCount)*parameters.tickMs || !mathRequest) return false;
      mathRequest->op=Op::SetHorizon; mathRequest->arguments[0]=number(id); mathRequest->arguments[1]=value;
      mathRequest->state=1;
      return true;
    }
    if (instruction.op==Op::Restore) {
      if (pendingTicks || hasNativeCopies()) return true;
      if (mathRequest && mathRequest->op==Op::Restore && mathRequest->state==2) return false;
      if (!machine.stackSize) return false;
      const auto id=snapshotContext(machine,machine.stack[machine.stackSize-1]);
      if (machine.error!=Error::None || !parameters.restoreContexts ||
          id>=parameters.restoreContextCount || parameters.restoreContexts[id].origin) return false;
      if (!mathRequest) return false;
      mathRequest->op=Op::Restore; mathRequest->arguments[0]=number(id);
      mathRequest->state=1;
      return true;
    }
    if (requiresHostMath(instruction.op) && mathRequest && mathRequest->state!=2 &&
        instruction.b<=3 && machine.stackSize>=instruction.b) {
      bool cached=mathRequest->state==0 && mathRequest->op==instruction.op && mathRequest->error==Error::None;
      for (std::uint32_t i=0;i<instruction.b && cached;++i) {
        const auto &a=mathRequest->arguments[i], &b=machine.stack[machine.stackSize-instruction.b+i];
        // The pure-operation cache must distinguish signed zeros and NaN bits.
        cached=a.kind==b.kind && a.handle==b.handle &&
            __double_as_longlong(a.x)==__double_as_longlong(b.x) &&
            __double_as_longlong(a.y)==__double_as_longlong(b.y) &&
            __double_as_longlong(a.z)==__double_as_longlong(b.z) &&
            __double_as_longlong(a.w)==__double_as_longlong(b.w);
      }
      if (cached) { mathRequest->state=2; return false; }
      mathRequest->op=instruction.op;
      for (std::uint32_t i=0;i<instruction.b;++i)
        mathRequest->arguments[i]=machine.stack[machine.stackSize-instruction.b+i];
      mathRequest->state=1;
      return true;
    }
    if (!pendingTicks) return false;
    if (pendingTicks>=128) return true;
    switch (instruction.op) {
    case Op::ReadCurrent: return instruction.a>=3;
    case Op::Step: return machine.pendingEventObserved();
    case Op::EventState: return !machine.eventContext.handle || machine.needsEventState(machine.memory.items(machine.eventContext)[2]);
    case Op::EventValue: return machine.eventContext.handle && machine.needsEventState(machine.memory.items(machine.eventContext)[1]);
    case Op::EventRead: return instruction.a>=3 && (!machine.eventContext.handle || machine.needsEventState(machine.memory.items(machine.eventContext)[2]));
    case Op::Read: return instruction.a>=3 && machine.stackSize && machine.needsEventState(machine.stack[machine.stackSize-1]);
    case Op::CurrentState: case Op::PreviousState: case Op::ReadPrevious: case Op::Snapshot: case Op::Publish: case Op::UseInputs: case Op::History: return true;
    default: return false;
    }
  }
  __device__ void reusePrefix(Machine<DevicePhysics> &machine) {
    if (history.handle) return;
    const PackedCheckpoint *selected=nullptr;
    const auto *events=machine.memory.items(inputs);
    const auto count=machine.memory.length(inputs);
    std::uint32_t first=0,last=parameters.checkpointCount;
    while (first<last) {
      const auto middle=first+(last-first)/2;
      if (parameters.checkpoints[middle].tick<=tickIndex+pendingTicks) first=middle+1;
      else last=middle;
    }
    for (auto i=first;i>0;) {
      if (cancelled()) { machine.error=Error::Cancelled; return; }
      const auto &checkpoint=parameters.checkpoints[--i];
      if (checkpoint.tick<=tickIndex) break;
      if (checkpoint.inputCount>count) continue;
      const auto end=checkpoint.current[0].x;
      if (checkpoint.inputCount<count && events[checkpoint.inputCount].x<=end) continue;
      const bool same=checkpointInputValidation.matches(checkpoint.inputGeneration,events,
          parameters.checkpointInputs+checkpoint.inputOffset,checkpoint.inputCount);
      if (same) { selected=&checkpoint; break; }
    }
    if (!selected) return;
    const auto time=current.properties[0],tick=current.properties[1];
    for (std::uint32_t i=0;i<PropertyCount;++i) {
      current.properties[i]=selected->current[i]; previous.properties[i]=selected->previous[i];
    }
    current.properties[2]=metadata.properties[2]; previous.properties[2]=metadata.properties[2];
    if (checkpointGeneration!=selected->extensionGeneration) {
      invalidateSnapshotExtension(machine);
      extended=parameters.checkpointExtensions[selected->extensionGeneration-1];
    } else {
      // Inputs were checked above; only these overlays can differ within a
      // proven extension generation. Preserve any live snapshot's base version.
      overflowDirtyCount=max(overflowDirtyCount,extended.collisionReplacementOverflow.count);
      overflowDirtyCount=max(overflowDirtyCount,selected->overflow.count);
    }
    extended.collisionReplacementOverflow=selected->overflow;
    state=selected->state;
    checkpointGeneration=selected->extensionGeneration;
    const auto countSkipped=selected->tick-tickIndex;
    pendingTicks-=countSkipped; skipped+=countSkipped; tickIndex=selected->tick;
    consumeControls(machine,selected->current[0].x);
    current.properties[0]=time; current.properties[1]=tick;
    currentDirty=pendingTicks!=0; previousDirty=false;
    scratch.surfaceCacheValid=false;
  }
  __device__ void observeCurrent(Machine<DevicePhysics> &machine) {
    flush(machine);
    if (!currentDirty) return;
    ReadPublicState(parameters,state,auxiliary(),parameters.initialTime+static_cast<std::uint64_t>(tickIndex)*parameters.tickMs,current,metadata);
    currentDirty=false;
  }
  __device__ void observePrevious(Machine<DevicePhysics> &machine) {
    flush(machine);
    if (!previousDirty) return;
    ReadPublicState(parameters,previousPhysics,previousAuxiliary,
        parameters.initialTime+static_cast<std::uint64_t>(tickIndex-1)*parameters.tickMs,previous,metadata);
    previousDirty=false;
  }
  __device__ bool cancelled() const { return *reinterpret_cast<const volatile std::uint32_t *>(parameters.cancellation)!=0; }
  __device__ bool interpreterRequested() const { return *reinterpret_cast<const volatile std::uint32_t *>(parameters.cancellation)==2; }
  __device__ void initialize(Machine<DevicePhysics> &machine) {
    inputs=machine.memory.copy(parameters.inputs);
    if (parameters.histories.handle) {
      history=machine.memory.copy(machine.memory.items(parameters.histories)[0]);
      history.x=0;
    }
    for (std::uint32_t i=0;i<PropertyCount;++i) {
      current.properties[i]=machine.memory.items(parameters.initialView)[i];
      previous.properties[i]=machine.memory.items(parameters.previousView)[i];
      if (i>=30 && i<=33) {
        const auto flatten=[&](Value value) {
          const auto *items=machine.memory.items(value);
          return Value{Kind::List,0,items[0].x,items[1].x,items[2].x,items[3].x};
        };
        current.properties[i]=flatten(current.properties[i]);
        previous.properties[i]=flatten(previous.properties[i]);
      }
      metadata.properties[i]=current.properties[i];
    }
    rebuildControls(machine,current.properties[0].x);
  }
  __device__ std::uint32_t snapshotContext(Machine<DevicePhysics> &machine,Value snapshot) {
    if (!machine.array(snapshot,Kind::Snapshot) || !machine.check(machine.memory.length(snapshot)==5,Error::Type)) return 0;
    const auto native=machine.memory.items(snapshot)[0];
    if (native.kind==Kind::Opaque && native.handle && machine.memory.length(native)==sizeof(std::uint32_t))
      return *reinterpret_cast<const std::uint32_t *>(machine.memory.bytes+native.handle)+1;
    if (native.kind==Kind::List && native.handle && machine.memory.length(native)>=4 && machine.memory.length(native)<=6) {
      const auto id=machine.memory.items(native)[3];
      if (!machine.check(id.kind==Kind::Number && id.x>=0 && id.x<parameters.restoreContextCount && ::floor(id.x)==id.x,Error::Bounds)) return 0;
      return static_cast<std::uint32_t>(id.x);
    }
    return 0;
  }
  __device__ bool restoreView(Machine<DevicePhysics> &machine,Value value,PublicState &view) {
    if (!machine.array(value,Kind::State) || !machine.check(machine.memory.length(value)==PropertyCount,Error::Type)) return false;
    for (std::uint32_t i=0;i<PropertyCount;++i) {
      auto property=machine.memory.items(value)[i];
      if (i>=30 && i<=33) {
        if (!machine.array(property,Kind::List) || !machine.check(machine.memory.length(property)==4,Error::Type)) return false;
        const auto *items=machine.memory.items(property);
        property={Kind::List,0,items[0].x,items[1].x,items[2].x,items[3].x};
      }
      view.properties[i]=property;
    }
    return true;
  }
  __device__ Value restore(Machine<DevicePhysics> &machine,Value snapshot,bool restarting=false) {
    if (restarting && cancelled()) { machine.error=interpreterRequested() ? Error::Interpreter : Error::Cancelled; return {}; }
    discardUnobservedHistory(machine);
    if (mathRequest && mathRequest->op==(restarting ? Op::Restart : Op::Restore) && mathRequest->state==2) {
      mathRequest->state=0;
      if (!machine.check(mathRequest->error==Error::None,mathRequest->error)) return {};
    }
    const auto id=snapshotContext(machine,snapshot);
    if (machine.error!=Error::None || !machine.check(parameters.restoreContexts && id<parameters.restoreContextCount,Error::Unsupported)) return {};
    const auto &context=parameters.restoreContexts[id];
    if (!machine.check(context.origin!=nullptr,Error::Unsupported)) return {};
    const auto liveHorizon=current.properties[2];
    finishNativeCopies(machine);
    const auto *fields=machine.memory.items(snapshot);
    if (!machine.array(fields[1],Kind::Inputs) || !restoreView(machine,fields[2],current) ||
        !restoreView(machine,fields[3],previous)) return {};
    const auto elapsed=current.properties[0].x-static_cast<double>(context.initialTime);
    if (!machine.check(elapsed>=0 && elapsed<=static_cast<double>(context.tickCount)*parameters.tickMs &&
        ::fmod(elapsed,static_cast<double>(parameters.tickMs))==0,Error::Time)) return {};
    invalidateSnapshotExtension(machine);
    const auto native=fields[0];
    if (restarting) {
      if (!machine.check(liveHorizon.x>=current.properties[0].x,Error::Horizon)) return {};
      current.properties[2]=liveHorizon;
      auto changes=machine.memory.allocate(Kind::List,1);
      if (machine.memory.error!=Error::None) return {};
      // The first entry retains the restart's initial historical duration even
      // if another horizon edit replaces the live duration at the same tick.
      machine.memory.items(changes)[0]={Kind::Range,0,current.properties[0].x,liveHorizon.x,liveHorizon.x};
      machine.memory.assign(horizonChanges,changes);
      if (history.handle) {
        auto frames=machine.memory.allocate(Kind::List,1);
        if (machine.memory.error!=Error::None) return {};
        machine.memory.items(frames)[0]=readState(machine,current);
        frames.x=1;
        machine.memory.assign(history,frames);
      }
    } else {
      machine.memory.assign(horizonChanges,native.kind==Kind::List && machine.memory.length(native)>=5
          ? machine.memory.copy(machine.memory.items(native)[4]) : Value{});
    }
    if (!restarting && history.handle) {
      const bool cached=native.kind==Kind::List && machine.memory.length(native)==6;
      auto savedHistory=cached
          ? machine.memory.items(native)[5] : machine.memory.items(parameters.histories)[id];
      if (!machine.array(savedHistory,Kind::List)) return {};
      if (!cached) savedHistory.x=0;
      if (!machine.check(savedHistory.x>=0 && savedHistory.x<=machine.memory.length(savedHistory) &&
          ::floor(savedHistory.x)==savedHistory.x,Error::Bounds)) return {};
      machine.memory.assign(history,machine.memory.copy(savedHistory));
    }
    if (native.kind==Kind::Opaque && native.handle && machine.memory.length(native)==sizeof(std::uint32_t)) {
      extended=*context.origin;
      state=extended;
    } else {
      if (!machine.array(native,Kind::List) || !machine.check(machine.memory.length(native)>=3,Error::Type)) return {};
      const auto *parts=machine.memory.items(native);
      if (!machine.check(parts[0].kind==Kind::Opaque && parts[0].handle && machine.memory.length(parts[0])==sizeof(CudaCandidateState) &&
          parts[1].kind==Kind::Opaque && parts[1].handle && machine.memory.length(parts[1])==sizeof(CudaCandidatePhysicsState) &&
          parts[2].kind==Kind::Opaque && parts[2].handle,Error::Type)) return {};
      using Overflow=decltype(extended.collisionReplacementOverflow);
      const auto bytes=machine.memory.length(parts[2]);
      if (!machine.check(bytes>=offsetof(Overflow,values) && bytes<=sizeof(Overflow) &&
          (bytes - offsetof(Overflow,values))%sizeof(GmVec3)==0,Error::Type)) return {};
      extended=*reinterpret_cast<const CudaCandidateState *>(machine.memory.bytes+parts[0].handle);
      state=*reinterpret_cast<const CudaCandidatePhysicsState *>(machine.memory.bytes+parts[1].handle);
      memcpy(&extended.collisionReplacementOverflow,machine.memory.bytes+parts[2].handle,bytes);
    }
    parameters.origin=context.origin; parameters.ticks=context.ticks;
    parameters.initialTime=context.initialTime;
    parameters.tickCount=static_cast<std::uint32_t>((current.properties[2].x-context.initialTime)/parameters.tickMs);
    parameters.prestartMs=context.prestartMs;
    parameters.checkpoints=context.checkpoints; parameters.checkpointExtensions=context.checkpointExtensions;
    parameters.checkpointInputs=context.checkpointInputs; parameters.checkpointCount=context.checkpointCount;
    parameters.restoreContextIds[scratch.slot]=id;
    tickIndex=static_cast<std::uint32_t>(elapsed/parameters.tickMs); pendingTicks=0;
    metadata=current; currentDirty=false; previousDirty=false;
    checkpointInputValidation={}; overflowDirtyCount=0;
    scratch.surfaceCacheValid=false;
    if (!restarting) machine.memory.assign(inputs,machine.memory.copy(fields[1]));
    rebuildControls(machine,current.properties[0].x);
    return {};
  }
  __device__ void applyEvent(Value event) {
    CudaSearchInputEvent input;
    input.timeMs=static_cast<std::int32_t>(event.x); input.action=static_cast<std::uint32_t>(event.z);
    input.value=static_cast<std::int32_t>(event.y); input.valueKind=static_cast<std::uint32_t>(event.w);
    cuda_search_detail::ApplyControlEvent(controls,input);
  }
  __device__ void consumeControls(Machine<DevicePhysics> &machine,double until) {
    const auto *events=machine.memory.items(inputs);
    while (eventCursor<machine.memory.length(inputs) && events[eventCursor].x<=until)
      applyEvent(events[eventCursor++]);
  }
  __device__ void rebuildControls(Machine<DevicePhysics> &machine,double until) {
    controls={}; eventCursor=0;
    consumeControls(machine,until);
  }
  __device__ Value readProperty(Machine<DevicePhysics> &machine,const PublicState &view,std::uint32_t property) {
    if (!machine.check(property<PropertyCount,Error::Bounds)) return {};
    const auto value=view.properties[property];
    if (property>=30 && property<=33) {
      auto result=machine.memory.allocateForOverwrite(Kind::List,4);
      if (machine.memory.error!=Error::None) return {};
      const double values[]={value.x,value.y,value.z,value.w};
      for (std::uint32_t i=0;i<4;++i) machine.memory.items(result)[i]=property==31 ? number(values[i]) : boolean(values[i]!=0);
      return result;
    }
    return value;
  }
  __device__ Value readState(Machine<DevicePhysics> &machine,const PublicState &view) {
    auto result=machine.memory.allocateForOverwrite(Kind::State,PropertyCount);
    if (machine.memory.error!=Error::None) return {};
    for (std::uint32_t i=0;i<PropertyCount;++i) machine.memory.items(result)[i]=readProperty(machine,view,i);
    return result;
  }
  __device__ bool mayReach(const Machine<DevicePhysics> &machine,const std::uint8_t *continuations) const {
    if (!continuations) return true;
    const auto reachable=[&](std::uint32_t pc) {
      return pc<parameters.program.codeSize && continuations[pc];
    };
    if (reachable(machine.pc)) return true;
    for (std::uint32_t i=0;i<machine.frameCount;++i) if (reachable(machine.frames[i].pc)) return true;
    return false;
  }
  __device__ void discardUnobservedHistory(Machine<DevicePhysics> &machine) {
    if (!history.handle || mayReach(machine,parameters.historyReachable)) return;
    // Retained lists and snapshots own their immutable prefixes independently.
    // With no future observer in any active frame, recording cannot resume.
    machine.memory.assign(history,{});
  }
  __device__ std::uint32_t historySize(Machine<DevicePhysics> &machine) const {
    return history.x ? static_cast<std::uint32_t>(history.x) : machine.memory.length(history);
  }
  __device__ __noinline__ void recordHistory(Machine<DevicePhysics> &machine,
      const CudaCandidatePhysicsState &physical,std::uint64_t time) {
    const auto count=historySize(machine);
    if (!machine.check(count<parameters.collectionLimit,Error::Capacity)) return;
    PublicState view;
    ReadPublicState(parameters,physical,auxiliary(),time,view,metadata);
    const auto frame=readState(machine,view);
    if (machine.memory.error!=Error::None) return;
    const auto length=count+1;
    const auto &header=machine.memory.header(history.handle);
    // Private descriptors freeze a prefix length in x. Only append at the
    // backing's end; imported lists and restored earlier prefixes detach.
    if (!history.x || count!=header.length || sizeof(Header)+static_cast<std::uint64_t>(length)*sizeof(Value)>header.bytes) {
      auto next=machine.memory.allocate(Kind::List,length);
      if (machine.memory.error!=Error::None) return;
      for (std::uint32_t i=0;i<count;++i) machine.memory.items(next)[i]=machine.memory.copy(machine.memory.items(history)[i]);
      machine.memory.assign(history,next);
    } else machine.memory.header(history.handle).length=length;
    machine.memory.items(history)[count]=frame;
    history.x=length;
  }
  __device__ Value apply(Op op,Machine<DevicePhysics> &machine,Value *a,std::uint32_t option) {
    switch (op) {
    case Op::Time: return current.properties[0];
    case Op::Horizon: return current.properties[2];
    case Op::AllTime: return {Kind::Range,0,0,current.properties[2].x};
    case Op::AtTime: { const auto at=machine.time(machine.numeric(a[0])); return {Kind::Range,0,static_cast<double>(at),static_cast<double>(at)}; }
    case Op::ReadCurrent:
      if (option==3 || option==4 || option==14 || option==36 || option==40) {
        flush(machine);
        if (!currentDirty) return readProperty(machine,current,option);
        switch (option) {
        case 3: return Vector(state.body.current.position);
        case 4: return Vector(state.body.current.linearSpeed);
        case 14: return Speed(state.body.current.linearSpeed);
        case 36: return number(state.race.progress.checkpointCount);
        case 40: return boolean(state.race.progress.raceCompleted);
        }
      }
      if (option>=3) observeCurrent(machine);
      return readProperty(machine,current,option);
    case Op::ReadPrevious:
      flush(machine);
      if (!previousDirty) return readProperty(machine,previous,option);
      switch (option) {
      case 3: return Vector(previousPhysics.body.current.position);
      case 4: return Vector(previousPhysics.body.current.linearSpeed);
      case 14: return Speed(previousPhysics.body.current.linearSpeed);
      case 36: return number(previousPhysics.race.progress.checkpointCount);
      case 40: return boolean(previousPhysics.race.progress.raceCompleted);
      }
      observePrevious(machine);
      return readProperty(machine,previous,option);
    case Op::CurrentState: observeCurrent(machine); return readState(machine,current);
    case Op::PreviousState: observePrevious(machine); return readState(machine,previous);
    case Op::History: {
      flush(machine);
      if (!machine.check(history.handle!=0,Error::Unsupported)) return {};
      const auto count=historySize(machine);
      auto result=machine.memory.allocate(Kind::List,count);
      if (machine.memory.error!=Error::None) return {};
      for (std::uint32_t i=0;i<count;++i)
        machine.memory.items(result)[i]=machine.memory.copy(machine.memory.items(history)[i]);
      return result;
    }
    case Op::Inputs: return machine.memory.copy(inputs);
    case Op::SetHorizon: {
      if (mathRequest && mathRequest->op==Op::SetHorizon && mathRequest->state==2) {
        mathRequest->state=0;
        if (!machine.check(mathRequest->error==Error::None,mathRequest->error)) return {};
      }
      const auto next=machine.time(machine.numeric(a[0]));
      if (machine.error!=Error::None || !machine.check(next>=parameters.tickMs && next>=current.properties[0].x,Error::Horizon)) return {};
      if (next==current.properties[2].x) return {};
      if (!machine.check(parameters.restoreContexts && parameters.restoreContextIds,Error::Unsupported)) return {};
      const auto &context=parameters.restoreContexts[parameters.restoreContextIds[scratch.slot]];
      if (!machine.check(next<=context.initialTime+static_cast<std::uint64_t>(context.tickCount)*parameters.tickMs,Error::Unsupported)) return {};
      observeCurrent(machine); observePrevious(machine);
      const auto count=horizonChanges.handle ? machine.memory.length(horizonChanges) : 0;
      const bool replace=count && machine.memory.items(horizonChanges)[count-1].x==current.properties[0].x;
      const auto length=count+(replace ? 0 : 1);
      // Extend an unshared allocation in place; retained snapshots keep their
      // immutable duration log through copy-on-write.
      if (!horizonChanges.handle || machine.memory.header(horizonChanges.handle).references!=1 ||
          sizeof(Header)+static_cast<std::uint64_t>(length)*sizeof(Value)>machine.memory.header(horizonChanges.handle).bytes) {
        auto changes=machine.memory.allocate(Kind::List,length);
        if (machine.memory.error!=Error::None) return {};
        for (std::uint32_t i=0;i<count;++i) machine.memory.items(changes)[i]=machine.memory.items(horizonChanges)[i];
        machine.memory.assign(horizonChanges,changes);
      } else machine.memory.header(horizonChanges.handle).length=length;
      const auto restartHorizon=replace ? machine.memory.items(horizonChanges)[length-1].z : 0;
      machine.memory.items(horizonChanges)[length-1]={Kind::Range,0,current.properties[0].x,static_cast<double>(next),restartHorizon};
      parameters.ticks=context.ticks;
      parameters.tickCount=static_cast<std::uint32_t>((next-context.initialTime)/parameters.tickMs);
      current.properties[2]=metadata.properties[2]=number(next);
      return {};
    }
    case Op::Restore: return restore(machine,a[0]);
    case Op::Restart: return restore(machine,parameters.restartSnapshot,true);
    case Op::Snapshot: {
      discardUnobservedHistory(machine);
      observeCurrent(machine); observePrevious(machine);
      // A second snapshot before the next physics wave completes the older
      // request synchronously; no unbounded per-lane copy queue is needed.
      finishNativeCopies(machine);
      const auto result=machine.memory.allocate(Kind::Snapshot,5);
      const auto contextId=parameters.restoreContextIds ? parameters.restoreContextIds[scratch.slot] : 0;
      // Only a future restore can observe a snapshot's private history cache.
      // Restart and horizon contexts alone must not force per-publication COW.
      const bool retainHistory=history.handle && mayReach(machine,parameters.restoreReachable);
      const auto native=machine.memory.allocate(Kind::List,retainHistory ? 6 : horizonChanges.handle ? 5 : contextId ? 4 : 3);
      bool newExtension=false;
      if (!snapshotExtension.handle) {
        snapshotExtension=machine.memory.allocate(Kind::Opaque,sizeof(CudaCandidateState),1);
        if (machine.memory.error!=Error::None) return {};
        if (!nativeCopies)
          *reinterpret_cast<CudaCandidateState *>(machine.memory.bytes+snapshotExtension.handle)=extended;
        newExtension=true;
        overflowDirtyCount=0;
        ++nativeVersions;
      }
      const auto physical=machine.memory.allocate(Kind::Opaque,sizeof(CudaCandidatePhysicsState),1);
      using Overflow=decltype(extended.collisionReplacementOverflow);
      static_assert(std::is_standard_layout_v<Overflow>);
      if (!machine.check(overflowDirtyCount<=CudaCollisionReplacementOverflowCapacity,Error::Bounds)) return {};
      const auto overflowBytes=offsetof(Overflow,values)+overflowDirtyCount*sizeof(GmVec3);
      const auto overflow=machine.memory.allocate(Kind::Opaque,static_cast<std::uint32_t>(overflowBytes),1);
      if (machine.memory.error!=Error::None) return {};
      if (nativeCopies) {
        *nativeCopies={newExtension ? snapshotExtension.handle : 0,physical.handle,overflow.handle,
            static_cast<std::uint32_t>(overflowBytes)};
        // VM code may drop the snapshot before the copy wave. Keep its opaque
        // destinations alive until the copy completes or the lane fails.
        if (newExtension) machine.memory.retain(snapshotExtension);
        machine.memory.retain(physical); machine.memory.retain(overflow);
      } else {
        *reinterpret_cast<CudaCandidatePhysicsState *>(machine.memory.bytes+physical.handle)=state;
        memcpy(machine.memory.bytes+overflow.handle,&extended.collisionReplacementOverflow,overflowBytes);
      }
      // Physics and collision data overlay a shared native extension. Writes
      // to the remaining fields invalidate its live cached version.
      machine.memory.items(native)[0]=machine.memory.copy(snapshotExtension);
      machine.memory.items(native)[1]=physical;
      machine.memory.items(native)[2]=overflow;
      if (contextId || horizonChanges.handle || retainHistory) machine.memory.items(native)[3]=number(contextId);
      if (horizonChanges.handle) machine.memory.items(native)[4]=machine.memory.copy(horizonChanges);
      if (retainHistory) machine.memory.items(native)[5]=machine.memory.copy(history);
      machine.memory.items(result)[0]=native;
      machine.memory.items(result)[1]=machine.memory.copy(inputs);
      machine.memory.items(result)[2]=readState(machine,current);
      machine.memory.items(result)[3]=readState(machine,previous);
      machine.memory.items(result)[4]=number(static_cast<double>(machine.candidates));
      return result;
    }
    case Op::SnapshotState: case Op::SnapshotInputs:
      if (!machine.array(a[0],Kind::Snapshot)) return {};
      return machine.memory.copy(machine.memory.items(a[0])[op==Op::SnapshotState ? 2 : 1]);
    case Op::UseInputs: {
      flush(machine);
      if (machine.error!=Error::None) return {};
      if (!machine.array(a[0],Kind::Inputs)) return {};
      const auto *events=machine.memory.items(a[0]), *old=machine.memory.items(inputs);
      const auto size=machine.memory.length(a[0]), oldSize=machine.memory.length(inputs);
      std::uint32_t prefix=0,special=0;
      for (std::uint32_t i=0;i<size;++i) {
        if (i && !machine.check(events[i-1].x<=events[i].x,Error::Unsorted)) return {};
        for (std::uint32_t j=i;j>0 && events[j-1].x==events[i].x;--j)
          if (!machine.check(events[j-1].z!=events[i].z,Error::DuplicateInput)) return {};
        if (events[i].x<=current.properties[0].x) {
          if (!machine.check(prefix<oldSize && machine.equal(events[i],old[prefix]),Error::PastInputs)) return {};
          ++prefix;
        }
        if (events[i].z==0 || events[i].z>=7) {
          while (special<oldSize && old[special].z>=1 && old[special].z<=6) ++special;
          if (!machine.check(special<oldSize && machine.equal(events[i],old[special]),Error::Unsupported)) return {};
          ++special;
        }
      }
      while (special<oldSize && old[special].z>=1 && old[special].z<=6) ++special;
      if (!machine.check(special==oldSize,Error::Unsupported)) return {};
      if (prefix<oldSize && !machine.check(old[prefix].x>current.properties[0].x,Error::PastInputs)) return {};
      machine.memory.assign(inputs,machine.memory.copy(a[0]));
      checkpointInputValidation={};
      rebuildControls(machine,current.properties[0].x); return {};
    }
    case Op::Step: {
      discardUnobservedHistory(machine);
      if (cancelled()) { machine.error=interpreterRequested() ? Error::Interpreter : Error::Cancelled; return {}; }
      if (!machine.check(tickIndex+pendingTicks<parameters.tickCount,Error::Horizon)) return {};
      if (parameters.historySizes) {
        const auto id=parameters.restoreContextIds ? parameters.restoreContextIds[scratch.slot] : 0;
        const auto prefix=horizonChanges.handle && machine.memory.length(horizonChanges) && machine.memory.items(horizonChanges)[0].z
            ? std::uint64_t{1} : parameters.historySizes[id];
        if (!machine.check(prefix+tickIndex+pendingTicks<parameters.collectionLimit,Error::Capacity)) return {};
      }
      ++pendingTicks;
      const auto nextTime=parameters.initialTime+static_cast<std::uint64_t>(tickIndex+pendingTicks)*parameters.tickMs;
      current.properties[0]=number(static_cast<double>(nextTime));
      current.properties[1]=number(static_cast<double>(nextTime/parameters.tickMs));
      if (pendingTicks==128) flush(machine);
      return {};
    }
    default: machine.error=Error::Unsupported; return {};
    }
  }
};

// Keep the physics loop separate from list/string/VM dispatch. A source read
// or input edit flushes pending ticks first; no observable state is skipped.
template<bool OrderedEllipsoids,bool Scheduled>
template<CudaHandlingSpecialization Handling>
__device__ __forceinline__ Value DevicePhysics<OrderedEllipsoids,Scheduled>::advancePending(Machine<DevicePhysics> &machine) {
  if (!pendingTicks || machine.error!=Error::None) return {};
  if (pendingTicks && machine.error==Error::None) reusePrefix(machine);
  if (!pendingTicks || machine.error!=Error::None) return {};
  auto working=state;
  auto localScratch=scratch;
  const auto configuration=parameters;
  const auto result=advanceTicks<Handling>(machine,working,localScratch,configuration);
  state=working; scratch=localScratch;
  return result;
}

template<bool OrderedEllipsoids,bool Scheduled>
template<CudaHandlingSpecialization Handling>
__device__ __forceinline__ Value DevicePhysics<OrderedEllipsoids,Scheduled>::advanceTicks(
    Machine<DevicePhysics> &machine,CudaCandidatePhysicsState &state,
    cuda::collision::CudaCollisionSearchScratch &scratch,const KernelParameters &parameters) {
  while (pendingTicks && machine.error==Error::None) {
      if (cancelled()) { machine.error=interpreterRequested() ? Error::Interpreter : Error::Cancelled; return {}; }
      // A flush cannot observe intermediate ticks. Only its final predecessor
      // is visible through previous-state after control returns to the VM.
      if (pendingTicks==1) {
        previousPhysics=state; previousAuxiliary=auxiliary(); previousDirty=true;
      }
      const auto nextTime=parameters.initialTime+static_cast<std::uint64_t>(tickIndex+1)*parameters.tickMs;
      consumeControls(machine,static_cast<double>(nextTime));
      auto tick=parameters.ticks[tickIndex];
      tick.controls=cuda_search_detail::ControlsFromState(controls);
      tick.stuntsInput=cuda_search_detail::StuntsFromState(controls,parameters.prestartMs);
      state.world.schemePeriodMs=tick.periodMs; state.world.tickTimeMs=tick.timeMs;
      if (!state.firstStep) {
        if (tick.actionFlags&CudaControlActionResetAtRaceStart) {
          invalidateSnapshotExtension(machine);
          static_cast<CudaCandidatePhysicsState &>(extended)=state;
          cuda::transition::PrepareStep(extended,tick,parameters.configuration);
          state=extended;
        } else cuda::transition::PrepareStep(state,tick,parameters.configuration);
      }
      state.vehicle.mobil.absorbContactEnabled=true;
      state.vehicle.mobil.physicsUpdatesEnabled=(tick.actionFlags&CudaControlActionSuppressVehicleForceCallbacks)==0;
      for (std::uint32_t i=0;i<tick.respawnAtCheckpointCount;++i) {
        invalidateSnapshotExtension(machine);
        static_cast<CudaCandidatePhysicsState &>(extended)=state;
        if (cuda::transition::Respawn(extended,parameters.configuration)) {
          state=extended; ++state.incrementalRespawnCount;
          cuda::stunts::ApplyRespawnPenalty(extended.stunts);
        }
      }
      auto before=static_cast<const CudaCandidatePhysicsState &>(state);
      const auto status=cuda::physics::Step<false,true,false,false,OrderedEllipsoids,false,true,Handling>(parameters.scene,parameters.configuration,state,scratch);
      if (!machine.check(status==cuda::physics::Status::Success,Error::Physics)) return {};
      // Capture writes the new prefix and zeroes retired entries. Preserve all
      // bytes it may have touched since the cached full snapshot was captured.
      if (overflowDirtyCount<scratch.replacementOverflowCount) overflowDirtyCount=scratch.replacementOverflowCount;
      if (overflowDirtyCount<extended.collisionReplacementOverflow.count) overflowDirtyCount=extended.collisionReplacementOverflow.count;
      cuda::collision::detail::CaptureReplacementOverflow(scratch,extended.collisionReplacementOverflow);
      if (!before.race.progress.raceCompleted && state.race.progress.raceCompleted) {
        // Refinement stops at the crossing substep. Keep the completed physics
        // tick, and use a copy solely to obtain its sub-tick finish estimate.
        cuda::finish::Refinement finish;
        const auto refined=cuda::finish::StepAndRefine<false,true,false,false,OrderedEllipsoids>(parameters.scene,parameters.configuration,before,tick,scratch,finish);
        if (!machine.check(refined==cuda::physics::Status::Success && finish.present && !finish.failed,Error::Physics)) return {};
        state.finishTime.present=true; state.finishTime.value=finish.estimate;
      }
      if (state.stuntsEnabled) {
        invalidateSnapshotExtension(machine);
        static_cast<CudaCandidatePhysicsState &>(extended)=state;
        if (!machine.check(cuda::stunts::Update(extended,tick)==cuda::stunts::Status::Success,Error::Physics)) return {};
      }
      state.firstStep=false; ++state.controlCursor; ++tickIndex; ++stepped;
      --pendingTicks;
      currentDirty=true;
      if (history.handle) {
        recordHistory(machine,state,nextTime);
        if (machine.memory.error!=Error::None) machine.error=machine.memory.error;
        if (machine.error!=Error::None) return {};
      }
  }
  return {};
}

__device__ inline Value CompactShallow(Memory &source,Memory &target,Value value,std::uint32_t maximumLength=UINT32_MAX) {
  if (!reference(value)) return value;
  auto &header=source.header(value.handle);
  if (header.next) return target.copy(Value{value.kind,header.next,value.x,value.y,value.z,value.w});
  const auto length=min(header.length,maximumLength);
  const auto result=target.allocate(value.kind,length,value.kind==Kind::Opaque ? 1 : sizeof(Value));
  if (target.error!=Error::None) return {};
  header.next=result.handle;
  if (value.kind==Kind::Opaque) {
    for (std::uint32_t i=0;i<length;++i) target.bytes[result.handle+i]=source.bytes[value.handle+i];
  }
  return result;
}

__device__ inline Value CompactValue(Memory &source,Memory &target,Value value) {
  const auto result=CompactShallow(source,target,value);
  if (!reference(value) || value.kind==Kind::Opaque || target.error!=Error::None) return result;
  struct Frame { Value source, target; std::uint32_t next; } frames[65];
  std::uint32_t depth=1;
  frames[0]={value,result,0};
  while (depth && target.error==Error::None) {
    auto &frame=frames[depth-1];
    if (frame.next==target.length(frame.target)) { --depth; continue; }
    const auto child=source.items(frame.source)[frame.next];
    const bool descend=reference(child) && child.kind!=Kind::Opaque && !source.header(child.handle).next;
    auto maximumLength=UINT32_MAX;
    if (frame.source.kind==Kind::Snapshot && frame.next==0 && child.kind==Kind::List && source.length(child)==6) {
      const auto *parts=source.items(child);
      if (parts[0].kind==Kind::Opaque && parts[0].handle && source.length(parts[0])==sizeof(CudaCandidateState) &&
          parts[1].kind==Kind::Opaque && parts[1].handle && source.length(parts[1])==sizeof(CudaCandidatePhysicsState) &&
          parts[2].kind==Kind::Opaque && parts[3].kind==Kind::Number && parts[5].kind==Kind::List) {
        // This is a private in-device restore cache, not a source list. The
        // existing native snapshot decoder reconstructs its sampled history.
        maximumLength=parts[4].kind!=Kind::None ? 5 : parts[3].x!=0 ? 4 : 3;
      }
    }
    const auto copied=CompactShallow(source,target,child,maximumLength);
    target.items(frame.target)[frame.next++]=copied;
    if (descend) {
      if (depth==65) { target.error=Error::Recursion; break; }
      frames[depth++]={child,copied,0};
    }
  }
  return result;
}

__device__ inline bool ClearCompactionLinks(Memory &source,Value value) {
  if (!reference(value) || !source.header(value.handle).next) return true;
  source.header(value.handle).next=0;
  if (value.kind==Kind::Opaque) return true;
  struct Frame { Value value; std::uint32_t next; } frames[65];
  std::uint32_t depth=1;
  frames[0]={value,0};
  while (depth) {
    auto &frame=frames[depth-1];
    if (frame.next==source.length(frame.value)) { --depth; continue; }
    const auto child=source.items(frame.value)[frame.next++];
    if (!reference(child) || !source.header(child.handle).next) continue;
    source.header(child.handle).next=0;
    if (child.kind==Kind::Opaque) continue;
    if (depth==65) return false;
    frames[depth++]={child,0};
  }
  return true;
}

template<bool OrderedEllipsoids> struct ScheduledLane {
  KernelParameters parameters;
  cuda::collision::CudaCollisionSearchScratch scratch;
  DevicePhysics<OrderedEllipsoids,true> physics;
  Machine<DevicePhysics<OrderedEllipsoids,true>> machine;
  bool finished=false,returned=false;
  Result result{};
  std::uint64_t vmCycles=0,physicsCycles=0;
  __device__ ScheduledLane(KernelParameters parameters,CudaCandidateState &state,std::uint32_t lane)
      : parameters(parameters),scratch{0,0,0,false,true,
        parameters.collisionStorage.collisions,parameters.collisionStorage.shapeCollisions,
        parameters.collisionStorage.shapeWorld,parameters.collisionStorage.movingBounds,
        parameters.collisionStorage.surfaceHits,parameters.collisionStorage.meshRanges,
        parameters.collisionStorage.meshCells,lane,parameters.count,parameters.collisionStorage.shapeCount},
        physics(this->parameters,state,scratch),machine(parameters.program,physics) {
    scratch.responseOrderStorage=parameters.collisionStorage.responseOrder;
    physics.mathRequest=parameters.mathRequests+lane;
    *physics.mathRequest=MathRequest{};
    if (parameters.nativeCopies) {
      physics.nativeCopies=parameters.nativeCopies+lane;
      *physics.nativeCopies={};
    }
  }
};

template<bool OrderedEllipsoids>
__device__ inline void InitializeLane(const KernelParameters &parameters,unsigned char *arenas,Value *stacks,
    CudaCandidateState *states,ScheduledLane<OrderedEllipsoids> &context,std::uint32_t lane,bool initializeStorage=true) {
  if (initializeStorage) states[lane]=*parameters.origin;
  new (&context) ScheduledLane<OrderedEllipsoids>(parameters,states[lane],lane);
  auto &machine=context.machine;
  machine.memory.bytes=arenas+static_cast<std::size_t>(lane)*parameters.arenaBytes;
  machine.memory.sharedBytes=parameters.sharedArena;
  if (initializeStorage)
    for (std::uint32_t i=0;i<parameters.initialBytes;++i) machine.memory.bytes[i]=parameters.initialArena[i];
  machine.memory.capacity=parameters.arenaBytes; machine.memory.used=parameters.initialBytes;
  machine.globals=parameters.globals; machine.stack=stacks+lane*1024u; machine.stackCapacity=1024u;
  machine.tickMs=parameters.tickMs; machine.batchSize=parameters.batchSize;
  context.physics.initialize(machine);
}

// Separate registration keeps source cursor startup independent of the mapped VM module.
const void *SelectCursorInitializeKernel(bool ordered);
const void *SelectCursorKernel(bool ordered,const CudaPackedStaticConfigurationHeader &configuration);

} // namespace forevertas::blocks::cuda_program_detail

#endif
