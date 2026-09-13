#include "blocks/program_cuda.h"
#include "blocks/program_cuda_kernel.h"
#include "blocks/program_cuda_import.h"
#include "blocks/visual_debugger.h"
#include "blocks/visual_state_horizon.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace forevertas::blocks {
namespace {
void CopyNative(vm::Memory &memory,void *destination,vm::Value source,std::size_t size) {
  if (source.kind!=vm::Kind::Opaque || !source.handle || memory.length(source)!=size)
    throw std::runtime_error("Invalid CUDA physical snapshot.");
  std::memcpy(destination,memory.bytes+source.handle,size);
}

void OverlayNative(vm::Memory &memory,forevervalidator::simulation::CudaCandidateState &packed,
    vm::Value physical,vm::Value collision) {
  forevervalidator::simulation::CudaCandidatePhysicsState active;
  CopyNative(memory,&active,physical,sizeof(active));
  static_cast<forevervalidator::simulation::CudaCandidatePhysicsState &>(packed)=active;
  if (collision.kind==vm::Kind::None) return;
  using Overflow=decltype(packed.collisionReplacementOverflow);
  if (collision.kind!=vm::Kind::Opaque || !collision.handle)
    throw std::runtime_error("Invalid CUDA collision snapshot.");
  const auto bytes=memory.length(collision);
  if (bytes<offsetof(Overflow,values) || bytes>sizeof(Overflow) ||
      (bytes - offsetof(Overflow,values))%sizeof(packed.collisionReplacementOverflow.values[0]))
    throw std::runtime_error("Invalid CUDA collision snapshot.");
  CopyNative(memory,&packed.collisionReplacementOverflow,collision,bytes);
}

struct HostMath {
  bool cancelled() const { return false; }
  bool interpreterRequested() const { return false; }
  vm::Value apply(vm::Op,vm::Machine<HostMath> &machine,vm::Value *,std::uint32_t) {
    machine.error=vm::Error::Unsupported; return {};
  }
};
}

vm::Result EvaluateCudaProgramHostMath(vm::Op op,vm::Value *arguments) {
  HostMath physics;
  vm::Machine<HostMath> machine({},physics);
  if (!vm::requiresHostMath(op)) return {{},vm::Error::Unsupported};
  const auto value=machine.builtin(op,arguments,0);
  return {value,machine.error};
}

struct CudaProgramPrefixCache::Impl {
  struct Origin {
    std::shared_ptr<const unsigned char> identity;
    std::uint64_t time;
    std::uint32_t horizon;
    VisualState last;
  };
  struct Entry {
    std::shared_ptr<const unsigned char> origin;
    std::uint32_t tick,horizon;
    forevervalidator::simulation::CudaCandidateState physics;
    VisualState state,previous;
    VisualInputs inputs;
  };
  std::optional<Origin> active;
  std::vector<std::shared_ptr<const Entry>> entries;
  bool enabled=std::getenv("FOREVERTAS_CUDA_VM_PREFIX_DISABLE")==nullptr;
  static constexpr std::uint32_t denseTicks=128;
  static bool checkpoint(std::uint64_t elapsed) {
    return elapsed && elapsed%10==0 && (elapsed<=denseTicks*10 || elapsed%320==0);
  }
  template<class Capture>
  void record(const VisualState &state,const VisualState &previous,const VisualInputs &inputs,Capture capture) {
    auto &origin=*active;
    const auto end=std::upper_bound(inputs.begin(),inputs.end(),state.timeMs,
        [](std::uint64_t time,const SandboxInputEvent &event) { return static_cast<std::int64_t>(time)<event.timeMs; });
    VisualInputs consumed(inputs.begin(),end);
    const auto tick=static_cast<std::uint32_t>((state.timeMs-origin.time)/10);
    for (const auto &entry : entries)
      if (entry->origin==origin.identity && entry->tick==tick && entry->horizon==origin.horizon &&
          std::equal(consumed.begin(),consumed.end(),entry->inputs.begin(),entry->inputs.end(),SameInputEvent)) return;
    auto entry=std::make_shared<Entry>(Entry{origin.identity,tick,origin.horizon,{},state,previous,std::move(consumed)});
    if (!capture(entry->physics)) return;
    // Preserve early tick observations independently of the coarse history.
    const bool dense=tick<=denseTicks;
    const auto sameClass=[&](const auto &value) { return (value->tick<=denseTicks)==dense; };
    if (static_cast<std::size_t>(std::count_if(entries.begin(),entries.end(),sameClass))>=(dense ? denseTicks : 64u))
      entries.erase(std::find_if(entries.begin(),entries.end(),sameClass));
    entries.push_back(std::move(entry));
  }
};

