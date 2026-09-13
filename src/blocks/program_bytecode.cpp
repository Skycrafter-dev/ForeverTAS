#include "blocks/program_bytecode.h"
#include "blocks/block_value.h"
#include "blocks/visual_catalog.h"
#include "blocks/visual_runtime.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace forevertas::blocks {
namespace {
using vm::Op;
struct Builtin { Op op; std::vector<std::string> inputs; };
const std::map<std::string,Builtin> &Builtins() {
  static const std::map<std::string,Builtin> values{
    {"data/has-value",{Op::HasValue,{"value"}}}, {"data/list",{Op::List,{}}},
    {"data/append",{Op::Append,{"list","value"}}}, {"data/length",{Op::Length,{"list"}}},
    {"data/item",{Op::Item,{"list","index"}}}, {"data/contains",{Op::Contains,{"list","value"}}},
    {"data/replace-item",{Op::ReplaceItem,{"list","index","value"}}}, {"data/delete-item",{Op::DeleteItem,{"list","index"}}},
    {"data/numbers",{Op::Numbers,{"from","to","step"}}},
    {"math/add",{Op::Add,{"a","b"}}}, {"math/subtract",{Op::Subtract,{"a","b"}}},
    {"math/multiply",{Op::Multiply,{"a","b"}}}, {"math/divide",{Op::Divide,{"a","b"}}},
    {"math/min",{Op::Min,{"a","b"}}}, {"math/max",{Op::Max,{"a","b"}}},
    {"math/modulo",{Op::Modulo,{"a","b"}}}, {"math/abs",{Op::Abs,{"value"}}},
    {"math/floor",{Op::Floor,{"value"}}}, {"math/ceil",{Op::Ceil,{"value"}}},
    {"math/round",{Op::Round,{"value"}}}, {"math/sqrt",{Op::Sqrt,{"value"}}},
    {"math/sin",{Op::Sin,{"value"}}}, {"math/cos",{Op::Cos,{"value"}}},
    {"math/clamp",{Op::Clamp,{"value","minimum","maximum"}}}, {"math/weighted-blend",{Op::Blend,{"a","b","weight"}}},
    {"math/percent-ratio",{Op::Percent,{"value"}}}, {"math/kmh",{Op::Kmh,{"value"}}},
    {"math/random",{Op::Random,{"a","b"}}}, {"math/random-integer",{Op::RandomInteger,{"a","b"}}},
    {"math/seed",{Op::Seed,{"value"}}}, {"math/number-range",{Op::Range,{"minimum","maximum"}}},
    {"math/integer-range",{Op::IntegerRange,{"minimum","maximum"}}},
    {"math/range-minimum",{Op::RangeMinimum,{"range"}}}, {"math/range-maximum",{Op::RangeMaximum,{"range"}}},
    {"math/magnitude",{Op::Magnitude,{"value"}}}, {"math/normalize",{Op::Normalize,{"value"}}},
    {"math/component",{Op::Component,{"value"}}}, {"math/vector-scale",{Op::Scale,{"value","factor"}}},
    {"math/distance",{Op::Distance,{"a","b"}}}, {"math/dot",{Op::Dot,{"a","b"}}},
    {"math/vector-add",{Op::VectorAdd,{"a","b"}}}, {"math/vector-subtract",{Op::VectorSubtract,{"a","b"}}},
    {"math/rotation-distance",{Op::RotationDistance,{"a","b"}}},
    {"conditions/equal",{Op::Equal,{"a","b"}}}, {"conditions/less",{Op::Less,{"a","b"}}},
    {"conditions/less-equal",{Op::LessEqual,{"a","b"}}}, {"conditions/greater",{Op::Greater,{"a","b"}}},
    {"conditions/greater-equal",{Op::GreaterEqual,{"a","b"}}}, {"conditions/not",{Op::Not,{"value"}}},
    {"conditions/inside",{Op::Inside,{"position","volume"}}},
    {"targets/point",{Op::Vector,{"x","y","z"}}}, {"targets/direction",{Op::Direction,{"x","y","z"}}},
    {"targets/size",{Op::Size,{"x","y","z"}}}, {"targets/rotation",{Op::Rotation,{"yaw","pitch","roll"}}},
    {"targets/box",{Op::Box,{"center","size"}}}, {"targets/prism",{Op::Prism,{"origin","depth","plane","polygon"}}},
    {"targets/polygon-from-points",{Op::Polygon,{"points"}}},
    {"inputs/empty",{Op::EmptyInputs,{}}}, {"inputs/count",{Op::InputCount,{"inputs"}}},
    {"inputs/time",{Op::InputTime,{"inputs","index"}}}, {"inputs/value",{Op::InputValue,{"inputs","index"}}},
    {"inputs/action",{Op::InputAction,{"inputs","index"}}}, {"inputs/remove",{Op::RemoveInput,{"inputs","index"}}},
    {"inputs/sort",{Op::SortInputs,{"inputs"}}}, {"inputs/with-time",{Op::MoveInput,{"inputs","index","time"}}},
    {"inputs/with-value",{Op::ChangeInput,{"inputs","index","value"}}},
    {"inputs/value-at",{Op::HeldInput,{"inputs","time","action"}}},
    {"inputs/set",{Op::SetInput,{"inputs","time","action","value"}}},
    {"runtime/workers",{Op::Workers,{}}}, {"runtime/batch-size",{Op::BatchSize,{}}},
    {"results/count",{Op::Count,{}}}, {"results/add-count",{Op::AddCount,{"amount"}}},
    {"results/iterations",{Op::Iterations,{}}}, {"results/clear",{Op::ClearResult,{}}},
    {"results/has-result",{Op::HasResult,{}}}, {"results/best-score",{Op::ResultScore,{}}},
    {"results/snapshot",{Op::ResultSnapshot,{}}}, {"results/publish",{Op::Publish,{"score"}}},
    {"results/publish-snapshot",{Op::PublishSnapshot,{"snapshot","score"}}},
    {"simulation/time",{Op::Time,{}}}, {"simulation/horizon",{Op::Horizon,{}}},
    {"simulation/tick-duration",{Op::TickDuration,{}}}, {"simulation/read",{Op::Read,{"state"}}},
    {"simulation/state",{Op::CurrentState,{}}}, {"simulation/previous-state",{Op::PreviousState,{}}},
    {"simulation/snapshot",{Op::Snapshot,{}}}, {"simulation/snapshot-state",{Op::SnapshotState,{"snapshot"}}},
    {"simulation/snapshot-inputs",{Op::SnapshotInputs,{"snapshot"}}}, {"simulation/inputs",{Op::Inputs,{}}},
    {"simulation/replace-inputs",{Op::UseInputs,{"inputs"}}},
    {"simulation/restore",{Op::Restore,{"snapshot"}}},
    {"simulation/set-horizon",{Op::SetHorizon,{"time"}}},
    {"simulation/history",{Op::History,{}}},
    {"simulation/restart",{Op::Restart,{}}},
    {"simulation/step",{Op::Step,{}}},
    {"time/range",{Op::TimeRange,{"from","to"}}}, {"time/at",{Op::AtTime,{"time"}}}, {"time/all",{Op::AllTime,{}}}
  };
  return values;
}

class Compiler {
public:
  const VisualProgram &source;
  ProgramBytecode result;
  std::map<std::string,const VisualNode *> definitions;
  std::map<std::string,std::uint32_t> symbols, functions;
  std::vector<const VisualNode *> pending;
  std::map<VisualNodeId,std::uint32_t> handlers;
  struct Loop { std::vector<std::uint32_t> exits, continues; };
  std::vector<Loop> loops;
  VisualNodeId current = 0;

