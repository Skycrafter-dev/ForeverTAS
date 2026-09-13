#include "blocks/program_value_codec.h"
#include "blocks/visual_debugger.h"

#include <algorithm>
#include <cmath>

namespace forevertas::blocks {
namespace {
using namespace vm;
constexpr double MaximumInteger=9007199254740991.0;
struct CallbackFailure { std::exception_ptr exception; };

// Abstract execution uses the same VM, but varying arguments/random draws are
// intervals. An unknown branch, possible error or physics effect abandons the
// proof. Only a proven identical, effect-free return may be shared by lanes.
class UniformPhysics {
public:
  static constexpr bool uniformExecution=true;
  const VisualSnapshot &baseline;
  const VisualRuntimeControl &control;
  Value inputs,candidates=number(0);
  bool uniformRandom=false;
  ProgramValueCodec *codec=nullptr;
  UniformPhysics(const VisualSnapshot &baseline,const VisualRuntimeControl &control) : baseline(baseline),control(control) {}
  bool cancelled() {
    try {
      if (control.progress) control.progress();
      return (control.stopRequested && control.stopRequested()) || interpreterRequested();
    } catch (...) { throw CallbackFailure{std::current_exception()}; }
  }
  bool interpreterRequested() const { return control.debugger && control.debugger->enabled(); }
  static bool numeric(Value value) { return value.kind==Kind::Number || value.kind==Kind::NumberInterval; }
  static double low(Value value) { return value.x; }
  static double high(Value value) { return value.kind==Kind::NumberInterval ? value.y : value.x; }
  static bool integral(Value value) { return value.kind==Kind::NumberInterval ? value.z!=0 : value.x==std::floor(value.x); }
  static Value interval(double lo,double hi,bool integers) {
    return lo==hi ? number(lo) : Value{Kind::NumberInterval,0,lo,hi,integers ? 1.0 : 0.0};
  }
  bool uniformBuiltin(Op op,Machine<UniformPhysics> &machine,Value *a,std::uint32_t count,Value &result) {
    const auto unsupported=[&] { machine.error=Error::Unsupported; return true; };
    if (op==Op::MapPrepare || op==Op::MapEnter || op==Op::MapCollect || op==Op::MapResult || op==Op::MapNeedsRestore) return unsupported();
    if (op==Op::Count || op==Op::AddCount) {
      const auto amount=op==Op::Count ? number(1) : a[0];
      if (!numeric(amount) || !integral(amount) || low(amount)<0 || high(amount)>MaximumInteger ||
          high(candidates)>MaximumInteger-high(amount)) return unsupported();
      candidates=interval(low(candidates)+low(amount),high(candidates)+high(amount),true);
      return true;
    }
    if (op==Op::Iterations) { result=candidates; return true; }
    if (op==Op::Seed) {
      if (!numeric(a[0]) || !integral(a[0]) || low(a[0])<0 || high(a[0])>MaximumInteger) return unsupported();
      uniformRandom=a[0].kind==Kind::Number;
      if (uniformRandom) machine.random.seed(static_cast<std::uint64_t>(a[0].x));
      return true;
    }
    if ((op==Op::Random || op==Op::RandomInteger) && !uniformRandom) {
      // Integer draws have exact closed bounds. Floating arithmetic can
      // overshoot ideal bounds after cancellation, so do not prove from them.
      if (op==Op::Random) return unsupported();
      if (a[0].kind!=Kind::Number || a[1].kind!=Kind::Number || a[0].x>a[1].x ||
          !std::isfinite(a[1].x-a[0].x)) return unsupported();
      if (op==Op::RandomInteger && (!integral(a[0]) || !integral(a[1]) || a[0].x<-MaximumInteger || a[1].x>MaximumInteger))
        return unsupported();
      result=interval(a[0].x==0 ? 0 : a[0].x,a[1].x==0 ? 0 : a[1].x,true);
      return true;
    }
    if ((op==Op::Min || op==Op::Max) && numeric(a[0]) && numeric(a[1])) {
      if (a[0].kind==Kind::Number && a[1].kind==Kind::Number) return false;
      if (high(a[0])<low(a[1])) { result=op==Op::Min ? a[0] : a[1]; return true; }
      if (high(a[1])<low(a[0])) { result=op==Op::Min ? a[1] : a[0]; return true; }
      const auto lo=op==Op::Min ? minimum(low(a[0]),low(a[1])) : maximum(low(a[0]),low(a[1]));
      const auto hi=op==Op::Min ? minimum(high(a[0]),high(a[1])) : maximum(high(a[0]),high(a[1]));
      if (lo==0 && hi==0) return unsupported();
      result=interval(lo,hi,integral(a[0]) && integral(a[1]));
      return true;
    }
    // Unknown values may be stored locally, but must not reach ordinary
    // builtins (including list construction/equality) as concrete numbers.
    for (std::uint32_t i=0;i<count;++i) if (a[i].kind==Kind::NumberInterval) return unsupported();
    return false;
  }
  Value apply(Op op,Machine<UniformPhysics> &machine,Value *a,std::uint32_t option) {
    switch (op) {
    case Op::Inputs: return machine.memory.copy(inputs);
    case Op::UseInputs:
      if (!machine.array(a[0],Kind::Inputs) || !machine.equal(a[0],inputs)) machine.error=Error::Unsupported;
      return {};
    case Op::Time: return number(static_cast<double>(baseline.state.timeMs));
    case Op::Horizon: return number(baseline.horizonMs);
    case Op::AllTime: return {Kind::Range,0,0,static_cast<double>(baseline.horizonMs)};
    case Op::AtTime: {
      const auto time=machine.time(machine.numeric(a[0])); return {Kind::Range,0,static_cast<double>(time),static_cast<double>(time)};
    }
    case Op::ReadCurrent: return codec->encode(ReadVisualStateProperty(baseline.state,option));
    case Op::ReadPrevious: return codec->encode(ReadVisualStateProperty(baseline.previous,option));
    case Op::CurrentState: return codec->encode(VisualValue(baseline.state));
    case Op::PreviousState: return codec->encode(VisualValue(baseline.previous));
    default: machine.error=Error::Unsupported; return {};
    }
  }
};
}

std::optional<BytecodeBatchResult> TryUniformProgram(const ProgramBytecode &program,
    const std::shared_ptr<const VisualSnapshot> &baseline,const std::map<std::string,VisualValue> &globals,
    const VisualList &arguments,const VisualRuntimeControl &control,HostBytecodeStorage &storage) try {
  if (arguments.size()<2) return {};
  for (const auto &argument : arguments) {
    const auto *value=std::get_if<double>(&argument.data);
    if (!value || !std::isfinite(*value) || *value<0 || *value>MaximumInteger || *value!=std::floor(*value)) return {};
  }
  storage.prepare();
  UniformPhysics physics(*baseline,control);
  Machine<UniformPhysics> machine(program.view(),physics);
  machine.memory.bytes=storage.arena.get(); machine.memory.capacity=storage.capacity;
  machine.stack=storage.stack.data(); machine.stackCapacity=static_cast<std::uint32_t>(storage.stack.size());
  machine.tickMs=control.tickMs; machine.batchSize=control.batchSize;
  machine.operationLimit=8192;
  ProgramValueCodec codec(program,machine.memory); physics.codec=&codec;
  const auto dynamicReads=ResolveDynamicInitialGlobals(program,globals,arguments);
  const auto &initialReads=dynamicReads ? *dynamicReads : program.initialGlobalReads;
  machine.globals=machine.memory.allocate(Kind::List,static_cast<std::uint32_t>(program.symbols.size()));
  if (machine.memory.error!=Error::None) return {};
  for (std::size_t i=0;i<program.symbols.size();++i) {
    auto &slot=machine.memory.items(machine.globals)[i]; slot.kind=Kind::Unset;
    if (initialReads.size()==program.symbols.size() && !initialReads[i]) continue;
    const auto found=globals.find(program.symbols[i]);
    if (found!=globals.end()) slot=codec.encode(found->second);
  }
  physics.inputs=codec.encode(VisualValue(*baseline->inputs));
  if (codec.hasNativeObjects()) return {};
  const auto result=machine.execute(program.entry,UniformPhysics::interval(0,MaximumInteger,true),1);
  if (result.error==Error::Cancelled) return BytecodeBatchResult{{},result.operations,true};
  if (result.error!=Error::None || result.value.kind==Kind::NumberInterval) return {};
  const auto value=codec.decode(result.value);
  return BytecodeBatchResult{std::vector<VisualValue>(arguments.size(),value),result.operations*arguments.size(),false};
} catch (const CallbackFailure &failure) {
  std::rethrow_exception(failure.exception);
} catch (const BytecodeNeedsInterpreter &) {
  return {};
} catch (const std::runtime_error &) {
  // A proof failure never replaces the concrete program's own diagnostic.
  return {};
}
}