CudaProgramPrefixCache::CudaProgramPrefixCache() : impl_(std::make_unique<Impl>()) {}
CudaProgramPrefixCache::~CudaProgramPrefixCache()=default;
void CudaProgramPrefixCache::start(const VisualPhysicsSnapshot &origin,std::uint32_t horizon,bool replace) {
  if (!impl_->enabled || (!replace && impl_->active)) return;
  const auto &view=origin.state.View();
  impl_->active=Impl::Origin{origin.identity,view.timeMs,horizon,view};
}
void CudaProgramPrefixCache::invalidate() { impl_->active.reset(); }
bool CudaProgramPrefixCache::needsNativeState(const VisualState &state) const {
  return impl_->active && state.timeMs>impl_->active->time && Impl::checkpoint(state.timeMs-impl_->active->time);
}
void CudaProgramPrefixCache::observe(forevervalidator::experimental::PhysicsSandbox &sandbox,
    const VisualState &state,const VisualState *previous) try {
  if (!impl_->active) return;
  auto &origin=*impl_->active;
  // Native capture is comparatively expensive; dense frames come from the
  // resident cursor's already-transferred trajectory instead.
  const bool checkpoint=state.timeMs>origin.time && (state.timeMs-origin.time)%320==0;
  if (!checkpoint) { origin.last=state; return; }
  const auto before=previous ? *previous : origin.last;
  origin.last=state;
  if (before.timeMs+10!=state.timeMs) return;
  auto inputs=sandbox.ReadInputs();
  if (!inputs) return;
  impl_->record(state,before,inputs.Value(),[&](auto &physics) {
    const auto context=forevervalidator::experimental::PhysicsSandboxCudaExecutionAccess::Capture(sandbox);
    if (!context) return false;
    physics=context.Value().physics.initialState;
    return true;
  });
} catch (const std::bad_alloc &) {
  // Checkpoint recording is optional; allocation pressure must not invalidate
  // an otherwise executable source program.
  impl_->entries.clear(); impl_->active.reset();
}
void CudaProgramPrefixCache::append(CudaProgramInput &input,ProgramValueCodec &codec,
    vm::Memory &memory,const VisualSnapshot &origin) const {
  const auto *native=dynamic_cast<const VisualPhysicsSnapshot *>(origin.native.get());
  if (!native || !impl_->enabled) return;
  auto checkpoints=std::make_shared<std::vector<CudaProgramCheckpoint>>();
  auto events=std::make_shared<std::vector<vm::Value>>();
  const auto flatten=[&](const VisualState &state,auto &target) {
    const auto encoded=codec.encode(VisualValue(state));
    for (std::uint32_t i=0;i<CudaProgramStatePropertyCount;++i) {
      auto value=memory.items(encoded)[i];
      if (i>=30 && i<=33) {
        const auto *items=memory.items(value);
        value={vm::Kind::List,0,items[0].x,items[1].x,items[2].x,items[3].x};
      }
      target[i]=value;
    }
    memory.release(encoded);
  };
  std::vector<const Impl::Entry *> selected;
  for (const auto &entry : impl_->entries) {
    if (entry->origin!=native->identity || entry->horizon!=origin.horizonMs ||
        entry->state.timeMs!=origin.state.timeMs+entry->tick*static_cast<std::uint64_t>(input.tickMs)) continue;
    selected.push_back(entry.get());
  }
  std::stable_sort(selected.begin(),selected.end(),[](const auto *a,const auto *b) { return a->tick<b->tick; });
  std::optional<forevervalidator::simulation::CudaCandidateState> extension;
  std::uint32_t generation=0;
  for (const auto *entry : selected) {
    CudaProgramCheckpoint checkpoint;
    checkpoint.state=entry->physics; checkpoint.tick=entry->tick;
    // Exclude the separately overlaid fields. Comparing every remaining byte
    // makes extension sharing conservative, including padding and future fields.
    auto normalized=entry->physics;
    static_cast<forevervalidator::simulation::CudaCandidatePhysicsState &>(normalized)={};
    normalized.collisionReplacementOverflow={};
    if (!extension || std::memcmp(&normalized,&*extension,sizeof(normalized))!=0) {
      extension=std::move(normalized);
      ++generation;
    }
    checkpoint.extensionGeneration=generation;
    flatten(entry->state,checkpoint.current); flatten(entry->previous,checkpoint.previous);
    const auto encoded=codec.encode(VisualValue(entry->inputs));
    checkpoint.inputOffset=static_cast<std::uint32_t>(events->size());
    checkpoint.inputCount=memory.length(encoded);
    events->insert(events->end(),memory.items(encoded),memory.items(encoded)+checkpoint.inputCount);
    memory.release(encoded);
    checkpoints->push_back(std::move(checkpoint));
  }
  input.checkpoints=std::move(checkpoints); input.checkpointInputs=std::move(events);
  if (std::getenv("FOREVERTAS_CUDA_VM_PROFILE"))
    std::fprintf(stderr,"CUDA_VM_PREFIX_PROFILE checkpoints=%zu generations=%u\n",selected.size(),generation);
}
namespace {
using namespace forevervalidator::experimental;

template<class T> T Require(PhysicsSandboxResult<T> result,const char *operation) {
  if (!result) throw std::runtime_error(std::string(operation)+": "+result.Error().diagnostic);
  return std::move(result.Value());
}

void CheckStateProperties() {
  static const std::vector<std::string> properties={"time","tick","duration","position","velocity","local-velocity","angular-velocity",
    "force","torque","rotation","rotation-x","rotation-y","rotation-z","rotation-w","speed","signed-speed","accelerate","brake","steering",
    "gear","rpm","turning-rate","sliding","freewheeling","lateral-contact","turbo","turbo-type","turbo-boost","burning","gear-changed",
    "wheel-contact","wheel-surface","wheel-has-surface","wheel-sliding","camera-up","camera-flight","checkpoints","total-checkpoints",
    "laps","total-laps","finished","finish-time","precise-finish-time","finish-lower-bound","finish-upper-bound","respawns","stunt-points",
    "environment","vehicle","play-mode"};
  if (properties.size()!=VisualStateProperties().size()) throw std::logic_error("CUDA state property ABI mismatch.");
  for (std::size_t i=0;i<properties.size();++i)
    if (VisualStateProperties()[i].first!=properties[i]) throw std::logic_error("CUDA state property order mismatch.");
}

class CudaExecutor final : public VisualBatchExecutor {
public:
  CudaExecutor(PhysicsSandbox &sandbox,ProgramHistoryReplay replay,std::shared_ptr<CudaProgramPrefixCache> prefixes)
      : sandbox_(sandbox),replay_(std::move(replay)),prefixes_(std::move(prefixes)) {}
  ~CudaExecutor() override {
    if (!std::getenv("FOREVERTAS_CUDA_VM_PROFILE")) return;
    const auto started=std::chrono::steady_clock::now();
    kernel_.reset();
    std::fprintf(stderr,"CUDA_VM_RELEASE_PROFILE milliseconds=%.6f\n",
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
  }
  std::optional<BytecodeBatchResult> execute(const ProgramBytecode &program,
      const std::shared_ptr<const VisualSnapshot> &baseline,
      const std::map<std::string,VisualValue> &globals,const VisualList &arguments,
      const std::vector<std::uint64_t> &seeds,const VisualRuntimeControl &control) override try {
    const bool profile=std::getenv("FOREVERTAS_CUDA_VM_PROFILE")!=nullptr;
    const auto started=std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration importTime{};
    std::size_t nativeSnapshots=0;
    const auto fallback=[&](const std::string &reason) -> std::optional<BytecodeBatchResult> {
      if (control.executionModeChanged)
        control.executionModeChanged("CUDA physics with CPU block control: "+reason);
      return {};
    };
    if (control.executionModeChanged)
      control.executionModeChanged("Preparing CUDA block program");
    const auto *physical=dynamic_cast<const VisualPhysicsSnapshot *>(baseline->native.get());
    if (!physical || arguments.empty()) return fallback("no compatible branch state");
    if (control.debugger && control.debugger->enabled()) return fallback("debugger enabled");
    const auto context=Require(PhysicsSandboxCudaExecutionAccess::Capture(sandbox_),"Preparing programmable CUDA");
    // This adapter's public state layout is checked against the source registry,
    // rather than silently interpreting a different property order.
    CheckStateProperties();
    CudaProgramInput input; input.program=&program; input.context=&context; input.tickMs=control.tickMs;
    input.batchSize=control.batchSize;
    input.collectionLimit=control.collectionLimit;
    const bool readsHistory=std::any_of(program.code.begin(),program.code.end(),[](const auto &instruction) {
      return instruction.op==vm::Op::History;
    });
    const bool hasRestore=std::any_of(program.code.begin(),program.code.end(),[](const auto &instruction) {
      return instruction.op==vm::Op::Restore;
    });
    const bool hasRestart=std::any_of(program.code.begin(),program.code.end(),[](const auto &instruction) {
      return instruction.op==vm::Op::Restart;
    });
    if (readsHistory) {
      input.historyReachable=HistoryReadReachability(program);
      input.restoreReachable=RestoreReachability(program);
    }
    constexpr std::uint32_t maximumImportCapacity=64u*1024u*1024u;
    vm::Memory memory;
    std::unique_ptr<ProgramValueCodec> encoded;
    std::vector<std::shared_ptr<const VisualSnapshot>> importedSnapshots;
    const auto dynamicReads=ResolveDynamicInitialGlobals(program,globals,arguments);
    const auto &initialReads=dynamicReads ? *dynamicReads : program.initialGlobalReads;
    for (;;) {
      if (control.stopRequested && control.stopRequested()) return BytecodeBatchResult{{},0,true};
      try {
        input.arena.resize(input.arenaCapacity);
        memory=vm::Memory{}; memory.bytes=input.arena.data(); memory.capacity=input.arenaCapacity;
        encoded=std::make_unique<ProgramValueCodec>(program,memory);
        input.arguments.clear(); importedSnapshots.clear();
        auto &codec=*encoded;
        // Imported snapshots expose immutable metadata on the device. A compact
        // token returns the original native state and history without rebuilding
        // an earlier snapshot relative to this map's later baseline.
        codec.setSnapshotEncoder([&](const auto &snapshot,vm::Value inputs) {
          if (importedSnapshots.size()>=UINT32_MAX) throw BytecodeNeedsInterpreter{};
          const auto value=memory.allocate(vm::Kind::Snapshot,5);
          const auto token=memory.allocate(vm::Kind::Opaque,sizeof(std::uint32_t),1);
          if (memory.error!=vm::Error::None) throw std::runtime_error(BytecodeError(memory.error));
          const auto index=static_cast<std::uint32_t>(importedSnapshots.size());
          std::memcpy(memory.bytes+token.handle,&index,sizeof(index));
          auto *fields=memory.items(value);
          fields[0]=token; fields[1]=memory.copy(inputs);
          fields[2]=codec.encode(VisualValue(snapshot->state));
          fields[3]=codec.encode(VisualValue(snapshot->previous));
          fields[4]=vm::number(static_cast<double>(snapshot->candidate));
          importedSnapshots.push_back(snapshot);
          return value;
        });
        input.globals=memory.allocate(vm::Kind::List,static_cast<std::uint32_t>(program.symbols.size()));
        if (memory.error!=vm::Error::None) throw std::runtime_error(BytecodeError(memory.error));
        for (std::size_t i=0;i<program.symbols.size();++i) {
          auto &slot=memory.items(input.globals)[i]; slot.kind=vm::Kind::Unset;
          if (initialReads.size()==program.symbols.size() && !initialReads[i]) continue;
          const auto value=globals.find(program.symbols[i]);
          if (value!=globals.end()) slot=codec.encode(value->second);
        }
        input.inputs=codec.encode(VisualValue(*baseline->inputs));
        input.initialView=codec.encode(VisualValue(baseline->state));
        input.previousView=codec.encode(VisualValue(baseline->previous));
        for (const auto &argument : arguments) input.arguments.push_back(codec.encode(argument));
        if (hasRestart) input.restartSnapshot=codec.encode(VisualValue(control.restartOrigin ? control.restartOrigin : baseline));
        input.historySizes.assign(1,baseline->history->size);
        for (const auto &snapshot : importedSnapshots) input.historySizes.push_back(snapshot->history->size);
        if (readsHistory) {
          const auto count=hasRestore ? importedSnapshots.size()+1 : 1;
          input.histories=memory.allocate(vm::Kind::List,static_cast<std::uint32_t>(count));
          if (memory.error!=vm::Error::None) throw std::runtime_error(BytecodeError(memory.error));
          std::map<const VisualHistoryNode *,vm::Value> cached;
          for (std::size_t i=0;i<count;++i) {
            if (control.stopRequested && control.stopRequested()) return BytecodeBatchResult{{},0,true};
            const auto &history=i ? importedSnapshots[i-1]->history : baseline->history;
            const auto found=cached.find(history.get());
            if (found!=cached.end()) { memory.items(input.histories)[i]=memory.copy(found->second); continue; }
            const auto states=VisualHistoryStates(history);
            if (states.size()>UINT32_MAX) throw BytecodeNeedsInterpreter{};
            const auto list=memory.allocate(vm::Kind::List,static_cast<std::uint32_t>(states.size()));
            if (memory.error!=vm::Error::None) throw std::runtime_error(BytecodeError(memory.error));
            memory.items(input.histories)[i]=list;
            cached.emplace(history.get(),list);
            for (std::size_t tick=0;tick<states.size();++tick) {
              if ((tick&255u)==0 && control.stopRequested && control.stopRequested()) return BytecodeBatchResult{{},0,true};
              memory.items(list)[tick]=codec.encode(VisualValue(states[tick]));
            }
          }
        }
        if (prefixes_) prefixes_->append(input,codec,memory,*baseline);
        break;
      } catch (const std::runtime_error &) {
        if (memory.error!=vm::Error::Capacity) throw;
        if (input.arenaCapacity==maximumImportCapacity) return fallback("initial values exceed the GPU arena");
        // Rebuild all handles and native tokens together. Growing in place
        // would invalidate references held by recursive value encoding.
        input.arenaCapacity=std::min(input.arenaCapacity*2,maximumImportCapacity);
      } catch (const std::bad_alloc &) {
        return fallback("insufficient memory to import initial values");
      }
    }
    std::vector<std::shared_ptr<const CudaProgramRestoreContext>> restoreContexts;
    std::vector<std::shared_ptr<const CudaProgramRestoreContext>> horizonContexts(importedSnapshots.size()+1);
    std::vector<std::shared_ptr<const VisualSnapshot>> horizonOrigins(importedSnapshots.size()+1);
    input.importedSnapshotCount=static_cast<std::uint32_t>(importedSnapshots.size());
    if (hasRestore || hasRestart) {
      restoreContexts.resize(importedSnapshots.size());
      input.restoreContext=[&](std::uint32_t index) -> std::shared_ptr<const CudaProgramRestoreContext> {
        if (index>=importedSnapshots.size()) return {};
        if (restoreContexts[index]) return restoreContexts[index];
        const auto &snapshot=importedSnapshots[index];
        const auto *native=dynamic_cast<const VisualPhysicsSnapshot *>(snapshot->native.get());
        if (!native || !snapshot->inputs || !snapshot->history) return {};
        auto saved=std::make_shared<CudaProgramRestoreContext>();
        saved->reuseBaseline=native->identity && native->identity==physical->identity &&
            snapshot->state.timeMs==baseline->state.timeMs && snapshot->horizonMs==baseline->horizonMs &&
            std::equal(snapshot->inputs->begin(),snapshot->inputs->end(),baseline->inputs->begin(),baseline->inputs->end(),SameInputEvent);
        if (!saved->reuseBaseline) {
          const auto backup=Require(sandbox_.CaptureState(),"Saving host before CUDA restore preparation");
          const auto backupView=Require(sandbox_.ReadState(),"Reading host before CUDA restore preparation");
          const auto backupInputs=Require(sandbox_.ReadInputs(),"Saving inputs before CUDA restore preparation");
          const auto restoreHost=[&](const auto &state,const auto &inputs,std::uint32_t horizon) {
            const auto current=Require(sandbox_.ReadState(),"Reading CUDA restore preparation cursor");
            Require(sandbox_.SetSimulationHorizonMs(std::max(horizon,static_cast<std::uint32_t>(current.timeMs))),
                "Extending CUDA restore preparation horizon");
            Require(sandbox_.RestoreState(state),"Restoring CUDA preparation state");
            Require(sandbox_.ReplaceInputs(inputs),"Restoring CUDA preparation inputs");
            Require(sandbox_.SetSimulationHorizonMs(horizon),"Restoring CUDA preparation horizon");
          };
          std::exception_ptr failure;
          try {
            restoreHost(native->state,*snapshot->inputs,snapshot->horizonMs);
            saved->context=std::make_shared<const PhysicsSandboxCudaExecutionContext>(
                Require(PhysicsSandboxCudaExecutionAccess::Capture(sandbox_),"Preparing imported CUDA restore context"));
          } catch (...) { failure=std::current_exception(); }
          restoreHost(backup,backupInputs,VisualStateHorizonMs(backupView));
          if (failure) std::rethrow_exception(failure);
          if (prefixes_) {
            CudaProgramInput cached; cached.tickMs=control.tickMs;
            std::vector<unsigned char> bytes(2u*1024u*1024u);
            for (;;) {
              vm::Memory staging; staging.bytes=bytes.data(); staging.capacity=static_cast<std::uint32_t>(bytes.size());
              ProgramValueCodec codec(program,staging);
              try {
                prefixes_->append(cached,codec,staging,*snapshot);
                saved->checkpoints=std::move(cached.checkpoints); saved->checkpointInputs=std::move(cached.checkpointInputs);
                break;
              } catch (const std::runtime_error &) {
                if (staging.error!=vm::Error::Capacity) throw;
                if (bytes.size()==maximumImportCapacity) break;
                bytes.resize(std::min<std::size_t>(bytes.size()*2,maximumImportCapacity));
              }
            }
          }
        }
        restoreContexts[index]=saved;
        return saved;
      };
    }
    input.horizonContext=[&](std::uint32_t id,std::uint32_t horizon) -> std::shared_ptr<const CudaProgramRestoreContext> {
      if (id>importedSnapshots.size()) return {};
      if (horizonOrigins[id] && horizonOrigins[id]->horizonMs>=horizon) return horizonContexts[id];
      const auto base=id ? importedSnapshots[id-1] : baseline;
      const auto *native=dynamic_cast<const VisualPhysicsSnapshot *>(base->native.get());
      if (!native || !base->inputs || !base->history) return {};
      const auto prior=horizonContexts[id] ? horizonContexts[id] : id && input.restoreContext ? input.restoreContext(id-1) : nullptr;
      const auto &old=prior && prior->context ? *prior->context : *input.context;
      if (prior && horizon<=old.state.timeMs+static_cast<std::uint64_t>(old.ticks.size())*control.tickMs) return prior;
      const auto maximum=2147481040u/control.tickMs*control.tickMs;
      horizon=static_cast<std::uint32_t>(std::max<std::uint64_t>(horizon,std::min<std::uint64_t>(maximum,
          old.state.timeMs+static_cast<std::uint64_t>(old.ticks.size())*control.tickMs*2)));
      const auto backup=Require(sandbox_.CaptureState(),"Saving host before CUDA horizon preparation");
      const auto backupView=Require(sandbox_.ReadState(),"Reading host before CUDA horizon preparation");
      const auto backupInputs=Require(sandbox_.ReadInputs(),"Saving inputs before CUDA horizon preparation");
      const auto restoreHost=[&](const auto &state,const auto &inputs,std::uint32_t duration) {
        const auto current=Require(sandbox_.ReadState(),"Reading CUDA horizon preparation cursor");
        Require(sandbox_.SetSimulationHorizonMs(std::max(duration,static_cast<std::uint32_t>(current.timeMs))),
            "Extending CUDA horizon preparation horizon");
        Require(sandbox_.RestoreState(state),"Restoring CUDA horizon preparation state");
        Require(sandbox_.ReplaceInputs(inputs),"Restoring CUDA horizon preparation inputs");
        Require(sandbox_.SetSimulationHorizonMs(duration),"Restoring CUDA horizon preparation horizon");
      };
      auto saved=std::make_shared<CudaProgramRestoreContext>();
      auto wide=std::make_shared<VisualSnapshot>(*base);
      std::exception_ptr failure;
      try {
        restoreHost(native->state,*base->inputs,horizon);
        saved->context=std::make_shared<const PhysicsSandboxCudaExecutionContext>(
            Require(PhysicsSandboxCudaExecutionAccess::Capture(sandbox_),"Preparing extended CUDA horizon context"));
        wide->native=std::make_shared<VisualPhysicsSnapshot>(Require(sandbox_.CaptureState(),"Capturing extended CUDA horizon origin"));
        wide->horizonMs=horizon; SetVisualStateHorizonMs(wide->state,horizon);
      } catch (...) { failure=std::current_exception(); }
      restoreHost(backup,backupInputs,VisualStateHorizonMs(backupView));
      if (failure) std::rethrow_exception(failure);
      const auto &next=*saved->context;
      if (old.state.timeMs!=next.state.timeMs || old.ticks.size()>next.ticks.size())
        throw BytecodeNeedsInterpreter{};
      // Observation/comparison flags describe reporting at the old endpoint;
      // every field used by physics must remain identical in the wider plan.
      for (std::size_t i=0;i<old.ticks.size();++i) {
        const auto &a=old.ticks[i], &b=next.ticks[i];
        if (a.periodMs!=b.periodMs || a.timeMs!=b.timeMs || a.actionFlags!=b.actionFlags ||
            a.stuntsTimeLimitMs!=b.stuntsTimeLimitMs || a.respawnAtCheckpointCount!=b.respawnAtCheckpointCount ||
            a.controls.lowSpeedGateA!=b.controls.lowSpeedGateA || a.controls.lowSpeedGateB!=b.controls.lowSpeedGateB ||
            a.controls.steering!=b.controls.steering || a.stuntsInput.lastChangeTimeMs!=b.stuntsInput.lastChangeTimeMs)
          throw BytecodeNeedsInterpreter{};
      }
      if (prior && !prior->reuseBaseline) { saved->checkpoints=prior->checkpoints; saved->checkpointInputs=prior->checkpointInputs; }
      else { saved->checkpoints=input.checkpoints; saved->checkpointInputs=input.checkpointInputs; }
      horizonContexts[id]=saved; horizonOrigins[id]=wide;
      return saved;
    };
    input.arena.resize(memory.used);
    const auto originalImportBytes=memory.used;
    const auto *sharedPolicy=std::getenv("FOREVERTAS_CUDA_VM_SHARED_IMPORTS");
    if ((!sharedPolicy || std::strcmp(sharedPolicy,"0")!=0) && input.arena.size()>=1024u*1024u) {
      try {
        cuda_program_detail::SharedImportBuilder packed(input.arena);
        input.globals=packed.copy(input.globals,true);
        input.inputs=packed.copy(input.inputs);
        input.initialView=packed.copy(input.initialView);
        input.previousView=packed.copy(input.previousView);
        input.histories=packed.copy(input.histories);
        input.restartSnapshot=packed.copy(input.restartSnapshot);
        for (auto &value : input.arguments) value=packed.copy(value);
        if (input.checkpoints) {
          auto checkpoints=std::make_shared<std::vector<CudaProgramCheckpoint>>(*input.checkpoints);
          for (auto &checkpoint : *checkpoints) {
            for (auto &value : checkpoint.current) value=packed.copy(value);
            for (auto &value : checkpoint.previous) value=packed.copy(value);
          }
          input.checkpoints=std::move(checkpoints);
        }
        if (input.checkpointInputs) {
          auto inputs=std::make_shared<std::vector<vm::Value>>(*input.checkpointInputs);
          for (auto &value : *inputs) value=packed.copy(value);
          input.checkpointInputs=std::move(inputs);
        }
        packed.finish();
        input.arena=std::move(packed.local);
        input.sharedArena=std::make_shared<const std::vector<unsigned char>>(std::move(packed.payloads));
        memory.bytes=input.arena.data(); memory.used=static_cast<std::uint32_t>(input.arena.size());
      } catch (const std::bad_alloc &) {
        return fallback("insufficient memory to import initial values");
      }
    }
    if (profile) std::fprintf(stderr,"CUDA_VM_IMPORT_PROFILE originalBytes=%u privateBytes=%zu sharedBytes=%zu\n",
        originalImportBytes,input.arena.size(),input.sharedArena ? input.sharedArena->size() : 0);
    std::uint32_t importedCapacity=128u*1024u;
    while (importedCapacity<memory.used) importedCapacity*=2;
    auto importClassCapacity=importedCapacity;
    while (importClassCapacity<originalImportBytes) importClassCapacity*=2;
    unsigned arenaClass=0;
    for (auto capacity=2u*1024u*1024u;capacity<importClassCapacity;capacity*=2) ++arenaClass;
    // Large imports must not inflate subsequent small maps. Keep their
    // learned working capacities in separate, bounded power-of-two classes.
    auto &capacities=capacities_[readsHistory ? 1 : 0];
    auto &arenaCapacity=importClassCapacity>1024u*1024u ? capacities.importedArenas[arenaClass] : capacities.arena;
    auto &outputCapacity=importClassCapacity>1024u*1024u ? capacities.importedOutputs[arenaClass] : capacities.output;
    arenaCapacity=std::max(arenaCapacity,importedCapacity);
    outputCapacity=std::max(outputCapacity,128u*1024u);
    const auto workingCapacityLimit=readsHistory ? maximumImportCapacity : std::max(2u*1024u*1024u,
        std::min(importClassCapacity*2,maximumImportCapacity));
    bool historyAfterTick=false;
    for (std::size_t i=0;i<input.historyReachable.size();++i)
      historyAfterTick=historyAfterTick || (program.code[i].op==vm::Op::Step && input.historyReachable[i]);
    if (historyAfterTick) {
      // Budget the ordinary per-frame storage up front. Extra retained values
      // still use bounded arena growth; this estimate is not a semantic limit.
      const auto allocation=[](std::uint64_t count) {
        std::uint64_t bytes=32;
        while (bytes<sizeof(vm::Header)+count*sizeof(vm::Value)) bytes*=2;
        return bytes;
      };
      const auto bytes=originalImportBytes+context.ticks.size()*(allocation(CudaProgramStatePropertyCount)+4*allocation(4));
      std::uint32_t hint=128u*1024u;
      while (hint<bytes && hint<workingCapacityLimit) hint*=2;
      arenaCapacity=std::max(arenaCapacity,hint);
    }
    // A returned imported value may be larger than the ordinary output budget.
    // Grow serialization storage lazily, under the same bounded wave policy.
    input.outputCapacityLimit=workingCapacityLimit;
    // Native snapshot payloads and a replacement result exceed the small arena
    // before useful simulation starts. Other programs keep the smaller default.
    if (std::any_of(program.code.begin(),program.code.end(),[](const auto &instruction) {
          return instruction.op==vm::Op::Snapshot || instruction.op==vm::Op::Publish;
        })) arenaCapacity=std::max(arenaCapacity,256u*1024u);
    input.arenaCapacity=arenaCapacity;
    input.outputCapacity=outputCapacity;
    // Keep the template small: splitting a large logical map must not copy
    // every argument and seed again for each memory-bounded wave.
    const auto argumentValues=std::move(input.arguments);
    BytecodeBatchResult result; result.values.reserve(arguments.size());
    if (!kernel_) kernel_=std::make_unique<CudaProgramKernel>();
    auto waveSize=kernel_->suggestedWaveSize(input.arenaCapacity,input.outputCapacity);
    for (std::size_t first=0;first<arguments.size();) {
      if (control.stopRequested && control.stopRequested()) return BytecodeBatchResult{{},result.operations,true};
      const auto last=std::min(arguments.size(),first+waveSize);
      CudaProgramInput wave=input;
      wave.arguments.assign(argumentValues.begin()+static_cast<std::ptrdiff_t>(first),argumentValues.begin()+static_cast<std::ptrdiff_t>(last));
      wave.seeds.assign(seeds.begin()+static_cast<std::ptrdiff_t>(first),seeds.begin()+static_cast<std::ptrdiff_t>(last));
      std::vector<CudaProgramOutput> outputs;
      try {
        outputs=kernel_->execute(wave,[&] {
          if (control.progress) control.progress();
          return (control.stopRequested && control.stopRequested()) || (control.debugger && control.debugger->enabled());
        });
      } catch (const std::bad_alloc &) {
        // Available VRAM can change after budgeting (for example, a viewer or
        // another application allocating textures). Retry only this wave.
        if (profile) std::fprintf(stderr,"CUDA_VM_ALLOCATION_RETRY lanes=%zu arena=%u output=%u\n",
            last-first,input.arenaCapacity,input.outputCapacity);
        kernel_.reset();
        if (waveSize==1) {
          while (result.values.size()<arguments.size()) {
            result.fallbackLanes.push_back(result.values.size()); result.values.emplace_back();
          }
          break;
        }
        waveSize=std::max<std::size_t>(1,waveSize/2);
        kernel_=std::make_unique<CudaProgramKernel>();
        continue;
      }
      for (const auto &output : outputs) outputCapacity=std::max(outputCapacity,output.outputCapacity);
      input.outputCapacity=outputCapacity;
      const bool workingCapacityExceeded=std::any_of(outputs.begin(),outputs.end(),[](const auto &output) {
        return output.result.error==vm::Error::Capacity && !output.outputCapacityExceeded;
      });
      const bool outputCapacityExceeded=std::any_of(outputs.begin(),outputs.end(),[](const auto &output) {
        return output.outputCapacityExceeded;
      });
      if ((workingCapacityExceeded && input.arenaCapacity<workingCapacityLimit) ||
          (outputCapacityExceeded && input.outputCapacity<input.outputCapacityLimit)) {
        // Retry only an uncommitted wave, with exactly the same arguments and
        // RNG seeds. Small programs need not reserve the worst-case arena.
        if (workingCapacityExceeded)
          arenaCapacity=input.arenaCapacity=std::min(input.arenaCapacity*2,workingCapacityLimit);
        if (outputCapacityExceeded)
          outputCapacity=input.outputCapacity=std::min(input.outputCapacity*2,input.outputCapacityLimit);
        waveSize=kernel_->suggestedWaveSize(input.arenaCapacity,input.outputCapacity);
        continue;
      }
      for (const auto &output : outputs) {
        result.operations+=output.result.operations;
        if (output.result.error!=vm::Error::None) {
          if (profile) std::fprintf(stderr,"CUDA_VM_LANE_FALLBACK lane=%zu error=%u arena=%u output=%u\n",
              result.values.size(),static_cast<unsigned>(output.result.error),input.arenaCapacity,input.outputCapacity);
          if (control.debugger && control.debugger->enabled()) return fallback("debugger enabled");
          if (output.result.error==vm::Error::Cancelled) return BytecodeBatchResult{{},result.operations,true};
          result.fallbackLanes.push_back(result.values.size());
          result.values.emplace_back();
          continue;
        }
        // Output buffers are immutable while decoding. Memory's accessors do
        // not mutate them; ownership is transferred to normal visual values.
        vm::Memory decoded; decoded.bytes=const_cast<unsigned char *>(output.arena.data());
        decoded.capacity=decoded.used=static_cast<std::uint32_t>(output.arena.size());
        ProgramValueCodec reader(program,decoded); reader.setStrings(encoded->strings());
        reader.setSnapshotDecoder([&,replay=replay_](vm::Value value) {
          if (decoded.length(value)!=5) throw std::runtime_error("Invalid CUDA program snapshot.");
          const auto *fields=decoded.items(value);
          if (fields[0].kind==vm::Kind::Opaque && fields[0].handle && decoded.length(fields[0])==sizeof(std::uint32_t)) {
            std::uint32_t index=0;
            CopyNative(decoded,&index,fields[0],sizeof(index));
            if (index>=importedSnapshots.size()) throw std::runtime_error("Invalid imported CUDA snapshot token.");
            return importedSnapshots[index];
          }
          auto base=baseline;
          std::uint32_t contextId=0;
          std::uint32_t restartHorizon=0;
          std::vector<std::pair<std::uint64_t,std::uint32_t>> horizonChanges;
          if (fields[0].kind==vm::Kind::List && fields[0].handle && decoded.length(fields[0])>=4) {
            const auto id=decoded.items(fields[0])[3];
            if (id.kind!=vm::Kind::Number || id.x<0 || id.x>importedSnapshots.size() || std::floor(id.x)!=id.x)
              throw std::runtime_error("Invalid CUDA snapshot restore context.");
            contextId=static_cast<std::uint32_t>(id.x);
            if (contextId) base=importedSnapshots[contextId-1];
            if (decoded.length(fields[0])>=5 && decoded.items(fields[0])[4].kind!=vm::Kind::None) {
              const auto log=decoded.items(fields[0])[4];
              if (log.kind!=vm::Kind::List || !log.handle) throw std::runtime_error("Invalid CUDA horizon history.");
              for (std::uint32_t i=0;i<decoded.length(log);++i) {
                const auto change=decoded.items(log)[i];
                if (change.kind!=vm::Kind::Range || !std::isfinite(change.x) || !std::isfinite(change.y) ||
                    change.x<base->state.timeMs || change.y<control.tickMs || change.y<change.x ||
                    change.y>2147481040.0 || std::fmod(change.x,control.tickMs) || std::fmod(change.y,control.tickMs) ||
                    (!horizonChanges.empty() && change.x<=horizonChanges.back().first))
                  throw std::runtime_error("Invalid CUDA horizon history entry.");
                if (!std::isfinite(change.z) || (change.z && (i || change.x!=base->state.timeMs ||
                    change.z<control.tickMs || change.z<change.x || change.z>2147481040.0 || std::fmod(change.z,control.tickMs))))
                  throw std::runtime_error("Invalid CUDA restart history entry.");
                if (change.z) restartHorizon=static_cast<std::uint32_t>(change.z);
                horizonChanges.emplace_back(static_cast<std::uint64_t>(change.x),static_cast<std::uint32_t>(change.y));
              }
            }
          }
          const auto replayBase=horizonOrigins[contextId] ? horizonOrigins[contextId] : base;
          const auto *origin=dynamic_cast<const VisualPhysicsSnapshot *>(replayBase->native.get());
          if (!origin) throw std::runtime_error("Invalid CUDA snapshot native origin.");
          forevervalidator::simulation::CudaCandidateState packed;
          if (fields[0].kind==vm::Kind::List && fields[0].handle &&
              (decoded.length(fields[0])>=2 && decoded.length(fields[0])<=6)) {
            const auto *parts=decoded.items(fields[0]);
            CopyNative(decoded,&packed,parts[0],sizeof(packed));
            OverlayNative(decoded,packed,parts[1],decoded.length(fields[0])>=3 ? parts[2] : vm::Value{});
          } else CopyNative(decoded,&packed,fields[0],sizeof(packed));
          const auto state=std::get<VisualState>(reader.decode(fields[2]).data);
          const auto previous=std::get<VisualState>(reader.decode(fields[3]).data);
          auto inputs=std::make_shared<const VisualInputs>(std::get<VisualInputs>(reader.decode(fields[1]).data));
          const auto importStarted=profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
          auto native=Require(PhysicsSandboxCudaExecutionAccess::ImportState(origin->state,packed,state,*inputs),"Importing CUDA block snapshot");
          if (profile) { importTime+=std::chrono::steady_clock::now()-importStarted; ++nativeSnapshots; }
          if (state.timeMs<base->state.timeMs || (state.timeMs-base->state.timeMs)%control.tickMs)
            throw std::runtime_error("Invalid CUDA snapshot relative time.");
          const auto ticks=static_cast<std::uint32_t>((state.timeMs-base->state.timeMs)/control.tickMs);
          auto history=base->history;
          if (restartHorizon) {
            auto initial=base->state;
            SetVisualStateHorizonMs(initial,restartHorizon);
            history=std::make_shared<VisualHistoryNode>(VisualHistoryNode{initial,{},1});
          }
          if (fields[0].kind==vm::Kind::List && decoded.length(fields[0])==6) {
            const auto frames=decoded.items(fields[0])[5];
            if (frames.kind!=vm::Kind::List || !frames.handle || !std::isfinite(frames.x) || frames.x<0 ||
                std::floor(frames.x)!=frames.x || frames.x>decoded.length(frames))
              throw std::runtime_error("Invalid CUDA sampled history.");
            const auto count=frames.x ? static_cast<std::uint32_t>(frames.x) : decoded.length(frames);
            if (count!=history->size+ticks)
              throw std::runtime_error("Invalid CUDA sampled history.");
            if (ticks) {
              auto values=std::make_shared<VisualList>();
              values->reserve(count);
              for (std::uint32_t i=0;i<count;++i) values->push_back(reader.decode(decoded.items(frames)[i]));
              const auto historical=std::get<VisualState>(values->back().data);
              auto span=DeferredVisualHistory([values,prefix=history->size] {
                std::vector<VisualState> states; states.reserve(values->size()-prefix);
                for (std::size_t i=prefix;i<values->size();++i) states.push_back(std::get<VisualState>((*values)[i].data));
                return states;
              });
              history=std::make_shared<VisualHistoryNode>(VisualHistoryNode{historical,history,history->size+ticks,std::move(span)});
            }
          } else if (ticks) {
            auto span=DeferredVisualHistory([replay,replayBase,inputs,ticks,horizonChanges,initialHorizon=base->horizonMs] {
              auto states=replay(*replayBase,*inputs,ticks);
              std::size_t change=0;
              auto horizon=initialHorizon;
              for (auto &frame : states) {
                while (change<horizonChanges.size() && horizonChanges[change].first<frame.timeMs)
                  horizon=horizonChanges[change++].second;
                SetVisualStateHorizonMs(frame,horizon);
              }
              return states;
            });
            auto historical=state;
            SetVisualStateHorizonMs(historical,base->horizonMs);
            for (const auto &change : horizonChanges) if (change.first<state.timeMs) SetVisualStateHorizonMs(historical,change.second);
            history=std::make_shared<VisualHistoryNode>(VisualHistoryNode{historical,history,history->size+ticks,std::move(span)});
          }
          return std::make_shared<const VisualSnapshot>(VisualSnapshot{std::make_shared<VisualPhysicsSnapshot>(std::move(native)),
            state,previous,inputs,std::move(history),static_cast<std::uint32_t>(VisualStateHorizonMs(state)),static_cast<std::uint64_t>(fields[4].x)});
        });
        result.values.push_back(reader.decode(output.result.value));
      }
      first=last;
    }
    if (control.executionModeChanged) control.executionModeChanged("CUDA block-program kernel");
    if (profile) std::fprintf(stderr,"CUDA_VM_HOST_PROFILE lanes=%zu nativeSnapshots=%zu importMilliseconds=%.6f totalMilliseconds=%.6f\n",
        arguments.size(),nativeSnapshots,std::chrono::duration<double,std::milli>(importTime).count(),
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
    return result;
  } catch (const BytecodeNeedsInterpreter &) {
    if (control.executionModeChanged)
      control.executionModeChanged("CUDA physics with CPU block control: values require the source interpreter");
    return {};
  }
private:
  PhysicsSandbox &sandbox_;
  ProgramHistoryReplay replay_;
  std::shared_ptr<CudaProgramPrefixCache> prefixes_;
  std::unique_ptr<CudaProgramKernel> kernel_;
  struct Capacities {
    std::uint32_t arena=128u*1024u,output=128u*1024u;
    std::array<std::uint32_t,6> importedArenas{},importedOutputs{};
  };
  std::array<Capacities,2> capacities_;
};
}

struct CudaProgramPhysicsCursor::Impl {
  ProgramBytecode program;
  CudaProgramKernel kernel;
  PhysicsSandboxState origin;
  VisualInputs inputs;
  VisualState current,previous;
  std::optional<PhysicsSandboxState> saved;
  forevervalidator::simulation::CudaCandidateState packed,extension;
  struct Frame {
    VisualState current,previous;
    forevervalidator::simulation::CudaCandidateState packed;
  };
  std::vector<Frame> predicted;
  std::uint32_t position=0,remaining=0,window=1,extensionVersion=0;
  std::uint32_t maximumWindow=CudaPhysicsCursorMaximumLookahead;
  std::uint64_t windows=0,predictions=0,consumed=0,resets=0,transferred=0;
  double contextMilliseconds=0,stagingMilliseconds=0,startMilliseconds=0;
  std::uint64_t inputUpdates=0,inputRewinds=0;
  double inputMilliseconds=0;
  double predictionMilliseconds=0,decodeMilliseconds=0,captureMilliseconds=0;
  bool profile=std::getenv("FOREVERTAS_CUDA_VM_PROFILE")!=nullptr;
  vm::Error forecastError=vm::Error::None;