  explicit Compiler(const VisualProgram &source) : source(source) {
    result.strings={"xy","xz","yz"};
    const char *names[]={"unmapped","accelerate","gas","brake","steer","left","right","race-running","finish-line","respawn"};
    for (std::uint32_t i=0;i<10;++i) result.actionNames[i]=text(names[i]);
    for (auto id : source.topLevel) {
      const auto &node=*source.find(id);
      if (node.enabled && node.definitionId=="procedures/define") definitions[field(node,"name")]=&node;
    }
  }
  std::string field(const VisualNode &node,const std::string &key) const {
    const auto it=node.fields.find(key);
    if (it!=node.fields.end()) return it->second;
    for (const auto &value : FindVisualBlock(node.definitionId)->fields) if (value.key==key) return value.defaultValue;
    throw std::runtime_error("Missing field: "+key);
  }
  std::uint32_t text(const std::string &value) {
    const auto it=std::find(result.strings.begin(),result.strings.end(),value);
    if (it!=result.strings.end()) return static_cast<std::uint32_t>(it-result.strings.begin());
    result.strings.push_back(value); return static_cast<std::uint32_t>(result.strings.size()-1);
  }
  std::uint32_t symbol(const std::string &name) {
    const auto it=symbols.find(name); if (it!=symbols.end()) return it->second;
    const auto id=static_cast<std::uint32_t>(result.symbols.size()); result.symbols.push_back(name); symbols[name]=id; return id;
  }
  std::uint32_t temporary() {
    const auto id=static_cast<std::uint32_t>(result.symbols.size()); result.symbols.emplace_back(); return id;
  }
  std::uint32_t emit(Op op,std::uint32_t a=0,std::uint32_t b=0) {
    const auto id=static_cast<std::uint32_t>(result.code.size()); result.code.push_back({op,a,b,current,{}}); return id;
  }
  void constant(vm::Value value) { result.code[emit(Op::Constant)].literal=value; }
  void patch(std::uint32_t id) { result.code[id].a=static_cast<std::uint32_t>(result.code.size()); }
  std::uint32_t procedure(const std::string &name) {
    const auto it=functions.find(name); if (it!=functions.end()) return it->second;
    const auto found=definitions.find(name); if (found==definitions.end()) throw std::runtime_error("Unknown procedure: "+name);
    const auto id=static_cast<std::uint32_t>(pending.size()); pending.push_back(found->second);
    functions[name]=id; result.procedures.emplace_back(); result.procedureNames.push_back(name); return id;
  }
  void input(const VisualNode &node,const std::string &key) {
    const auto found=node.inputs.find(key); if (found==node.inputs.end()) throw std::runtime_error("Missing input: "+key);
    expression(*source.find(found->second));
  }
  std::uint32_t property(const std::string &name) {
    const auto &props=VisualStateProperties();
    const auto it=std::find_if(props.begin(),props.end(),[&](const auto &p) { return p.first==name; });
    if (it==props.end()) throw std::runtime_error("Unknown state property: "+name);
    return static_cast<std::uint32_t>(it-props.begin());
  }
  void expression(const VisualNode &node) {
    const auto old=current; current=node.id;
    const auto &id=node.definitionId;
    if (id=="values/none") constant({});
    else if (id=="values/boolean") constant(vm::boolean(field(node,"value")=="true"));
    else if (id=="values/text" || id=="inputs/action-name" || id=="targets/plane") constant({vm::Kind::Text,text(field(node,"value"))});
    else if (id=="values/number-range" || id=="values/integer-range") {
      for (const auto key : {"minimum","maximum"}) {
        const auto value=ParseNumberValue(field(node,key)); if (!value) throw std::runtime_error("Invalid numeric literal.");
        constant(vm::number(*value));
      }
      emit(id=="values/integer-range" ? Op::IntegerRange : Op::Range,0,2);
    } else if (id.rfind("values/",0)==0) {
      const auto value=ParseNumberValue(field(node,"value")); if (!value) throw std::runtime_error("Invalid numeric literal.");
      constant(vm::number(*value));
    } else if (id=="simulation/set-input") {
      emit(Op::Inputs);
      for (const auto key : {"time","action","value"}) input(node,key);
      emit(Op::SetInput,0,4); emit(Op::UseInputs,0,1);
    } else if (id=="data/get") emit(Op::Load,symbol(field(node,"name")));
    else if (id=="events/value") emit(Op::EventValue);
    else if (id=="events/state") emit(Op::EventState);
    else if (id=="simulation/step") { tick(); constant({}); }
    else if (id=="events/send") {
      const auto message=temporary(), payload=temporary(), state=temporary();
      input(node,"message"); emit(Op::CheckText); emit(Op::Local,message);
      input(node,"value"); emit(Op::Local,payload);
      emit(Op::EventCapture); emit(Op::Local,state);
      dispatch("events/on-message",payload,state,message);
      for (const auto slot : {message,payload,state}) { constant({}); emit(Op::Local,slot); }
      constant({});
    }
    else if (id=="procedures/map") {
      result.usesDynamicCalls=true;
      for (const auto &[name, definition] : definitions) procedure(name);
      const auto function=temporary(), items=temporary(), context=temporary(), baseline=temporary(), counter=temporary();
      input(node,"function"); emit(Op::CheckFunction); emit(Op::Local,function);
      input(node,"list"); emit(Op::Local,items);
      emit(Op::Load,items); emit(Op::Length,0,1); emit(Op::Drop);
      emit(Op::Load,function); emit(Op::Load,items); input(node,"workers");
      emit(Op::MapPrepare,0,3); emit(Op::Local,context);
      emit(Op::Load,items); emit(Op::Length,0,1); constant(vm::number(0)); emit(Op::Greater,0,2);
      const auto empty=emit(Op::IfFalse);
      emit(Op::Load,context); emit(Op::MapNeedsRestore,0,1); const auto noSnapshot=emit(Op::IfFalse);
      emit(Op::Snapshot); emit(Op::Local,baseline);
      patch(noSnapshot);
      constant(vm::number(1)); emit(Op::Local,counter);
      const auto top=static_cast<std::uint32_t>(result.code.size());
      const auto restore=[&]() {
        emit(Op::Load,context); emit(Op::MapNeedsRestore,0,1); const auto skip=emit(Op::IfFalse);
        emit(Op::Load,baseline); emit(Op::Restore,0,1); emit(Op::Drop); patch(skip);
      };
      restore();
      emit(Op::Load,context); emit(Op::Load,counter); emit(Op::MapEnter,0,2); emit(Op::Drop);
      emit(Op::Load,context); emit(Op::Load,counter);
      emit(Op::Load,function);
      emit(Op::Load,items); emit(Op::Load,counter); emit(Op::Item,0,2);
      emit(Op::Apply,0,1); emit(Op::MapCollect,0,3); emit(Op::Drop);
      emit(Op::Load,counter); constant(vm::number(1)); emit(Op::Add,0,2); emit(Op::Local,counter);
      emit(Op::Load,counter); emit(Op::Load,items); emit(Op::Length,0,1); emit(Op::LessEqual,0,2);
      emit(Op::IfTrue,top);
      restore();
      patch(empty); emit(Op::Load,context); emit(Op::MapResult,0,1);
      for (const auto slot : {function,items,context,baseline,counter}) { constant({}); emit(Op::Local,slot); }
    }
    else if (id=="procedures/apply" || id=="procedures/do") {
      // A function can arrive through globals, collections or a prior call.
      result.usesDynamicCalls=true;
      for (const auto &[name, definition] : definitions) procedure(name);
      input(node,"function"); emit(Op::CheckFunction); input(node,"arguments");
      emit(id=="procedures/apply" ? Op::Apply : Op::Do);
      if (id=="procedures/do") constant({});
    }
    else if (id=="procedures/reference") emit(Op::Reference,procedure(field(node,"name")));
    else if (id=="procedures/value") call(node,true);
    else if (id=="conditions/and" || id=="conditions/or") {
      input(node,"a"); const auto shortcut=emit(id=="conditions/and" ? Op::IfFalse : Op::IfTrue);
      input(node,"b"); // Validate the right operand as a Boolean too.
      emit(Op::Not,0,1); emit(Op::Not,0,1);
      const auto done=emit(Op::Jump); patch(shortcut); constant(vm::boolean(id=="conditions/or")); patch(done);
    } else {
      static const std::map<std::string,std::string> reads{{"simulation/car-position","position"},{"simulation/car-velocity","velocity"},
        {"simulation/car-local-velocity","local-velocity"},{"simulation/car-speed","speed"},{"simulation/car-rotation","rotation"},
        {"simulation/stunt-points","stunt-points"},{"simulation/finish-time","finish-time"},{"simulation/checkpoint-count","checkpoints"},
        {"simulation/race-completed","finished"},{"simulation/sliding","sliding"},{"simulation/freewheeling","freewheeling"}};
      const auto read=reads.find(id);
      if (read!=reads.end()) emit(Op::ReadCurrent,property(read->second),0);
      else {
        const auto found=Builtins().find(id);
        if (found==Builtins().end()) { emit(Op::Interpret); current=old; return; }
        auto op=found->second.op; std::uint32_t option=0;
        if (op==Op::Component) { const auto axis=field(node,"axis"); option=axis=="x" ? 0 : axis=="y" ? 1 : 2; }
        if (op==Op::Read) option=property(field(node,"property"));
        if (op==Op::Read && source.find(node.inputs.at("state"))->definitionId=="simulation/state") op=Op::ReadCurrent;
        if (op==Op::Read && source.find(node.inputs.at("state"))->definitionId=="simulation/previous-state") op=Op::ReadPrevious;
        if (op==Op::Read && source.find(node.inputs.at("state"))->definitionId=="events/state") op=Op::EventRead;
        const bool direct=op==Op::ReadCurrent || op==Op::ReadPrevious || op==Op::EventRead;
        if (!direct) for (const auto &key : found->second.inputs) input(node,key);
        emit(op,option,direct ? 0 : static_cast<std::uint32_t>(found->second.inputs.size()));
        if (op==Op::Step) result.usesPhysics=true;
      }
    }
    current=old;
  }
  void call(const VisualNode &node,bool reporter) {
    const auto target=procedure(field(node,"name")); const auto parameters=VisualProcedureParameters(node);
    for (std::size_t i=0;i<parameters.size();++i) input(node,"arg"+std::to_string(i));
    emit(reporter ? Op::CallValue : Op::Call,target,static_cast<std::uint32_t>(parameters.size()));
  }
  void handle(const VisualNode &node,std::uint32_t payload,std::uint32_t state) {
    auto found=handlers.find(node.id);
    if (found==handlers.end()) {
      const auto index=static_cast<std::uint32_t>(pending.size());
      pending.push_back(&node); result.procedures.emplace_back(); result.procedureNames.emplace_back();
      found=handlers.emplace(node.id,index).first;
    }
    emit(Op::Load,payload); emit(Op::Load,state); emit(Op::EventEnter);
    emit(Op::Call,found->second,0); emit(Op::EventExit);
  }
  void dispatch(const std::string &event,std::uint32_t payload,std::uint32_t state,std::uint32_t message=0) {
    for (auto id : source.topLevel) {
      const auto &node=*source.find(id);
      if (!node.enabled || node.definitionId!=event) continue;
      if (event=="events/on-message") {
        emit(Op::Load,message); constant({vm::Kind::Text,text(field(node,"name"))}); emit(Op::Equal,0,2);
        const auto skip=emit(Op::IfFalse); handle(node,payload,state); patch(skip);
      } else handle(node,payload,state);
    }
  }
  void tick() {
    result.usesPhysics=true;
    std::vector<std::pair<const VisualNode *,std::uint32_t>> edges;
    bool observed=false, checkpointEvent=false, finishEvent=false;
    for (auto id : source.topLevel) {
      const auto &node=*source.find(id);
      if (!node.enabled) continue;
      if (node.definitionId=="events/when") {
        const auto before=temporary(); input(node,"condition"); emit(Op::Not,0,1); emit(Op::Local,before);
        edges.emplace_back(&node,before);
      }
      if (node.definitionId=="events/on-tick" || node.definitionId=="events/on-checkpoint" ||
          node.definitionId=="events/on-finish" || node.definitionId=="events/when") observed=true;
      checkpointEvent|=node.definitionId=="events/on-checkpoint";
      finishEvent|=node.definitionId=="events/on-finish";
    }
    emit(Op::Step); emit(Op::Drop);
    if (!observed) return;
    const auto state=temporary(), checkpoint=temporary(), finished=temporary();
    emit(Op::EventCapture); emit(Op::Local,state);
    if (checkpointEvent) {
      emit(Op::Load,state); emit(Op::Read,property("checkpoints"),1);
      emit(Op::ReadPrevious,property("checkpoints")); emit(Op::Greater,0,2); emit(Op::Local,checkpoint);
    }
    if (finishEvent) {
      emit(Op::Load,state); emit(Op::Read,property("finished"),1);
      emit(Op::ReadPrevious,property("finished")); emit(Op::Not,0,1);
      const auto notNew=emit(Op::IfFalse); emit(Op::Not,0,1); emit(Op::Not,0,1);
      const auto done=emit(Op::Jump); patch(notNew); emit(Op::Drop); constant(vm::boolean(false)); patch(done); emit(Op::Local,finished);
    }
    // All edge predicates are sampled before any handler can change variables.
    for (const auto &[node,trigger] : edges) {
      emit(Op::Load,trigger); const auto skip=emit(Op::IfFalse);
      input(*node,"condition"); emit(Op::Not,0,1); emit(Op::Not,0,1); emit(Op::Local,trigger); patch(skip);
    }
    dispatch("events/on-tick",state,state);
    if (checkpointEvent) {
      emit(Op::Load,checkpoint); const auto skip=emit(Op::IfFalse); dispatch("events/on-checkpoint",state,state); patch(skip);
    }
    if (finishEvent) {
      emit(Op::Load,finished); const auto skip=emit(Op::IfFalse); dispatch("events/on-finish",state,state); patch(skip);
    }
    for (const auto &[node,trigger] : edges) {
      emit(Op::Load,trigger); const auto skip=emit(Op::IfFalse); handle(*node,state,state); patch(skip);
    }
    constant({}); emit(Op::Local,state);
  }
  void body(const VisualNode &node,const std::string &key="body") {
    const auto found=node.statements.find(key); if (found==node.statements.end()) return;
    for (const auto id : found->second) command(*source.find(id));
  }
  void command(const VisualNode &node) {
    if (!node.enabled) return;
    const auto old=current; current=node.id; const auto &id=node.definitionId;
    if (id=="data/set" || id=="data/local" || id=="data/change") {
      input(node,"value"); emit(id=="data/set" ? Op::Store : id=="data/local" ? Op::Local : Op::Change,symbol(field(node,"name")));
    } else if (id=="procedures/call") call(node,false);
    else if (id=="procedures/return") { input(node,"value"); emit(Op::Return); }
    else if (id=="flow/section") body(node);
    else if (id=="flow/try") {
      // A successful guarded body needs no interpreter. On error the executor
      // replays only this lane in source, preserving exact catch diagnostics.
      body(node);
    }
    else if (id=="flow/stop") emit(Op::Stop);
    else if (id=="flow/break" || id=="flow/continue") {
      if (loops.empty()) throw std::runtime_error("Loop control outside a loop.");
      (id=="flow/break" ? loops.back().exits : loops.back().continues).push_back(emit(Op::Jump));
    } else if (id=="flow/if") {
      input(node,"condition"); const auto no=emit(Op::IfFalse); body(node);
      const auto done=emit(Op::Jump); patch(no); body(node,"else"); patch(done);
    } else if (id=="flow/while" || id=="flow/until" || id=="flow/forever" || id=="flow/repeat" || id=="flow/for-each") {
      const bool counted=id=="flow/repeat", each=id=="flow/for-each";
      std::uint32_t counter=0,list=0;
      if (counted) { counter=temporary(); input(node,"count"); emit(Op::RepeatCount,0,1); emit(Op::Local,counter); }
      if (each) { list=temporary(); counter=temporary(); input(node,"list"); emit(Op::Local,list); constant(vm::number(1)); emit(Op::Local,counter); }
      const auto top=static_cast<std::uint32_t>(result.code.size()); loops.emplace_back();
      if (counted) { emit(Op::Load,counter); constant(vm::number(0)); emit(Op::Greater,0,2); loops.back().exits.push_back(emit(Op::IfFalse)); }
      if (each) {
        emit(Op::Load,counter); emit(Op::Load,list); emit(Op::Length,0,1); emit(Op::LessEqual,0,2);
        loops.back().exits.push_back(emit(Op::IfFalse)); emit(Op::Load,list); emit(Op::Load,counter); emit(Op::Item,0,2);
        emit(Op::Store,symbol(field(node,"name")));
      }
      if (id=="flow/while" || id=="flow/until") { input(node,"condition"); loops.back().exits.push_back(emit(id=="flow/while" ? Op::IfFalse : Op::IfTrue)); }
      body(node);
      for (auto jump : loops.back().continues) patch(jump);
      if (counted || each) { emit(Op::Load,counter); constant(vm::number(each ? 1 : -1)); emit(Op::Add,0,2); emit(Op::Local,counter); }
      emit(Op::Jump,top); for (auto jump : loops.back().exits) patch(jump); loops.pop_back();
      if (each) { constant({}); emit(Op::Local,list); }
    } else { expression(node); emit(Op::Drop); }
    current=old;
  }
  ProgramBytecode compile(const std::string &entry) {
    result.entry=procedure(entry);
    for (std::size_t i=0;i<pending.size();++i) {
      const auto &node=*pending[i]; current=node.id;
      const auto parameters=node.definitionId=="procedures/define" ? VisualProcedureParameters(node) : std::vector<std::string>{};
      result.procedures[i]={static_cast<std::uint32_t>(result.code.size()),static_cast<std::uint32_t>(result.arguments.size()),static_cast<std::uint32_t>(parameters.size())};
      for (const auto &parameter : parameters) result.arguments.push_back(symbol(parameter));
      body(node); emit(Op::End);
      // Local lookup is frame-scoped in the source language. Encode only the
      // slots this procedure can introduce, not every symbol in the workspace.
      auto &function=result.procedures[i];
      std::map<std::uint32_t,std::uint32_t> slots;
      const auto slot=[&](std::uint32_t symbol) {
        return slots.emplace(symbol,static_cast<std::uint32_t>(slots.size())).first->second;
      };
      for (std::uint32_t j=0;j<function.argumentCount;++j) {
        auto &argument=result.arguments[function.argumentOffset+j]; argument=slot(argument);
      }
      for (auto pc=function.entry;pc<result.code.size();++pc)
        if (result.code[pc].op==Op::Local) slot(result.code[pc].a);
      for (auto pc=function.entry;pc<result.code.size();++pc) {
        auto &instruction=result.code[pc];
        if (instruction.op!=Op::Load && instruction.op!=Op::Store && instruction.op!=Op::Local && instruction.op!=Op::Change) continue;
        const auto found=slots.find(instruction.a);
        instruction.b=found==slots.end() ? 0 : found->second+1;
      }
      function.localCount=static_cast<std::uint32_t>(slots.size());
    }
    // Isolate only state a child may change or whose RNG it may observe.
    // Unknown indirect targets stay conservative; direct calls reach a fixed point.
    std::vector<std::vector<std::uint32_t>> callers(result.procedures.size());
    std::vector<std::uint32_t> effects;
    for (std::uint32_t i=0;i<result.procedures.size();++i) {
      auto &function=result.procedures[i]; function.effects=0;
      const auto end=i+1<result.procedures.size() ? result.procedures[i+1].entry : result.code.size();
      for (auto pc=function.entry;pc<end;++pc) {
        const auto &instruction=result.code[pc];
        switch (instruction.op) {
        case Op::Step: case Op::UseInputs: case Op::Restore: case Op::Restart: case Op::SetHorizon:
          function.effects|=vm::Procedure::Physics; break;
        case Op::Random: case Op::RandomInteger: case Op::Seed: case Op::MapPrepare:
          function.effects|=vm::Procedure::RandomState; break;
        case Op::Store: case Op::Change: function.effects|=vm::Procedure::Globals; break;
        case Op::Count: case Op::AddCount: case Op::Iterations: case Op::ClearResult: case Op::HasResult:
        case Op::ResultScore: case Op::ResultSnapshot: case Op::Publish: case Op::PublishSnapshot: case Op::Snapshot:
          function.effects|=vm::Procedure::Results; break;
        case Op::EventEnter: case Op::EventExit: case Op::EventValue: case Op::EventState: case Op::EventRead: case Op::EventCapture:
          function.effects|=vm::Procedure::Events; break;
        case Op::Apply: case Op::Do: case Op::Interpret: function.effects=vm::Procedure::All; break;
        case Op::Call: case Op::CallValue:
          if (instruction.a<callers.size()) callers[instruction.a].push_back(i);
          else function.effects=vm::Procedure::All;
          break;
        default: break;
        }
      }
      if (function.effects) effects.push_back(i);
    }
    while (!effects.empty()) {
      const auto callee=effects.back(); effects.pop_back();
      for (const auto caller : callers[callee]) {
        const auto combined=result.procedures[caller].effects | result.procedures[callee].effects;
        if (combined==result.procedures[caller].effects) continue;
        result.procedures[caller].effects=combined; effects.push_back(caller);
      }
    }
    result.initialGlobalReads.assign(result.symbols.size(),false);
    result.procedureInitialGlobalReads.resize(result.procedures.size());
    for (std::size_t i=0;i<result.procedures.size();++i) {
      const auto &function=result.procedures[i];
      const auto end=i+1<result.procedures.size() ? result.procedures[i+1].entry : result.code.size();
      std::vector<bool> parameters(function.localCount,false);
      for (std::uint32_t j=0;j<function.argumentCount;++j)
        parameters[result.arguments[function.argumentOffset+j]]=true;
      std::vector<std::uint32_t> candidates;
      for (auto pc=function.entry;pc<end;++pc) {
        const auto &instruction=result.code[pc];
        if (instruction.op!=Op::Load && instruction.op!=Op::Change) continue;
        if (!instruction.b || !parameters[instruction.b-1])
          candidates.push_back(instruction.a);
      }
      std::sort(candidates.begin(),candidates.end());
      candidates.erase(std::unique(candidates.begin(),candidates.end()),candidates.end());
      if (candidates.empty()) continue;
      std::vector<std::size_t> visited(end-function.entry,0);
      std::vector<std::uint32_t> pendingReads;
      auto budget=std::min<std::size_t>(2000000,(end-function.entry)*64);
      auto dynamicBudget=budget;
      for (const auto symbol : candidates) {
        if (result.initialGlobalReads[symbol] && !result.usesDynamicCalls) {
          result.procedureInitialGlobalReads[i].push_back(symbol); continue;
        }
        auto &remaining=result.initialGlobalReads[symbol] ? dynamicBudget : budget;
        bool required=false;
        pendingReads.assign(1,function.entry);
        while (!pendingReads.empty()) {
          const auto pc=pendingReads.back(); pendingReads.pop_back();
          if (pc<function.entry || pc>=end || !remaining) {
            // An unfinished proof imports the value conservatively. Bound
            // analysis work independently of the number of variable names.
            required=true; break;
          }
          auto &seen=visited[pc-function.entry];
          if (seen==static_cast<std::size_t>(symbol)+1) continue;
          seen=static_cast<std::size_t>(symbol)+1; --remaining;
          const auto &instruction=result.code[pc];
          if ((instruction.op==Op::Load || instruction.op==Op::Change) && instruction.a==symbol) {
            required=true; break;
          }
          // Either assignment makes subsequent reads independent of the
          // entry value: it writes the global or initializes a frame local.
          if ((instruction.op==Op::Store || instruction.op==Op::Local) && instruction.a==symbol) continue;
          if (instruction.op==Op::Return || instruction.op==Op::End || instruction.op==Op::Stop ||
              instruction.op==Op::Interpret) continue;
          if (instruction.op==Op::Jump || instruction.op==Op::IfFalse || instruction.op==Op::IfTrue)
            pendingReads.push_back(instruction.a);
          if (instruction.op!=Op::Jump && pc+1<end) pendingReads.push_back(pc+1);
        }
        if (required) {
          result.initialGlobalReads[symbol]=true;
          result.procedureInitialGlobalReads[i].push_back(symbol);
        }
      }
    }
    return std::move(result);
  }
};
} // namespace

