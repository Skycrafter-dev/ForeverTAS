#include "blocks/program_value_codec.h"
#include "blocks/visual_debugger.h"

#include <set>

namespace forevertas::blocks {
namespace {
using namespace vm;
class HostPhysics {
public:
  VisualSimulationHost &host;
  const VisualRuntimeControl &control;
  ProgramValueCodec *codec=nullptr;
  VisualState current,previous;
  std::shared_ptr<const VisualInputs> inputs;
  std::shared_ptr<const VisualHistoryNode> history;
  Value inputValue;
  std::uint32_t horizon;
  std::uint32_t probes=0;
  std::uint32_t pendingTicks=0;
  const VisualSnapshot &restartOrigin;

  HostPhysics(VisualSimulationHost &host,const VisualRuntimeControl &control,const VisualSnapshot &baseline)
      : host(host),control(control),current(baseline.state),previous(baseline.previous),
        inputs(baseline.inputs),history(baseline.history),horizon(baseline.horizonMs),
        restartOrigin(control.restartOrigin ? *control.restartOrigin : baseline) {}
  bool cancelled() {
    if ((++probes & 255u)==0 && control.progress) control.progress();
    return (control.stopRequested && control.stopRequested()) || interpreterRequested();
  }
  bool interpreterRequested() const { return control.debugger && control.debugger->enabled(); }
  void flush(Machine<HostPhysics> &vm) {
    if (!pendingTicks || vm.error!=Error::None) return;
    const auto count=pendingTicks;
    if (cancelled()) { vm.error=interpreterRequested() ? Error::Interpreter : Error::Cancelled; return; }
    const auto advanced=host.advanceMany(count);
    if (!vm.check(advanced.state.timeMs==current.timeMs,Error::Physics)) return;
    previous=advanced.previous; current=advanced.state; pendingTicks=0;
    history=std::make_shared<VisualHistoryNode>(VisualHistoryNode{current,history,history->size+count,advanced.history});
  }
  Value apply(Op op,Machine<HostPhysics> &vm,Value *a,std::uint32_t option) {
    // Only the logical clock can be observed without executing pending ticks.
    // Every physical-state read, input replacement and snapshot is a barrier.
    if (op==Op::CurrentState || op==Op::PreviousState || op==Op::ReadPrevious || op==Op::Snapshot || op==Op::UseInputs || op==Op::Restore || op==Op::SetHorizon || op==Op::History || op==Op::Restart ||
        (op==Op::ReadCurrent && option>2)) flush(vm);
    if (vm.error!=Error::None) return {};
    switch (op) {
    case Op::Time: return number(static_cast<double>(current.timeMs));
    case Op::Horizon: return number(horizon);
    case Op::SetHorizon: {
      const auto next=vm.time(vm.numeric(a[0]));
      if (!vm.check(next>=vm.tickMs && next>=current.timeMs,Error::Horizon)) return {};
      if (vm.error!=Error::None) return {};
      current=host.setHorizon(next); horizon=next;
      return {};
    }
    case Op::AllTime: return {Kind::Range,0,0,static_cast<double>(horizon)};
    case Op::AtTime: { const auto at=vm.time(vm.numeric(a[0])); return {Kind::Range,0,static_cast<double>(at),static_cast<double>(at)}; }
    case Op::ReadCurrent: return codec->encode(ReadVisualStateProperty(current,option));
    case Op::ReadPrevious: return codec->encode(ReadVisualStateProperty(previous,option));
    case Op::CurrentState: return codec->encode(VisualValue(current));
    case Op::PreviousState: return codec->encode(VisualValue(previous));
    case Op::History: {
      if (cancelled()) { vm.error=interpreterRequested() ? Error::Interpreter : Error::Cancelled; return {}; }
      const auto states=VisualHistoryStates(history);
      auto result=vm.memory.allocate(Kind::List,static_cast<std::uint32_t>(states.size()));
      if (vm.memory.error!=Error::None) return {};
      for (std::size_t i=0;i<states.size();++i) {
        if (cancelled()) { vm.error=interpreterRequested() ? Error::Interpreter : Error::Cancelled; return result; }
        vm.memory.items(result)[i]=codec->encode(VisualValue(states[i]));
        if (vm.memory.error!=Error::None) break;
      }
      return result;
    }
    case Op::Inputs: return vm.memory.copy(inputValue);
    case Op::Snapshot: {
      auto value=std::make_shared<VisualSnapshot>(VisualSnapshot{host.capture(),current,previous,inputs,history,horizon,vm.candidates});
      return codec->encodeSnapshot(std::move(value),inputValue);
    }
    case Op::SnapshotState: return codec->encode(VisualValue(codec->snapshot(a[0])->state));
    case Op::SnapshotInputs:
      if (!vm.array(a[0],Kind::Snapshot)) return {};
      return vm.memory.copy(vm.memory.items(a[0])[1]);
    case Op::Restart: {
      if (cancelled()) { vm.error=interpreterRequested() ? Error::Interpreter : Error::Cancelled; return {}; }
      if (!restartOrigin.native || !restartOrigin.inputs || !restartOrigin.history)
        throw std::runtime_error("Invalid snapshot.");
      host.setHorizon(std::max(restartOrigin.horizonMs,static_cast<std::uint32_t>(host.read().timeMs)));
      current=host.restore(*restartOrigin.native);
      host.replaceInputs(*restartOrigin.inputs);
      current=host.setHorizon(restartOrigin.horizonMs);
      previous=restartOrigin.previous;
      host.replaceInputs(*inputs);
      current=host.setHorizon(horizon);
      history=std::make_shared<VisualHistoryNode>(VisualHistoryNode{current,{},1});
      return {};
    }
    case Op::Restore: {
      if (!vm.array(a[0],Kind::Snapshot)) return {};
      const auto saved=codec->snapshot(a[0]);
      if (!saved || !saved->native || !saved->inputs || !saved->history)
        throw std::runtime_error("Invalid snapshot.");
      host.setHorizon(std::max(saved->horizonMs,static_cast<std::uint32_t>(host.read().timeMs)));
      current=host.restore(*saved->native);
      host.replaceInputs(*saved->inputs);
      inputs=saved->inputs; horizon=saved->horizonMs;
      current=host.setHorizon(horizon);
      previous=saved->previous; history=saved->history;
      vm.memory.assign(inputValue,vm.memory.copy(vm.memory.items(a[0])[1]));
      return {};
    }
    case Op::Step:
      if (cancelled()) { vm.error=interpreterRequested() ? Error::Interpreter : Error::Cancelled; return {}; }
      if (!vm.check(current.timeMs<horizon,Error::Horizon)) return {};
      if (!vm.check(history->size+pendingTicks<control.collectionLimit,Error::Capacity)) return {};
      current.timeMs+=vm.tickMs; ++current.tick; ++pendingTicks;
      if (pendingTicks>=128) flush(vm);
      return {};
    case Op::UseInputs: {
      if (!vm.array(a[0],Kind::Inputs)) return {};
      const auto size=vm.memory.length(a[0]); const auto *events=vm.memory.items(a[0]);
      std::set<std::pair<std::int32_t,std::uint32_t>> keys;
      std::size_t prefix=0;
      for (std::uint32_t i=0;i<size;++i) {
        if (i && !vm.check(events[i-1].x<=events[i].x,Error::Unsorted)) return {};
        if (!vm.check(keys.emplace(static_cast<std::int32_t>(events[i].x),static_cast<std::uint32_t>(events[i].z)).second,Error::DuplicateInput)) return {};
        if (events[i].x<=current.timeMs) {
          if (!vm.check(prefix<vm.memory.length(inputValue) && vm.equal(events[i],vm.memory.items(inputValue)[prefix]),Error::PastInputs)) return {};
          ++prefix;
        }
      }
      if (prefix<vm.memory.length(inputValue) && !vm.check(vm.memory.items(inputValue)[prefix].x>current.timeMs,Error::PastInputs)) return {};
      auto decoded=std::get<VisualInputs>(codec->decode(a[0]).data);
      auto shared=std::make_shared<const VisualInputs>(std::move(decoded));
      host.replaceInputs(*shared); inputs=std::move(shared);
      vm.memory.assign(inputValue,vm.memory.copy(a[0])); return {};
    }
    default: vm.error=Error::Unsupported; return {};
    }
  }
};
}

void HostBytecodeStorage::prepare() {
  if (!arena) arena.reset(new unsigned char[capacity]);
  if (stack.empty()) stack.resize(1024);
}

BytecodeExecution ExecuteHostBytecode(const ProgramBytecode &program,VisualSimulationHost &host,
    const std::shared_ptr<const VisualSnapshot> &baseline,const std::map<std::string,VisualValue> &globals,
    const VisualValue &argument,std::uint64_t seed,const VisualRuntimeControl &control,HostBytecodeStorage &storage) try {
  storage.prepare();
  HostPhysics physics(host,control,*baseline);
  vm::Machine<HostPhysics> machine(program.view(),physics);
  machine.memory.bytes=storage.arena.get(); machine.memory.capacity=storage.capacity;
  machine.stack=storage.stack.data(); machine.stackCapacity=static_cast<std::uint32_t>(storage.stack.size());
  machine.tickMs=control.tickMs; machine.workers=1; machine.batchSize=control.batchSize;
  ProgramValueCodec codec(program,machine.memory); physics.codec=&codec;
  try {
  machine.globals=machine.memory.allocate(vm::Kind::List,static_cast<std::uint32_t>(program.symbols.size()));
  if (machine.memory.error==vm::Error::Capacity) return {{},0,false,true};
  for (std::size_t i=0;i<program.symbols.size();++i) {
    auto &slot=machine.memory.items(machine.globals)[i]; slot.kind=vm::Kind::Unset;
    if (program.initialGlobalReads.size()==program.symbols.size() && !program.initialGlobalReads[i]) continue;
    const auto value=globals.find(program.symbols[i]);
    if (value!=globals.end()) slot=codec.encode(value->second);
  }
  physics.inputValue=codec.encode(VisualValue(*baseline->inputs));
  vm::Result executed;
  try {
    executed=machine.execute(program.entry,codec.encode(argument),seed);
    if (executed.error==vm::Error::None) { physics.flush(machine); executed.error=machine.error; }
  }
  catch (const std::exception &error) {
    throw std::runtime_error("Block #"+std::to_string(machine.source)+": "+error.what());
  }
  if (executed.error==vm::Error::Cancelled) return {{},executed.operations,true,false};
  // The source evaluator owns observable diagnostics and catch behavior too.
  // Replay only a failing isolated branch rather than approximating its error.
  if (executed.error!=vm::Error::None)
    return {{},executed.operations,false,true};
  return {codec.decode(executed.value),executed.operations,false,false};
  } catch (const std::runtime_error &) {
    // Encoding the starting globals/arguments can exhaust the arena too.
    // The accelerator's storage budget is not a source-language limit.
    if (machine.memory.error==vm::Error::Capacity) return {{},machine.operations,false,true};
    throw;
  }
} catch (const BytecodeNeedsInterpreter &) {
  return {{},0,false,true};
}
} // namespace forevertas::blocks