  explicit Impl(PhysicsSandbox &sandbox)
      : origin(Require(sandbox.CaptureState(),"Capturing CUDA source origin")) {
    if (const auto *value=std::getenv("FOREVERTAS_CUDA_SOURCE_LOOKAHEAD")) {
      std::uint32_t limit=0;
      const auto parsed=std::from_chars(value,value+std::strlen(value),limit);
      if (parsed.ec==std::errc{} && *parsed.ptr=='\0' && limit)
        maximumWindow=std::min(limit,CudaPhysicsCursorMaximumLookahead);
    }
    reset(sandbox,origin);
  }
  ~Impl() {
    if (profile) std::fprintf(stderr,"CUDA_SOURCE_PROFILE windows=%llu predicted=%llu consumed=%llu resets=%llu bytes=%llu lookahead=%u\n",
        static_cast<unsigned long long>(windows),static_cast<unsigned long long>(predictions),
        static_cast<unsigned long long>(consumed),static_cast<unsigned long long>(resets),
        static_cast<unsigned long long>(transferred),maximumWindow);
    if (profile) std::fprintf(stderr,"CUDA_SOURCE_RESET_PROFILE contextMilliseconds=%.6f stagingMilliseconds=%.6f startMilliseconds=%.6f\n",
        contextMilliseconds,stagingMilliseconds,startMilliseconds);
    if (profile) std::fprintf(stderr,"CUDA_SOURCE_INPUT_PROFILE updates=%llu rewinds=%llu milliseconds=%.6f\n",
        static_cast<unsigned long long>(inputUpdates),static_cast<unsigned long long>(inputRewinds),inputMilliseconds);
    if (profile) std::fprintf(stderr,"CUDA_SOURCE_ADVANCE_PROFILE predictionMilliseconds=%.6f decodeMilliseconds=%.6f captureMilliseconds=%.6f\n",
        predictionMilliseconds,decodeMilliseconds,captureMilliseconds);
  }
  void reset(PhysicsSandbox &sandbox,PhysicsSandboxState state) {
    const auto started=profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ++resets;
    origin=std::move(state);
    inputs=Require(sandbox.ReadInputs(),"Reading CUDA source inputs");
    current=origin.View(); previous=current; saved=origin;
    CheckStateProperties();
    const auto context=Require(PhysicsSandboxCudaExecutionAccess::Capture(sandbox),"Preparing CUDA source cursor");
    const auto captured=profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    packed=extension=context.physics.initialState;
    predicted.clear(); position=0; window=1; extensionVersion=0; forecastError=vm::Error::None;
    remaining=static_cast<std::uint32_t>(context.ticks.size());
    auto input=encode(context);
    const auto staged=profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    kernel.startPhysicsCursor(input);
    if (profile) {
      contextMilliseconds+=std::chrono::duration<double,std::milli>(captured-started).count();
      stagingMilliseconds+=std::chrono::duration<double,std::milli>(staged-captured).count();
      startMilliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-staged).count();
    }
  }