vm::Program ProgramBytecode::view() const {
  vm::Program result{code.data(),procedures.data(),arguments.data(),static_cast<std::uint32_t>(code.size()),
    static_cast<std::uint32_t>(procedures.size()),static_cast<std::uint32_t>(symbols.size()),{}};
  std::copy(actionNames.begin(),actionNames.end(),result.actionNames); return result;
}

BytecodeCompilation CompileMappedProgram(const VisualProgram &source,const std::string &procedure) {
  try { return {Compiler(source).compile(procedure),{}}; }
  catch (const std::exception &error) { return {{},error.what()}; }
}

namespace {
std::vector<std::uint8_t> ReachabilityBeforeReturn(const ProgramBytecode &program,vm::Op target) {
  std::vector<std::uint8_t> result(program.code.size(),0);
  if (std::none_of(program.code.begin(),program.code.end(),[&](const auto &instruction) {
        return instruction.op==target;
      })) return result;
  std::vector<std::vector<std::uint32_t>> predecessors(program.code.size());
  std::vector<std::uint32_t> pending;
  const auto mark=[&](std::uint32_t pc) {
    if (!result[pc]) { result[pc]=1; pending.push_back(pc); }
  };
  for (std::size_t pc=0;pc<program.code.size();++pc) {
    const auto &instruction=program.code[pc];
    const auto edge=[&](std::size_t next) {
      if (next<program.code.size()) predecessors[next].push_back(static_cast<std::uint32_t>(pc));
    };
    if (instruction.op==target) mark(static_cast<std::uint32_t>(pc));
    switch (instruction.op) {
    case vm::Op::Apply: case vm::Op::Do: mark(static_cast<std::uint32_t>(pc)); break;
    case vm::Op::Return: case vm::Op::End: case vm::Op::Stop: case vm::Op::Interpret: continue;
    case vm::Op::Jump: edge(instruction.a); continue;
    case vm::Op::IfFalse: case vm::Op::IfTrue: edge(instruction.a); break;
    case vm::Op::Call: case vm::Op::CallValue:
      if (instruction.a<program.procedures.size()) edge(program.procedures[instruction.a].entry);
      else mark(static_cast<std::uint32_t>(pc));
      break;
    default: break;
    }
    edge(pc+1);
  }
  while (!pending.empty()) {
    const auto pc=pending.back(); pending.pop_back();
    for (const auto previous : predecessors[pc]) mark(previous);
  }
  return result;
}
}