  CudaProgramInput encode(const PhysicsSandboxCudaExecutionContext &context) {
    CudaProgramInput input;
    input.program=&program; input.context=&context;
    if (inputs.size()>(UINT32_MAX-32768u)/(2u*sizeof(vm::Value))) throw BytecodeNeedsInterpreter{};
    const auto capacity=2u*inputs.size()*sizeof(vm::Value)+32768u;
    input.arenaCapacity=static_cast<std::uint32_t>(capacity);
    input.arena.resize(capacity);
    vm::Memory memory; memory.bytes=input.arena.data(); memory.capacity=input.arenaCapacity;
    ProgramValueCodec codec(program,memory);
    input.globals=memory.allocate(vm::Kind::List,0);
    input.inputs=codec.encode(VisualValue(inputs));
    input.initialView=codec.encode(VisualValue(current));
    input.previousView=codec.encode(VisualValue(previous));
    if (memory.error!=vm::Error::None) throw BytecodeNeedsInterpreter{};
    input.arena.resize(memory.used);
    // Cursor steps export directly; their working arena only holds staged values.
    input.arenaCapacity=memory.used;
    input.outputCapacity=8u*1024u*1024u;
    input.arguments.emplace_back(); input.seeds.push_back(0);
    return input;
  }

  void replaceInputs(PhysicsSandbox &sandbox) {
    const auto started=profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const bool preserveState=position==predicted.size() && forecastError==vm::Error::None;
    inputs=Require(sandbox.ReadInputs(),"Reading CUDA source replacement inputs");
    const auto context=Require(PhysicsSandboxCudaExecutionAccess::Capture(sandbox),"Preparing CUDA source replacement inputs");
    auto input=encode(context);
    kernel.updatePhysicsCursor(input,preserveState);
    saved.reset();
    packed=extension=context.physics.initialState;
    predicted.clear(); position=0; window=1; extensionVersion=0; forecastError=vm::Error::None;
    remaining=static_cast<std::uint32_t>(context.ticks.size());
    ++inputUpdates; inputRewinds+=!preserveState;
    if (profile) inputMilliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  }

  VisualState decode(const vm::Value *properties) const {
    alignas(vm::Value) std::array<unsigned char,8192> arena{};
    vm::Memory memory; memory.bytes=arena.data(); memory.capacity=static_cast<std::uint32_t>(arena.size());
    const auto value=memory.allocate(vm::Kind::State,CudaProgramStatePropertyCount);
    for (std::uint32_t i=0;i<CudaProgramStatePropertyCount;++i) {
      auto property=properties[i];
      if (i>=30 && i<=33) {
        const double components[]={property.x,property.y,property.z,property.w};
        property=memory.allocate(vm::Kind::List,4);
        for (std::uint32_t j=0;j<4;++j)
          memory.items(property)[j]=i==31 ? vm::number(components[j]) : vm::boolean(components[j]!=0);
      } else if (vm::reference(property)) throw std::logic_error("Unexpected CUDA source state reference.");
      memory.items(value)[i]=property;
    }
    if (memory.error!=vm::Error::None) throw std::logic_error("CUDA source state exceeds its fixed representation.");
    ProgramValueCodec codec(program,memory);
    return std::get<VisualState>(codec.decode(value).data);
  }
};

CudaProgramPhysicsCursor::CudaProgramPhysicsCursor(PhysicsSandbox &sandbox) : impl_(std::make_unique<Impl>(sandbox)) {}
void CudaProgramPrefixCache::observe(const CudaProgramPhysicsCursor &cursor) try {
  if (!impl_->active) return;
  const auto &source=*cursor.impl_;
  const bool checkpoint=needsNativeState(source.current);
  impl_->active->last=source.current;
  if (!checkpoint || source.previous.timeMs+10!=source.current.timeMs) return;
  // The cursor retains the last consumed frame separately from its forecast.
  impl_->record(source.current,source.previous,source.inputs,[&](auto &physics) {
    physics=source.packed;
    return true;
  });
} catch (const std::bad_alloc &) {
  impl_->entries.clear(); impl_->active.reset();
}
CudaProgramPhysicsCursor::~CudaProgramPhysicsCursor()=default;
void CudaProgramPhysicsCursor::reset(PhysicsSandbox &sandbox) {
  impl_->reset(sandbox,Require(sandbox.CaptureState(),"Capturing CUDA source origin"));
}
void CudaProgramPhysicsCursor::replaceInputs(PhysicsSandbox &sandbox) { impl_->replaceInputs(sandbox); }
const VisualState &CudaProgramPhysicsCursor::read() const { return impl_->current; }
std::optional<VisualAdvance> CudaProgramPhysicsCursor::advance(std::uint32_t ticks) {
  auto &p=*impl_;
  if (ticks>p.remaining) return {};
  for (std::uint32_t tick=0;tick<ticks;++tick) {
    if (p.position==p.predicted.size()) {
      if (p.forecastError!=vm::Error::None) return {};
      const auto started=p.profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      auto batch=p.kernel.predictPhysicsCursor(std::min(p.window,p.remaining));
      const auto predicted=p.profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      if (p.profile) p.predictionMilliseconds+=std::chrono::duration<double,std::milli>(predicted-started).count();
      ++p.windows; p.predictions+=batch.states.size(); p.transferred+=batch.arena.size();
      p.forecastError=batch.error;
      p.predicted.clear(); p.position=0;
      if (batch.states.empty()) return {};
      vm::Memory memory; memory.bytes=batch.arena.data();
      memory.capacity=memory.used=static_cast<std::uint32_t>(batch.arena.size());
      if (batch.native.kind!=vm::Kind::List || memory.length(batch.native)<batch.states.size())
        throw std::runtime_error("Invalid CUDA source prediction.");
      for (std::size_t i=0;i<batch.states.size();++i) {
        const auto record=memory.items(batch.native)[i];
        if (record.kind!=vm::Kind::List || memory.length(record)!=4)
          throw std::runtime_error("Invalid CUDA source native record.");
        const auto *fields=memory.items(record);
        if (fields[0].kind!=vm::Kind::Number || !std::isfinite(fields[0].x) || fields[0].x<p.extensionVersion ||
            fields[0].x>UINT32_MAX || fields[0].x!=static_cast<std::uint32_t>(fields[0].x))
          throw std::runtime_error("Invalid CUDA source native generation.");
        const auto version=static_cast<std::uint32_t>(fields[0].x);
        if (version!=p.extensionVersion) {
          CopyNative(memory,&p.extension,fields[1],sizeof(p.extension));
          p.extensionVersion=version;
        } else if (fields[1].kind!=vm::Kind::None)
          throw std::runtime_error("Unexpected CUDA source native generation.");
        auto packed=p.extension;
        OverlayNative(memory,packed,fields[2],fields[3]);
        p.predicted.push_back({p.decode(batch.states[i].current),p.decode(batch.states[i].previous),std::move(packed)});
      }
      p.window=std::min(p.window*2,p.maximumWindow);
      if (p.profile) p.decodeMilliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-predicted).count();
    }
    const auto &frame=p.predicted[p.position++];
    p.current=frame.current; p.previous=frame.previous; p.packed=frame.packed;
    p.saved.reset(); --p.remaining; ++p.consumed;
  }
  return VisualAdvance{p.current,p.previous,{}};
}
PhysicsSandboxState CudaProgramPhysicsCursor::capture() const {
  auto &p=*impl_;
  if (!p.saved) {
    const auto started=p.profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    p.saved=Require(PhysicsSandboxCudaExecutionAccess::ImportState(p.origin,p.packed,p.current,p.inputs),
        "Capturing CUDA source cursor");
    if (p.profile) p.captureMilliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  }
  return *p.saved;
}

std::shared_ptr<VisualBatchExecutor> CreateCudaProgramExecutor(PhysicsSandbox &sandbox,
    ProgramHistoryReplay replay,std::shared_ptr<CudaProgramPrefixCache> prefixes) {
  return std::make_shared<CudaExecutor>(sandbox,std::move(replay),std::move(prefixes));
}
}