std::vector<std::uint8_t> HistoryReadReachability(const ProgramBytecode &program) {
  return ReachabilityBeforeReturn(program,vm::Op::History);
}

std::vector<std::uint8_t> RestoreReachability(const ProgramBytecode &program) {
  return ReachabilityBeforeReturn(program,vm::Op::Restore);
}

std::string BytecodeError(vm::Error error) {
  switch (error) {
  case vm::Error::None: return {};
  case vm::Error::Capacity: return "The compiled program exceeded its memory limit.";
  case vm::Error::Type: return "The value has the wrong type for this operation.";
  case vm::Error::Unset: return "The variable has not been set.";
  case vm::Error::Integer: return "Expected a whole number in the operation's allowed range.";
  case vm::Error::NonFinite: return "The calculation did not produce a finite number.";
  case vm::Error::Bounds: return "The item index is outside the collection.";
  case vm::Error::Range: return "The range minimum exceeds its maximum or its step is zero.";
  case vm::Error::DivideByZero: return "Division by zero.";
  case vm::Error::Recursion: return "Procedure or collection nesting exceeds 64 levels.";
  case vm::Error::NoReturn: return "The procedure ended without returning a value.";
  case vm::Error::ArgumentCount: return "Procedure argument count does not match.";
  case vm::Error::Time: return "Time must be aligned to the simulation tick.";
  case vm::Error::Horizon: return "Simulation reached its horizon.";
  case vm::Error::PastInputs: return "Past inputs changed. Restore an earlier simulation state.";
  case vm::Error::Unsorted: return "Sort inputs by time before applying the sequence.";
  case vm::Error::DuplicateInput: return "Two inputs have the same time and action.";
  case vm::Error::ReadOnlyInput: return "Choose a writable car input action.";
  case vm::Error::UnknownAction: return "Unknown input action.";
  case vm::Error::NoResult: return "The program has not kept a result.";
  case vm::Error::Geometry: return "Invalid or zero-sized geometry.";
  case vm::Error::Physics: return "The CUDA physics step failed.";
  case vm::Error::Cancelled: return "Program stopped.";
  case vm::Error::InstructionLimit: return "The program exceeded its operation limit.";
  case vm::Error::Unsupported: return "This operation requires the source interpreter.";
  case vm::Error::Interpreter: return "Inspection switched this job to the source interpreter.";
  }
  return "Unknown compiled-program error.";
}
} // namespace forevertas::blocks
