#include "blocks/block_value.h"
#include "blocks/visual_catalog.h"
#include "blocks/visual_compiler.h"
#include "blocks/visual_runtime.h"
#include "blocks/program_value_codec.h"
#include "blocks/program_cuda.h"
#include "blocks/parallel_executor.h"
#include <random>
#include "blocks/visual_debugger.h"
#include "blocks/visual_macros.h"
#include "searches/search_runner.h"
#include "replay_file_io.h"
#include <forevervalidator/native.h>

#include <cmath>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <array>
#include <future>
#include <thread>
#include <set>

namespace {
using namespace forevertas;
using namespace forevertas::blocks;
using Id = VisualNodeId;

void Check(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

struct OperandProbe {
  unsigned polls=0,stopAfter=0;
  bool cancelled() { return stopAfter && ++polls>=stopAfter; }
  bool interpreterRequested() const { return false; }
  vm::Value apply(vm::Op op,vm::Machine<OperandProbe> &,vm::Value *,std::uint32_t) {
    Check(op==vm::Op::Time,"Unexpected operand-test physics.");
    return vm::number(11);
  }
};

struct OperandTrace {
  std::uint32_t pc,size,frames;
  std::uint64_t operations,source;
  vm::Error error;
  std::vector<vm::Value> stack;
  std::vector<unsigned char> arena;
  std::vector<std::uint32_t> free;
};

std::vector<OperandTrace> TraceOperands(unsigned variant,unsigned capacity,
    unsigned quantum,unsigned limit,unsigned stopAfter) {
  using vm::Op;
  std::vector<vm::Instruction> code;
  const auto emit=[&](Op op,unsigned a=0,unsigned b=0,vm::Value literal={}) {
    code.push_back({op,a,b,code.size()+100,literal});
  };
  const auto number=[&](double value) { emit(Op::Constant,0,0,vm::number(value)); };
  unsigned helper=0;
  if (variant==0) {
    number(900); number(10); number(20); emit(Op::CallValue,1,2); emit(Op::Add,0,2);
    for (unsigned i=1;i<=4;++i) number(i);
    emit(Op::List,0,4); number(7); emit(Op::Append,0,2); number(1); emit(Op::Item,0,2);
    emit(Op::Add,0,2); emit(Op::Time); emit(Op::Add,0,2); emit(Op::Return);
    helper=code.size(); emit(Op::Load,1,1); emit(Op::Load,2,2); emit(Op::Add,0,2); emit(Op::Return);
  } else if (variant==1) {
    emit(Op::List); number(7); emit(Op::Append,0,2); number(2); emit(Op::Add,0,2); emit(Op::Return);
  } else if (variant==2) {
    number(42);
    for (unsigned i=0;i<300;++i) { number(i); emit(Op::Drop); }
    emit(Op::Return);
  } else {
    if (variant==4) emit(Op::Reference,1);
    emit(Op::List); number(7); emit(Op::Append,0,2);
    if (variant==4) emit(Op::Apply,0,1); else emit(Op::CallValue,1,1);
    number(1); emit(Op::Item,0,2); emit(Op::Return);
    helper=code.size(); emit(Op::Load,1,1); emit(Op::Return);
  }
  const auto arity=variant>=3 ? 1u : 2u;
  const vm::Procedure procedures[]={{0,0,1,1},{helper,1,arity,arity}};
  const std::uint32_t arguments[]={0,0,1};
  OperandProbe probe; probe.stopAfter=stopAfter;
  vm::Machine<OperandProbe> machine({code.data(),procedures,arguments,
      static_cast<std::uint32_t>(code.size()),2,3},probe);
  std::vector<unsigned char> arena(4096);
  std::vector<vm::Value> stack(capacity);
  machine.memory.bytes=arena.data(); machine.memory.capacity=arena.size();
  machine.globals=machine.memory.allocate(vm::Kind::List,3);
  machine.stack=stack.data(); machine.stackCapacity=capacity; machine.operationLimit=limit;
  machine.begin(0,vm::number(0),7);
  std::vector<OperandTrace> trace;
  for (;;) {
    unsigned remaining=quantum;
    const bool finished=machine.resume([&](const auto &) { return quantum && remaining--==0; });
    trace.push_back({machine.pc,machine.stackSize,machine.frameCount,machine.operations,machine.source,machine.error,
        {stack.begin(),stack.begin()+machine.stackSize},{arena.begin(),arena.begin()+machine.memory.used},
        {std::begin(machine.memory.free),std::end(machine.memory.free)}});
    if (finished) break;
  }
  const auto result=machine.result();
  if (!limit && !stopAfter && capacity>=5) {
    Check(result.error==(variant==1 ? vm::Error::Type : vm::Error::None),"Operand test returned the wrong error.");
    if (variant!=1) Check(result.value.x==(variant==0 ? 948 : variant==2 ? 42 : 7),"Operand test returned the wrong value.");
  }
  trace.push_back({machine.pc,machine.stackSize,machine.frameCount,result.operations,result.source,result.error,
      {result.value},{arena.begin(),arena.begin()+machine.memory.used},
      {std::begin(machine.memory.free),std::end(machine.memory.free)}});
  return trace;
}

void OperandResumeParity() {
  unsigned cases=0;
  for (unsigned variant=0;variant<5;++variant) for (unsigned capacity=1;capacity<=8;++capacity)
    for (const auto quantum : {0u,1u,2u,5u}) for (const auto limit : {0u,1u,5u,12u,22u,256u})
      for (const auto stopAfter : {0u,1u}) {
        const auto expected=TraceOperands(variant,capacity,1,limit,stopAfter);
        const auto actual=TraceOperands(variant,capacity,quantum,limit,stopAfter);
        std::size_t boundary=0;
        for (std::size_t i=0;i<actual.size();++i) {
          const auto &a=actual[i];
          while (boundary+2<expected.size() && expected[boundary].operations<a.operations) ++boundary;
          const auto &e=i+1==actual.size() ? expected.back() : expected[boundary];
          Check(a.pc==e.pc && a.size==e.size && a.frames==e.frames && a.operations==e.operations &&
              a.source==e.source && a.error==e.error && a.arena==e.arena && a.free==e.free && a.stack.size()==e.stack.size(),
              "Suspension changed accounting, ownership or control state.");
          for (std::size_t j=0;j<a.stack.size();++j)
            Check(std::memcmp(&a.stack[j],&e.stack[j],sizeof(vm::Value))==0,"A suspended operand stack changed.");
        }
        ++cases;
      }
  std::cout << "PASS operand ownership, calls, suspension, capacity, limits and cancellation: " << cases << " cases\n";
}

class Program {
public:
  VisualProgram graph;
  Id block(const std::string &id, std::map<std::string,Id> inputs = {},
           std::map<std::string,std::string> fields = {},
           std::map<std::string,std::vector<Id>> statements = {}) {
    // Test shorthand expands multiple ticks into visible ordinary repetition.
    if (id == "simulation/step" && inputs.count("ticks"))
      return block("flow/repeat", {{"count",inputs.at("ticks")}}, {}, {{"body",{block("simulation/step")}}});
    const auto *definition = FindVisualBlock(id);
    Check(definition != nullptr, "Unknown test block: " + id);
    VisualNode node;
    node.id = ++next_;
    node.definitionId = id;
    for (const auto &field : definition->fields) node.fields[field.key] = field.defaultValue;
    for (const auto &[key,value] : fields) node.fields[key] = value;
    node.inputs = std::move(inputs);
    node.statements = std::move(statements);
    for (const auto &input : VisualInputsForNode(node)) {
      if (node.inputs.count(input.key)) continue;
      if (input.defaultBlockId.empty()) {
        using T = VisualValueType;
        const auto type = input.type;
        const std::string fallback = type == T::Snapshot ? "simulation/snapshot" :
            type == T::State ? "simulation/state" : type == T::Inputs ? "simulation/inputs" :
            type == T::Boolean ? "values/boolean" : type == T::Text ? "values/text" :
            type == T::Vector3 || type == T::Direction3 ? "targets/direction" :
            type == T::Position3 ? "targets/point" : type == T::Rotation3 ? "targets/rotation" :
            type == T::TimeRange ? "time/all" :
            type == T::Volume ? "targets/box" : type == T::List ? "data/list" : "values/number";
        node.inputs[input.key] = block(fallback);
        continue;
      }
      std::map<std::string,std::string> defaults;
      const auto *child = FindVisualBlock(input.defaultBlockId);
      if (!input.defaultValue.empty() && child->fields.size()==1) defaults[child->fields.front().key]=input.defaultValue;
      if (input.defaultBlockId == "values/number-range" || input.defaultBlockId == "values/integer-range") {
        const auto comma=input.defaultValue.find(',');
        if (comma!=std::string::npos) defaults={{"minimum",input.defaultValue.substr(0,comma)}, {"maximum",input.defaultValue.substr(comma+1)}};
      }
      if (input.defaultBlockId == "time/range" && !input.defaultValue.empty()) {
        const auto comma=input.defaultValue.find(',');
        if (comma!=std::string::npos) {
          node.inputs[input.key]=block("time/range",{
              {"from",ms(std::stod(input.defaultValue.substr(0,comma)))},
              {"to",ms(std::stod(input.defaultValue.substr(comma+1)))}});
          continue;
        }
      }
      node.inputs[input.key] = block(input.defaultBlockId,{},std::move(defaults));
    }
    const auto result=node.id;
    graph.nodes.emplace(result,std::move(node));
    return result;
  }
  Id num(double value) { return block("values/number",{},{{"value",FormatNumberValue(value)}}); }
  Id ms(double value) { return block("values/milliseconds",{},{{"value",FormatNumberValue(value)}}); }
  Id flag(bool value) { return block("values/boolean",{},{{"value",value ? "true" : "false"}}); }
  Id get(const std::string &name) { return block("data/get",{},{{"name",name}}); }
  Id set(const std::string &name, Id value) { return block("data/set",{{"value",value}},{{"name",name}}); }
  Id change(const std::string &name, double value=1) { return block("data/change",{{"value",num(value)}},{{"name",name}}); }
  Id binary(const std::string &id,Id a,Id b) { return block(id,{{"a",a},{"b",b}}); }
  Id repeat(Id count,std::vector<Id> body) { return block("flow/repeat",{{"count",count}},{},{{"body",std::move(body)}}); }
  Id branch(Id condition,std::vector<Id> yes,std::vector<Id> no={}) {
    return block("flow/if",{{"condition",condition}},{},{{"body",std::move(yes)},{"else",std::move(no)}});
  }
  Id x() { return block("math/component",{{"value",block("simulation/car-position")}},{{"axis","x"}}); }
  Id input(Id time,Id value,const std::string &action="steer") {
    return block("simulation/set-input",{{"time",time},{"value",value},
        {"action",block("inputs/action-name",{},{{"value",action}})}});
  }
  Id reference(const std::string &name) { return block("procedures/reference",{},{{"name",name}}); }
  void start(std::vector<Id> body) { graph.topLevel.insert(graph.topLevel.begin(),block("flow/when-start",{},{},{{"body",std::move(body)}})); }
  void define(const std::string &name,const std::string &parameters,std::vector<Id> body) {
    graph.topLevel.push_back(block("procedures/define",{},{{"name",name},{"parameters",parameters}},{{"body",std::move(body)}}));
  }
  Id call(const std::string &name,const std::string &parameters,std::vector<Id> arguments,bool reporter=true) {
    std::map<std::string,Id> inputs;
    for (std::size_t i=0;i<arguments.size();++i) inputs["arg"+std::to_string(i)]=arguments[i];
    return block(reporter ? "procedures/value" : "procedures/call",std::move(inputs),{{"name",name},{"parameters",parameters}});
  }
  void event(const std::string &id, std::vector<Id> body, std::map<std::string,Id> inputs={},
             std::map<std::string,std::string> fields={}) {
    graph.topLevel.push_back(block(id,std::move(inputs),std::move(fields),{{"body",std::move(body)}}));
  }
private:
  Id next_=0;
};

class TestHost final : public VisualSimulationHost {
  struct Saved final : VisualHostSnapshot {
    VisualState state;
    explicit Saved(VisualState value) : state(value) {}
  };
public:
  struct Probe { std::atomic_int active{0}, peak{0}; };
  std::shared_ptr<Probe> probe;
  std::shared_ptr<std::atomic_uint> advances;
  std::shared_ptr<VisualBatchExecutor> batch;
  VisualState state;
  VisualInputs events;
  std::uint32_t checkpointAtMs=0, finishAtMs=0;
  std::uint32_t capacity=256;
  TestHost() { state.durationMs=6000; state.car.rotationW=1; }
  VisualState read() const override { return state; }
  VisualState advance() override {
    if (advances) ++*advances;
    if (probe) {
      const int active=probe->active.fetch_add(1)+1;
      int peak=probe->peak.load();
      while (peak<active && !probe->peak.compare_exchange_weak(peak,active)) {}
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      probe->active.fetch_sub(1);
    }
    state.timeMs+=10;
    ++state.tick;
    if (checkpointAtMs && state.timeMs==checkpointAtMs) ++state.checkpointsCollected;
    if (finishAtMs && state.timeMs==finishAtMs) {
      state.raceCompleted=true;
      state.finishTimeMs=state.timeMs;
      const auto ns=static_cast<std::uint64_t>(state.timeMs)*1000000;
      state.finishTime=forevervalidator::FinishTimeEstimate{ns-1,ns,ns};
    }
    state.steering=static_cast<float>(SteeringStateAt(events,static_cast<std::int32_t>(state.timeMs)))/65536.0f;
    state.car.position.x+=state.steering;
    state.car.linearSpeed.x=state.steering*100;
    return state;
  }
  std::shared_ptr<const VisualHostSnapshot> capture() const override { return std::make_shared<Saved>(state); }
  VisualState restore(const VisualHostSnapshot &snapshot) override {
    const auto restored=dynamic_cast<const Saved &>(snapshot).state;
    if (restored.timeMs>state.durationMs) throw std::runtime_error("Restore cursor exceeds current host horizon.");
    state=restored;
    return state;
  }
  VisualInputs inputs() const override { return events; }
  void replaceInputs(VisualInputs inputs) override { events=std::move(inputs); }
  VisualState setHorizon(std::uint32_t ms) override {
    if (ms<state.timeMs) throw std::runtime_error("Horizon precedes the current host state.");
    state.durationMs=ms;
    return state;
  }
  std::unique_ptr<VisualSimulationHost> fork() const override { return std::make_unique<TestHost>(*this); }
  std::shared_ptr<VisualBatchExecutor> batchExecutor() override { return batch; }
  std::uint32_t workerCapacity() const override { return capacity; }
};

double Number(const VisualExecutionResult &result,const std::string &name) {
  return std::get<double>(result.variables.at(name).data);
}

void Fails(const Program &program,const std::string &message,VisualRuntimeControl control={}) {
  TestHost host;
  try { ExecuteVisualProgram(program.graph,host,control); }
  catch (const std::exception &error) {
    Check(std::string(error.what()).find(message)!=std::string::npos,
          "Expected diagnostic '"+message+"', got: "+error.what());
    return;
  }
  throw std::runtime_error("Expected failure: "+message);
}

void ControlAndFunctions() {
  Program p;
  p.define("factorial","n",{
      p.block("data/local",{{"value",p.get("n")}},{{"name","saved"}}),
      p.branch(p.binary("conditions/less-equal",p.get("n"),p.num(1)),
          {p.block("procedures/return",{{"value",p.num(1)}})}),
      p.block("procedures/return",{{"value",p.binary("math/multiply",p.get("saved"),
          p.call("factorial","n",{p.binary("math/subtract",p.get("n"),p.num(1))}))}})});
  p.define("sum three","a,b,c",{p.block("procedures/return",{{"value",
      p.binary("math/add",p.binary("math/add",p.get("a"),p.get("b")),p.get("c"))}})});
  p.start({p.set("n",p.num(99)),p.set("answer",p.call("factorial","n",{p.num(5)})),
      p.set("sum",p.call("sum three","a,b,c",{p.num(2),p.num(3),p.num(4)})),
      p.set("i",p.num(0)),p.set("visited",p.num(0)),
      p.block("flow/forever",{},{},{{"body",{
          p.change("i"),
          p.branch(p.binary("conditions/equal",p.get("i"),p.num(2)),{p.block("flow/continue")}),
          p.branch(p.binary("conditions/equal",p.get("i"),p.num(4)),{p.block("flow/break")}),p.change("visited")}}}),
      p.block("flow/until",{{"condition",p.binary("conditions/equal",p.get("i"),p.num(6))}},{},{{"body",{p.change("i")}}}),
      p.set("short-circuit",p.binary("conditions/and",p.flag(false),p.get("undefined")))});
  TestHost host;
  const auto result=ExecuteVisualProgram(p.graph,host);
  Check(Number(result,"answer")==120 && Number(result,"n")==99,"Procedure recursion leaked call-local variables.");
  Check(Number(result,"sum")==9,"Procedures did not accept three independently connected arguments.");
  Check(Number(result,"visited")==2 && Number(result,"i")==6,"Loop control did not execute in block order.");
  Check(!std::get<bool>(result.variables.at("short-circuit").data),"Boolean and did not short-circuit.");
}

void SnapshotAndCollections() {
  Program p;
  p.start({p.set("origin",p.block("simulation/snapshot")),p.input(p.ms(0),p.num(32768)),
      p.block("simulation/step",{{"ticks",p.num(2)}}),p.set("branch",p.block("simulation/snapshot")),
      p.set("branches",p.block("data/append",{{"list",p.block("data/list")},{"value",p.get("branch")}})),
      p.block("simulation/set-horizon",{{"time",p.ms(100)}}),
      p.block("simulation/restore",{{"snapshot",p.get("origin")}}),
      p.set("restored-input-count",p.block("inputs/count")),p.input(p.ms(0),p.num(-65536)),
      p.block("simulation/step",{{"ticks",p.num(3)}}),p.set("negative-x",p.x()),
      p.block("simulation/restore",{{"snapshot",p.block("data/item",{{"list",p.get("branches")},{"index",p.num(1)}})}}),
      p.set("restored-x",p.x()),p.set("history-count",p.block("data/length",{{"list",p.block("simulation/history")}})),
      p.set("old-time",p.block("simulation/read",{{"state",p.block("simulation/previous-state")}},{{"property","time"}})),
      p.set("snapshot-inputs",p.block("simulation/snapshot-inputs",{{"snapshot",p.get("branch")}})),
      p.set("input-time",p.block("inputs/time",{{"inputs",p.get("snapshot-inputs")},{"index",p.num(1)}}))});
  TestHost host;
  const auto result=ExecuteVisualProgram(p.graph,host);
  Check(Number(result,"restored-input-count")==0,"Snapshot restore leaked another branch's inputs.");
  Check(Number(result,"negative-x")==-3 && Number(result,"restored-x")==1,"Snapshot branches did not restore physics.");
  Check(Number(result,"history-count")==3 && Number(result,"old-time")==10,"Snapshot restore lost its history or previous state.");
  Check(host.state.durationMs==6000,"Snapshot restore did not restore the simulation horizon.");
  Check(Number(result,"input-time")==0 && host.events[0].timeMs==10,"Input times did not round-trip through the user timeline.");

  Program retained;
  retained.start({retained.set("snapshots",retained.block("data/list")),
      retained.repeat(retained.num(5),{
          retained.block("simulation/step"),
          retained.set("snapshots",retained.block("data/append",{
              {"list",retained.get("snapshots")},{"value",retained.block("simulation/snapshot")}}))})});
  TestHost retainedHost;
  const auto retainedResult=ExecuteVisualProgram(retained.graph,retainedHost);
  const auto snapshots=std::get<std::shared_ptr<const VisualList>>(retainedResult.variables.at("snapshots").data);
  std::shared_ptr<const VisualSnapshot> previousSnapshot;
  for (std::size_t index=0; index<snapshots->size(); ++index) {
    const auto snapshot=std::get<std::shared_ptr<const VisualSnapshot>>((*snapshots)[index].data);
    Check(snapshot->history->size==index+2,"Retained snapshot history length is wrong.");
    if (previousSnapshot) {
      Check(snapshot->history->previous==previousSnapshot->history,
            "Retained snapshots copied history instead of sharing its prefix.");
      Check(snapshot->inputs==previousSnapshot->inputs,
            "Retained snapshots copied an unchanged input sequence.");
    }
    previousSnapshot=snapshot;
  }
}

Program FeedbackSearch() {
  Program p;
  const auto score=[&] { return p.block("math/abs",{{"value",p.binary("math/subtract",p.x(),p.num(2))}}); };
  p.start({p.set("origin",p.block("simulation/snapshot")),p.set("steer",p.num(-65536)),p.set("branches",p.block("data/list")),
      p.repeat(p.num(3),{
          p.block("results/count"),p.block("simulation/restore",{{"snapshot",p.get("origin")}}),p.input(p.ms(0),p.get("steer")),
          p.repeat(p.num(3),{
              p.block("simulation/step"),
              p.branch(p.binary("conditions/less",p.x(),p.num(2)),
                  {p.input(p.block("simulation/time"),p.num(65536))},
                  {p.input(p.block("simulation/time"),p.num(0))})}),
          p.set("branches",p.block("data/append",{{"list",p.get("branches")},{"value",p.block("simulation/snapshot")}})),
          p.branch(p.binary("conditions/or",p.block("conditions/not",{{"value",p.block("results/has-result")}}),
              p.binary("conditions/less",score(),p.block("results/best-score"))),
              {p.block("results/publish",{{"score",score()}})}),
          p.change("steer",65536)})});
  return p;
}

void NovelSearch() {
  const auto p=FeedbackSearch();
  const auto compiled=CompileVisualProgram(p.graph);
  Check(compiled.ok && compiled.executable,"A general program was rejected or collapsed to native settings.");
  TestHost host;
  const auto result=ExecuteVisualProgram(*compiled.executable,host);
  Check(result.candidates==3 && result.published.has_value(),"Custom search did not own its iteration and result policy.");
  Check(result.published->score==0 && result.published->candidate==2,"Feedback-driven search chose the wrong branch.");
  Check(result.published->evaluationState.car.position.x==2,"Published state came from another candidate.");
  Check(std::get<std::shared_ptr<const VisualList>>(result.variables.at("branches").data)->size()==3,
        "A population of first-class snapshots could not be retained.");
  Check(result.published->snapshot->inputs->front().value.analog==0,"Selected inputs did not match the selected state.");
}

void RandomAndState() {
  Program p;
  p.start({p.block("math/seed",{{"value",p.num(177)}}),
      p.set("a",p.block("math/random-integer",{{"a",p.num(-100)},{"b",p.num(100)}})),
      p.block("math/seed",{{"value",p.num(177)}}),
      p.set("b",p.block("math/random-integer",{{"a",p.num(-100)},{"b",p.num(100)}}))});
  TestHost host;
  const auto result=ExecuteVisualProgram(p.graph,host);
  Check(Number(result,"a")==Number(result,"b"),"Program-controlled random seeds are not repeatable.");
  for (const auto &[key,label] : VisualStateProperties()) {
    (void)label;
    ReadVisualStateProperty(host.state,key);
  }
  Check(VisualStateProperties().size()>40,"Only the old search evaluator's state subset is exposed.");
  Check(std::holds_alternative<std::monostate>(ReadVisualStateProperty(host.state,"finish-time").data),
        "Missing runtime state was silently replaced with zero.");
}

void EveryReporterExecutes() {
  std::size_t checked = 0;
  for (const auto &definition : VisualBlockCatalog()) {
    if (!definition.toolboxVisible ||
        (definition.shape != VisualBlockShape::Reporter && definition.shape != VisualBlockShape::Predicate)) continue;
    Program p;
    p.define("my block", definition.id=="procedures/map" ? "item" : "", {p.block("procedures/return", {{"value", p.num(7)}})});
    std::map<std::string,Id> inputs;
    if (definition.id == "data/item" || definition.id == "data/replace-item" || definition.id == "data/delete-item")
      inputs["list"] = p.block("data/append", {{"value",p.num(1)}});
    if (definition.id == "targets/polygon-from-points" || definition.id == "targets/prism") {
      auto points=p.block("data/list");
      for (const auto &point : {std::pair<double,double>{0,0},{2,0},{0,2}})
        points=p.block("data/append",{{"list",points},{"value",p.block("targets/point",{{"x",p.num(point.first)},{"y",p.num(point.second)}})}});
      if (definition.id == "targets/prism") inputs["polygon"]=p.block("targets/polygon-from-points",{{"points",points}});
      else inputs["points"]=points;
    }
    const auto reporter = p.block(definition.id, inputs);
    p.start({p.set("value",p.num(1)),p.input(p.ms(0),p.num(100)),
        p.block("results/publish"),p.set("tested",reporter)});
    TestHost host;
    try {
      const auto compiled=CompileVisualProgram(p.graph);
      Check(compiled.ok && compiled.executable,"Reporter was not accepted by the program compiler.");
      const auto executed=ExecuteVisualProgram(*compiled.executable,host);
      Check(executed.variables.count("tested"),"Reporter failed to produce a storable value.");
    } catch (const std::exception &error) {
      throw std::runtime_error("Reporter " + definition.id + ": " + error.what());
    }
    ++checked;
  }
  std::cout << "PASS " << checked << " runtime reporters\n";
}

void DiagnosticsAndCancellation() {
  Program undefined;
  undefined.start({undefined.set("x",undefined.get("missing"))});
  Fails(undefined,"Block #");
  Program division;
  division.start({division.set("x",division.binary("math/divide",division.num(1),division.num(0)))});
  Fails(division,"Division by zero");
  Program badTicks;
  badTicks.start({badTicks.block("simulation/step",{{"ticks",badTicks.num(1.5)}})});
  Fails(badTicks,"whole number");
  Program badIndex;
  badIndex.start({badIndex.set("x",badIndex.block("data/item"))});
  Fails(badIndex,"whole number");
  Program past;
  past.start({past.input(past.ms(0),past.num(100)),past.block("simulation/step"),past.input(past.ms(0),past.num(200))});
  Fails(past,"Past inputs changed");
  Program badCall;
  badCall.start({badCall.set("x",badCall.call("missing","x",{badCall.num(1)}))});
  Fails(badCall,"Unknown procedure");
  Program signature;
  signature.define("f","x",{signature.block("procedures/return")});
  signature.start({signature.set("x",signature.call("f","y",{signature.num(1)}))});
  Fails(signature,"parameters do not match");
  Program nestedList;
  nestedList.start({nestedList.set("items",nestedList.block("data/list")),
      nestedList.repeat(nestedList.num(100),{nestedList.set("items",nestedList.block("data/append",{{"value",nestedList.get("items")}}))})});
  Fails(nestedList,"64 levels");
  Program emptyLoop;
  emptyLoop.start({emptyLoop.block("flow/forever")});
  VisualRuntimeControl control;
  int polls=0;
  control.stopRequested=[&] { return ++polls>=20; };
  TestHost host;
  const auto stopped=ExecuteVisualProgram(emptyLoop.graph,host,control);
  Check(stopped.stopped && polls==20 && host.state.timeMs==0,"An empty infinite loop ignored Stop.");
  control.stopRequested={};
  control.operationLimit=30;
  Fails(emptyLoop,"operation limit",control);
  Program recursive;
  recursive.define("recurse","",{recursive.call("recurse","",{},false)});
  recursive.start({recursive.call("recurse","",{},false)});
  Fails(recursive,"64 calls");
}

void EventScripts() {
  Program p;
  p.event("events/on-tick", {p.change("ticks"),
      p.block("events/send",{{"message",p.block("values/text",{},{{"value","tick"}})}, {"value",p.get("ticks")}})});
  p.event("events/on-message", {p.block("data/change",{{"value",p.block("events/value")}},{{"name","sum"}}),
      p.block("data/local",{{"value",p.num(9)}},{{"name","private"}})}, {}, {{"name","tick"}});
  p.event("events/when", {p.change("edges")},
      {{"condition",p.binary("conditions/greater-equal",p.block("simulation/time"),p.num(20))}});
  p.event("events/on-checkpoint", {p.change("checkpoints")});
  p.event("events/on-finish", {p.change("finishes"),p.set("finish-event",p.block("simulation/read",
      {{"state",p.block("events/state")}},{{"property","time"}}))});
  p.start({p.set("ticks",p.num(0)),p.set("sum",p.num(0)),p.set("edges",p.num(0)),
      p.set("checkpoints",p.num(0)),p.set("finishes",p.num(0)),p.block("simulation/step",{{"ticks",p.num(3)}})});
  TestHost host; host.checkpointAtMs=20; host.finishAtMs=30;
  const auto result=ExecuteVisualProgram(p.graph,host);
  Check(Number(result,"ticks")==3 && Number(result,"sum")==6,"Tick events or broadcast payloads lost execution order.");
  Check(Number(result,"edges")==1 && Number(result,"checkpoints")==1 && Number(result,"finishes")==1,
        "Simulation transitions did not trigger their visual handlers exactly once.");
  Check(Number(result,"finish-event")==30 && !result.variables.count("private"),"Event state or local scope leaked.");
  Program recursive;
  recursive.event("events/on-message",{recursive.block("events/send")});
  recursive.start({recursive.block("events/send")});
  Fails(recursive,"Event recursion");
}

bool EquivalentValue(const VisualValue &a,const VisualValue &b);

void CompiledEventScripts() {
  Program p;
  p.event("events/on-tick",{p.change("ticks"),p.block("events/send",{
      {"message",p.block("values/text",{},{{"value","tick"}})},{"value",p.get("ticks")}})});
  p.event("events/on-message",{p.block("data/change",{{"value",p.block("events/value")}},{{"name","sum"}}),
      p.block("data/local",{{"value",p.num(99)}},{{"name","ticks"}})}, {}, {{"name","tick"}});
  p.event("events/when",{p.change("edges")},{{"condition",p.binary("conditions/greater-equal",p.block("simulation/time"),p.num(20))}});
  p.event("events/on-checkpoint",{p.change("checkpoints")});
  p.event("events/on-finish",{p.set("finish",p.block("simulation/read",{{"state",p.block("events/state")}},{{"property","time"}}))});
  Id values=p.block("data/list");
  for (const auto &name : {"ticks","sum","edges","checkpoints","finish"})
    values=p.block("data/append",{{"list",values},{"value",p.get(name)}});
  p.define("event branch","item",{p.set("ticks",p.num(0)),p.set("sum",p.num(0)),p.set("edges",p.num(0)),
      p.set("checkpoints",p.num(0)),p.set("finish",p.num(0)),p.repeat(p.get("item"),{p.block("simulation/step")}),
      p.block("procedures/return",{{"value",values}})});
  p.start({p.set("values",p.block("procedures/map",{{"function",p.reference("event branch")},
      {"list",p.block("data/numbers",{{"from",p.num(1)},{"to",p.num(5)}})}}))});
  const auto code=CompileMappedProgram(p.graph,"event branch");
  Check(code.program.has_value(),"Event-driven mapped program was not compiled: "+code.reason);
  VisualValue expected;
  for (bool compiled : {false,true}) {
    TestHost host; host.checkpointAtMs=20; host.finishAtMs=30;
    VisualRuntimeControl control; control.compilePrograms=compiled; control.workerCount=1;
    std::string mode; control.executionModeChanged=[&](const std::string &value) { mode=value; };
    const auto result=ExecuteVisualProgram(p.graph,host,control);
    if (!compiled) expected=result.variables.at("values");
    else {
      Check(EquivalentValue(expected,result.variables.at("values")),"Compiled event ordering, payload, scope or transitions differ.");
      Check(mode=="Compiled block program on CPU","Event program silently fell back: "+mode);
    }
  }
  std::cout << "PASS compiled event dispatch, edge predicates, scopes and transition parity\n";
}

void CompiledReentrantEvents() {
  for (bool fullState : {false,true}) {
    Program p;
    const auto emitted=[&] {
      return p.block("simulation/read",{{"state",p.block("events/state")}},{{"property","time"}});
    };
    const auto append=[&](Id value) { return p.set("log",p.block("data/append",{{"list",p.get("log")},{"value",value}})); };
    p.event("events/on-tick",{
        append(emitted()),
        p.branch(p.binary("conditions/equal",p.block("simulation/time"),p.num(10)),{p.block("simulation/step")}),
        append(emitted()),
        p.set("armed",p.flag(true)),
        p.block("events/send",{{"message",p.block("values/text",{},{{"value","outer"}})},{"value",emitted()}})});
    p.event("events/on-message",{append(p.block("events/value")),
        p.block("events/send",{{"message",p.block("values/text",{},{{"value","inner"}})},{"value",p.num(77)}}),
        append(p.block("events/value")),p.block("procedures/return",{{"value",p.num(999)}})}, {},{{"name","outer"}});
    p.event("events/on-message",{append(p.block("events/value"))}, {},{{"name","inner"}});
    p.event("events/when",{append(p.num(1000))},{{"condition",p.get("armed")}});
    // An unused unsupported handler must not reject an unrelated branch.
    p.event("events/on-message",{p.block("simulation/restart")}, {},{{"name","never sent"}});
    if (fullState) p.event("events/on-tick",{append(p.block("events/value"))});
    p.define("reentrant branch","item",{p.set("log",p.block("data/list")),p.set("armed",p.flag(false)),
        p.repeat(p.get("item"),{p.block("simulation/step")}),p.block("procedures/return",{{"value",p.get("log")}})});
    p.start({p.set("values",p.block("procedures/map",{{"function",p.reference("reentrant branch")},
        {"list",p.block("data/numbers",{{"to",p.num(3)}})}}))});
    VisualValue expected;
    for (bool compiled : {false,true}) {
      TestHost host; VisualRuntimeControl control; control.compilePrograms=compiled;
      std::string mode; control.executionModeChanged=[&](const auto &value) { mode=value; };
      const auto result=ExecuteVisualProgram(p.graph,host,control);
      if (!compiled) expected=result.variables.at("values");
      else {
        Check(EquivalentValue(expected,result.variables.at("values")),"Reentrant events changed captured state, payload or edge order.");
        Check(mode=="Compiled block program on CPU","Reentrant event program silently fell back: "+mode);
      }
    }
  }
  std::cout << "PASS reentrant tick/message contexts, early returns and captured edge ordering\n";
}

void CompactFrameScopes() {
  Program p;
  p.define("read global","",{p.block("procedures/return",{{"value",p.get("value")}})});
  p.define("scoped branch","item",{
      p.set("write only",p.num(0)),
      p.set("before",p.get("value")),
      p.branch(p.binary("conditions/greater",p.get("item"),p.num(2)),{
          p.block("data/local",{{"value",p.get("item")}},{{"name","value"}})}),
      p.change("value"),
      p.block("procedures/return",{{"value",p.block("data/append",{
          {"list",p.block("data/append",{{"list",p.block("data/append",{{"list",p.block("data/list")},{"value",p.get("before")}})},
              {"value",p.get("value")}})}, {"value",p.call("read global","",{})}})}})});
  p.start({p.set("value",p.num(10)),p.set("values",p.block("procedures/map",{{"function",p.reference("scoped branch")},
      {"list",p.block("data/numbers",{{"to",p.num(4)}})}}))});
  const auto code=CompileMappedProgram(p.graph,"scoped branch");
  Check(code.program.has_value(),"Conditional local program was not compiled.");
  Check(code.program->procedures.front().localCount==2 && code.program->procedures.back().localCount==0,
        "Call frames still allocate workspace-wide symbol tables.");
  const auto reads=[&](const std::string &name) {
    const auto found=std::find(code.program->symbols.begin(),code.program->symbols.end(),name);
    Check(found!=code.program->symbols.end(),"Missing scoped global symbol.");
    return code.program->initialGlobalReads[found-code.program->symbols.begin()];
  };
  Check(!reads("item") && !reads("write only") && !reads("before") && reads("value"),
        "Initial globals confused parameters, writes or conditional local fallback.");
  VisualValue expected;
  for (bool compiled : {false,true}) {
    TestHost host; VisualRuntimeControl control; control.compilePrograms=compiled;
    const auto result=ExecuteVisualProgram(p.graph,host,control);
    if (!compiled) expected=result.variables.at("values");
    else Check(EquivalentValue(expected,result.variables.at("values")),"Compact locals changed conditional shadowing or call isolation.");
  }
  std::cout << "PASS compact frame slots, conditional local shadowing and global lookup\n";
}

void InitialGlobalControlFlow() {
  const bool required[]={false,false,true,true,false,false,true,false,false,false,true,true,true,false,true};
  for (unsigned variant=0;variant<std::size(required);++variant) {
    Program p;
    const auto condition=[&]() { return p.binary("conditions/greater",p.get("index"),p.num(1)); };
    const auto assign=[&]() { return p.set("value",p.num(21)); };
    const auto local=[&]() { return p.block("data/local",{{"value",p.num(21)}},{{"name","value"}}); };
    std::vector<Id> body;
    switch (variant) {
    case 0: body={assign()}; break;
    case 1: body={p.branch(condition(),{assign()},{p.set("value",p.num(22))})}; break;
    case 2: body={p.branch(condition(),{assign()})}; break;
    case 3: body={p.repeat(p.get("index"),{assign()})}; break;
    case 4: body={assign(),p.repeat(p.get("index"),{p.change("value")})}; break;
    case 5: body={p.branch(condition(),{local()},{p.set("value",p.num(22))})}; break;
    case 6: body={p.repeat(p.get("index"),{p.branch(condition(),{p.block("flow/break")}),assign()})}; break;
    case 7: body={assign(),p.repeat(p.get("index"),{p.branch(condition(),{p.block("flow/continue")}),p.change("value")})}; break;
    case 8: body={p.branch(p.binary("conditions/equal",p.get("index"),p.num(0)),
        {p.block("procedures/return",{{"value",p.num(5)}})}),assign()}; break;
    case 9: body={p.block("procedures/return",{{"value",p.num(5)}})}; break;
    case 10: body={p.branch(condition(),{local()})}; break;
    case 11: body={p.change("value")}; break;
    case 12: body={p.block("flow/while",{{"condition",p.binary("conditions/less",p.get("value"),p.num(20))}},
        {},{{"body",{assign()}}})}; break;
    case 13: body={assign(),p.block("flow/while",{{"condition",p.binary("conditions/less",p.get("value"),p.num(22))}},
        {},{{"body",{p.change("value")}}})}; break;
    case 14:
      p.define("callee","",{p.block("procedures/return",{{"value",p.get("value")}})});
      body={assign(),p.block("procedures/return",{{"value",p.call("callee","",{})}})}; break;
    }
    body.push_back(p.block("procedures/return",{{"value",p.get("value")}}));
    p.define("assignment branch","index",std::move(body));
    p.start({p.set("value",p.num(17)),p.set("values",p.block("procedures/map",{
        {"function",p.reference("assignment branch")},{"list",p.block("data/numbers",{{"from",p.num(0)},{"to",p.num(3)}})}}))});
    const auto code=CompileMappedProgram(p.graph,"assignment branch");
    Check(code.program.has_value(),"Could not compile global control-flow fixture.");
    const auto symbol=static_cast<std::size_t>(std::find(code.program->symbols.begin(),code.program->symbols.end(),"value")-
        code.program->symbols.begin());
    Check(symbol<code.program->initialGlobalReads.size() && code.program->initialGlobalReads[symbol]==required[variant],
        "Initial global control-flow proof was wrong for variant "+std::to_string(variant));
    VisualValue expected;
    for (const auto compiled : {false,true}) {
      TestHost host; VisualRuntimeControl control; control.compilePrograms=compiled;
      const auto result=ExecuteVisualProgram(p.graph,host,control);
      if (!compiled) expected=result.variables.at("values");
      else Check(EquivalentValue(expected,result.variables.at("values")),"Global assignment analysis changed source results.");
    }
    if (!required[variant]) {
      TestHost host;
      auto origin=std::make_shared<VisualSnapshot>(VisualSnapshot{host.capture(),host.state,host.state,
          std::make_shared<const VisualInputs>(),std::make_shared<VisualHistoryNode>(VisualHistoryNode{host.state,{},1}),6000,0});
      HostBytecodeStorage storage;
      const auto &values=*std::get<std::shared_ptr<const VisualList>>(expected.data);
      for (unsigned index=0;index<4;++index) {
        const auto result=ExecuteHostBytecode(*code.program,host,origin,
            {{"value",VisualValue(VisualProcedure{"unread entry value"})}},VisualValue(static_cast<double>(index)),1,{},storage);
        Check(!result.needsInterpreter && EquivalentValue(result.value,values[index]),
            "A proven overwritten global still required its initial value.");
      }
    }
  }
  Program wide;
  std::vector<Id> body;
  std::map<std::string,VisualValue> globals;
  for (unsigned i=0;i<1024;++i) {
    const auto name="written "+std::to_string(i);
    globals.emplace(name,VisualValue(17.0));
    body.push_back(wide.set(name,wide.num(0)));
  }
  for (unsigned i=0;i<1024;++i) body.push_back(wide.set("sink",wide.get("written "+std::to_string(i))));
  globals.emplace("needed",VisualValue(17.0));
  body.push_back(wide.block("procedures/return",{{"value",wide.get("needed")}}));
  wide.define("wide scope","index",std::move(body));
  const auto code=CompileMappedProgram(wide.graph,"wide scope");
  Check(code.program.has_value(),"Could not compile wide global-read proof.");
  TestHost host;
  auto origin=std::make_shared<VisualSnapshot>(VisualSnapshot{host.capture(),host.state,host.state,
      std::make_shared<const VisualInputs>(),std::make_shared<VisualHistoryNode>(VisualHistoryNode{host.state,{},1}),6000,0});
  HostBytecodeStorage storage;
  const auto actual=ExecuteHostBytecode(*code.program,host,origin,globals,VisualValue(1.0),1,{},storage);
  auto conservative=*code.program; conservative.initialGlobalReads.clear();
  const auto expected=ExecuteHostBytecode(conservative,host,origin,globals,VisualValue(1.0),1,{},storage);
  Check(!actual.needsInterpreter && !expected.needsInterpreter && std::get<double>(actual.value.data)==17 &&
      EquivalentValue(actual.value,expected.value) && actual.operations==expected.operations,
      "Bounded global-read analysis lost a required value or changed execution accounting.");
  std::cout << "PASS initial global reads: 15 control-flow variants; 1024-variable budget fixture retained "
      << std::count(code.program->initialGlobalReads.begin(),code.program->initialGlobalReads.end(),true) << " imports\n";
}

void DynamicGlobalReachability() {
  Program p;
  const auto args=[&](Id value) { return p.block("data/append",{{"list",p.block("data/list")},{"value",value}}); };
  p.define("early reader","value",{p.block("procedures/return",{{"value",p.get("temporary")}})});
  p.define("leaf","value",{p.block("procedures/return",{{"value",p.binary("math/add",p.get("required"),p.get("value"))}})});
  p.define("selected","value",{p.set("temporary",p.num(0)),p.block("procedures/return",{{"value",
      p.binary("math/add",p.call("leaf","value",{p.get("value")}),p.get("temporary"))}})});
  p.define("unrelated","value",{p.block("procedures/return",{{"value",p.block("data/length",{{"list",p.get("unrelated payload")}})}})});
  p.define("bridge","value",{p.block("procedures/return",{{"value",p.block("procedures/apply",
      {{"function",p.get("nested callback")},{"arguments",args(p.get("value"))}})}})});
  p.define("dispatch","index",{p.block("procedures/return",{{"value",p.block("procedures/apply",
      {{"function",p.block("data/item",{{"list",p.get("callbacks")},{"index",p.num(1)}})},
       {"arguments",args(p.get("index"))}})}})});
  p.define("argument dispatch","callback",{p.block("procedures/return",{{"value",p.block("procedures/apply",
      {{"function",p.block("data/item",{{"list",p.get("callback")},{"index",p.num(1)}})},
       {"arguments",args(p.num(5))}})}})});
  const auto compiled=CompileMappedProgram(p.graph,"dispatch");
  Check(compiled.program && compiled.program->usesDynamicCalls,"Dynamic global fixture did not compile.");
  const auto list=[](VisualValue value) { return VisualValue(std::make_shared<const VisualList>(VisualList{std::move(value)})); };
  std::map<std::string,VisualValue> globals={{"callbacks",list(VisualValue(VisualProcedure{"bridge"}))},
      {"nested callback",VisualValue(VisualProcedure{"selected"})},{"required",VisualValue(17.0)},
      {"unrelated payload",VisualValue(std::make_shared<const VisualList>(70000,VisualValue(1.0)))}};
  globals["temporary"]=globals.at("unrelated payload");
  const auto reads=[&](const ProgramBytecode &code,const std::vector<bool> &mask,const std::string &name) {
    const auto found=std::find(code.symbols.begin(),code.symbols.end(),name);
    Check(found!=code.symbols.end(),"Missing dynamic-global symbol.");
    return mask[found-code.symbols.begin()];
  };
  const auto mask=ResolveDynamicInitialGlobals(*compiled.program,globals,{VisualValue(1.0),VisualValue(2.0)});
  Check(mask && reads(*compiled.program,*mask,"required") && !reads(*compiled.program,*mask,"unrelated payload") &&
      !reads(*compiled.program,*mask,"temporary"),
      "Dynamic global reachability missed a nested callback or retained an unrelated definition.");
  TestHost host;
  auto origin=std::make_shared<VisualSnapshot>(VisualSnapshot{host.capture(),host.state,host.state,
      std::make_shared<const VisualInputs>(),std::make_shared<VisualHistoryNode>(VisualHistoryNode{host.state,{},1}),6000,0});
  HostBytecodeStorage storage; storage.capacity=256u*1024u;
  Check(ExecuteHostBytecode(*compiled.program,host,origin,globals,VisualValue(1.0),1,{},storage).needsInterpreter,
      "The unrelated global did not exercise conservative import capacity.");
  auto resolved=*compiled.program; resolved.initialGlobalReads=*mask;
  const auto result=ExecuteHostBytecode(resolved,host,origin,globals,VisualValue(1.0),1,{},storage);
  Check(!result.needsInterpreter && std::get<double>(result.value.data)==18,"Resolved callbacks changed execution.");
  globals["callbacks"]=list(VisualValue(VisualProcedure{"unrelated"}));
  const auto changed=ResolveDynamicInitialGlobals(*compiled.program,globals,{VisualValue(1.0)});
  Check(changed && reads(*compiled.program,*changed,"unrelated payload"),"A changed callback reused stale global reachability.");
  globals["callbacks"]=list(VisualValue(VisualProcedure{"bridge"}));
  globals["nested callback"]=VisualValue(VisualProcedure{"bridge"});
  Check(ResolveDynamicInitialGlobals(*compiled.program,globals,{VisualValue(1.0)}).has_value(),
      "Recursive callback reachability did not converge.");
  globals["nested callback"]=VisualValue(VisualProcedure{"selected"});
  const auto argumentCode=CompileMappedProgram(p.graph,"argument dispatch");
  Check(argumentCode.program.has_value(),"Argument callback fixture did not compile.");
  const auto incoming=list(VisualValue(VisualProcedure{"bridge"}));
  const auto argumentMask=ResolveDynamicInitialGlobals(*argumentCode.program,globals,{incoming});
  Check(argumentMask && reads(*argumentCode.program,*argumentMask,"required") &&
      !reads(*argumentCode.program,*argumentMask,"unrelated payload"),"A callback in an incoming collection was missed.");
  auto argumentResolved=*argumentCode.program; argumentResolved.initialGlobalReads=*argumentMask;
  const auto applied=ExecuteHostBytecode(argumentResolved,host,origin,globals,incoming,1,{},storage);
  Check(!applied.needsInterpreter && std::get<double>(applied.value.data)==22,"Incoming callback semantics changed.");
  VisualValue shared(VisualProcedure{"selected"});
  for (unsigned depth=0;depth<40;++depth) shared=VisualValue(std::make_shared<const VisualList>(VisualList{shared,shared}));
  Check(ResolveDynamicInitialGlobals(*argumentCode.program,globals,{shared}).has_value(),"Shared callback collections expanded exponentially.");
  globals["nested callback"]=VisualValue(VisualProcedure{"missing"});
  Check(!ResolveDynamicInitialGlobals(*compiled.program,globals,{VisualValue(1.0)}),"Unknown callback targets were guessed.");
  globals["nested callback"]=VisualValue(VisualProcedure{"selected"});
  auto missing=*compiled.program; missing.procedureInitialGlobalReads.clear();
  Check(!ResolveDynamicInitialGlobals(missing,globals,{VisualValue(1.0)}),"Missing reachability metadata was trusted.");
  missing=*compiled.program; missing.initialGlobalReads.clear();
  Check(!ResolveDynamicInitialGlobals(missing,globals,{VisualValue(1.0)}),"Missing global-read proof was trusted.");
  {
    const auto large=VisualValue(std::make_shared<const VisualList>(1000000,VisualValue(0.0)));
    Check(!ResolveDynamicInitialGlobals(*argumentCode.program,globals,{large}),"Exhausted callback analysis did not stay conservative.");
  }
  Program uniform;
  uniform.define("constant","",{uniform.block("procedures/return",{{"value",uniform.num(7)}})});
  uniform.define("unused","",{uniform.block("procedures/return",{{"value",uniform.get("unrelated payload")}})});
  uniform.define("invoke","index",{uniform.block("procedures/return",{{"value",uniform.block("procedures/apply",
      {{"function",uniform.get("callback")},{"arguments",uniform.block("data/list")}})}})});
  const auto uniformCode=CompileMappedProgram(uniform.graph,"invoke");
  Check(uniformCode.program.has_value(),"Dynamic uniform fixture did not compile.");
  globals["callback"]=VisualValue(VisualProcedure{"constant"});
  const auto proof=TryUniformProgram(*uniformCode.program,origin,globals,{VisualValue(1.0),VisualValue(2.0)},{},storage);
  Check(proof && proof->values.size()==2 && std::get<double>(proof->values.front().data)==7,
      "Unreachable dynamic definitions prevented a valid uniform proof.");
  std::cout << "PASS dynamic callback globals, incoming collections, recursion, shared values and conservative recovery\n";
}

void CompiledErrorParity() {
  for (const auto kind : {"type","horizon","unset","arguments","map-type","map-list","map-workers","map-arguments"}) {
    Program p;
    p.define("callback","value",{p.block("procedures/return",{{"value",p.get("value")}})});
    p.define("no arguments","",{p.block("procedures/return",{{"value",p.num(0)}})});
    Id value;
    if (std::string(kind).rfind("map-",0)==0) {
      const auto function=std::string(kind)=="map-type" ? p.get("bad function") :
          p.reference(std::string(kind)=="map-arguments" ? "no arguments" : "callback");
      const auto items=std::string(kind)=="map-type" ? p.get("missing") :
          std::string(kind)=="map-list" ? p.get("bad function") : p.block("data/list");
      const auto workers=std::string(kind)=="map-list" ? p.get("missing") :
          p.num(std::string(kind)=="map-workers" ? 0 : 2);
      value=p.block("procedures/map",{{"function",function},{"list",items},{"workers",workers}});
    }
    else if (std::string(kind)=="type") value=p.block("procedures/apply",{{"function",p.get("bad function")},{"arguments",p.get("missing")}});
    else if (std::string(kind)=="arguments") value=p.block("procedures/apply",{{"function",p.reference("callback")},{"arguments",p.block("data/list")}});
    else value=p.get("missing");
    p.define("error branch","item",{
        p.repeat(p.num(std::string(kind)=="horizon" ? 3 : 1),{p.block("simulation/step")}),
        p.block("procedures/return",{{"value",value}})});
    p.start({p.set("bad function",p.num(1)),p.block("flow/try",{},{{"name","caught"}},{{"body",{
        p.set("values",p.block("procedures/map",{{"function",p.reference("error branch")},
            {"list",p.block("data/numbers",{{"to",p.num(2)}})}}))}}})});
    VisualValue expected;
    for (bool compiled : {false,true}) {
      TestHost host; VisualRuntimeControl control; control.compilePrograms=compiled; control.horizonMs=20;
      const auto result=ExecuteVisualProgram(p.graph,host,control);
      Check(result.variables.count("caught"),"Mapped error disappeared.");
      if (!compiled) expected=result.variables.at("caught");
      else Check(EquivalentValue(expected,result.variables.at("caught")),"Compiled branch changed observable source error: "+std::string(kind));
      Check(host.state.timeMs==0,"Failing map changed its parent state.");
    }
  }
  std::cout << "PASS exact caught source errors after compiled type, horizon and argument failures\n";
}

void CompiledEventClock() {
  for (bool escape : {false,true}) {
    Program p;
    const auto emitted=[&] { return p.block("simulation/read",{{"state",p.block("events/state")}},{{"property","time"}}); };
    const auto record=[&] { return p.set("log",p.block("data/append",{{"list",p.get("log")},{"value",emitted()}})); };
    p.event("events/on-tick",{record(),
        p.branch(p.binary("conditions/equal",p.block("simulation/time"),p.num(10)),{p.block("simulation/step")}),record(),
        p.set("log",p.block("data/append",{{"list",p.get("log")},{"value",p.block("simulation/read",
            {{"state",p.block("events/state")}},{{"property","position"}})}}))});
    if (escape) p.event("events/on-tick",{p.set("escaped",p.block("events/state"))});
    p.define("unreachable state reader","",{p.block("procedures/return",{{"value",p.block("events/state")}})});
    p.define("clock branch","item",{p.set("log",p.block("data/list")),p.repeat(p.get("item"),{p.block("simulation/step")}),
        p.block("procedures/return",{{"value",p.get("log")}})});
    p.start({p.input(p.ms(0),p.num(65536)),p.set("values",p.block("procedures/map",{{"function",p.reference("clock branch")},
        {"list",p.block("data/numbers",{{"to",p.num(3)}})}}))});
    const auto code=CompileMappedProgram(p.graph,"clock branch");
    Check(code.program.has_value(),"Clock event branch failed compilation.");
    VisualValue expected;
    for (bool compiled : {false,true}) {
      TestHost host; VisualRuntimeControl control; control.compilePrograms=compiled;
      const auto result=ExecuteVisualProgram(p.graph,host,control);
      if (!compiled) expected=result.variables.at("values");
      else Check(EquivalentValue(expected,result.variables.at("values")),"Deferred event clock changed nested emitted times.");
    }
  }
  std::cout << "PASS deferred event clocks, reentrant captures and conservative state escape\n";
}

Program MappedBranches() {
  Program p;
  p.define("branch","steering",{
      p.set("shared",p.num(999)),p.input(p.ms(0),p.get("steering")),p.block("simulation/step"),
      p.block("procedures/return",{{"value",p.block("data/append",{
          {"list",p.block("data/append",{{"value",p.block("simulation/snapshot")}})},
          {"value",p.block("math/random-integer",{{"a",p.num(0)},{"b",p.num(1000000)}})}})}})});
  p.start({p.set("shared",p.num(42)),p.block("math/seed",{{"value",p.num(123)}}),
      p.set("branches",p.block("procedures/map",{{"function",p.reference("branch")},{"list",p.block("data/numbers",{
          {"from",p.num(-65536)},{"to",p.num(65536)},{"step",p.num(65536)}})}})),
      p.set("sum",p.num(0)),p.block("flow/for-each",{{"list",p.block("data/numbers",{{"to",p.num(4)}})}},{{"name","n"}},
          {{"body",{p.block("data/change",{{"value",p.get("n")}},{{"name","sum"}})}}})});
  return p;
}

void PartialBatchFallback() {
  struct Partial final : VisualBatchExecutor {
    std::optional<BytecodeBatchResult> execute(const ProgramBytecode &,
        const std::shared_ptr<const VisualSnapshot> &,const std::map<std::string,VisualValue> &,
        const VisualList &arguments,const std::vector<std::uint64_t> &,const VisualRuntimeControl &) override {
      BytecodeBatchResult result;
      for (const auto &argument : arguments) result.values.emplace_back(std::get<double>(argument.data)*10);
      result.values[2]=VisualValue{}; result.fallbackLanes={2};
      return result;
    }
  };
  Program p;
  p.define("mixed branch","item",{p.block("simulation/step"),
      p.block("procedures/return",{{"value",p.binary("math/multiply",p.get("item"),p.num(10))}})});
  p.start({p.set("values",p.block("procedures/map",{{"function",p.reference("mixed branch")},
      {"list",p.block("data/numbers",{{"to",p.num(5)}})}}))});
  for (unsigned workers : {1u,3u}) {
    TestHost host; host.advances=std::make_shared<std::atomic_uint>(0); host.batch=std::make_shared<Partial>();
    VisualRuntimeControl control; control.workerCount=workers;
    std::string mode; control.executionModeChanged=[&](const auto &value) { mode=value; };
    const auto result=ExecuteVisualProgram(p.graph,host,control);
    const auto values=std::get<std::shared_ptr<const VisualList>>(result.variables.at("values").data);
    for (std::size_t i=0;i<values->size();++i) Check(std::get<double>((*values)[i].data)==(i+1)*10,"Partial batch lost an ordered result.");
    Check(host.advances->load()==1,"One fallback lane replayed already committed accelerator work.");
    Check(mode=="CUDA block-program kernel with source fallback: 1/5 lanes","Partial fallback was not reported.");
    Check(host.state.timeMs==0,"Partial fallback changed the parent simulation.");
  }
  std::cout << "PASS partial batch fallback preserves completed lanes and parent state\n";
}

void ParallelBranches() {
  const auto p=MappedBranches();
  TestHost serialHost, parallelHost;
  parallelHost.probe=std::make_shared<TestHost::Probe>();
  VisualRuntimeControl serial, parallel; parallel.workerCount=3;
  const auto a=ExecuteVisualProgram(p.graph,serialHost,serial), b=ExecuteVisualProgram(p.graph,parallelHost,parallel);
  const auto branches=[](const auto &result) { return std::get<std::shared_ptr<const VisualList>>(result.variables.at("branches").data); };
  Check(Number(a,"shared")==42 && Number(b,"shared")==42 && Number(b,"sum")==10,"Mapped branches changed parent variables.");
  Check(serialHost.state.timeMs==0 && parallelHost.state.timeMs==0 && parallelHost.events.empty(),"A mapped job changed the parent simulation.");
  Check(parallelHost.probe->peak.load()>1,"Independent branches were not actually executed concurrently.");
  for (std::size_t i=0; i<3; ++i) {
    const auto x=std::get<std::shared_ptr<const VisualList>>((*branches(a))[i].data);
    const auto y=std::get<std::shared_ptr<const VisualList>>((*branches(b))[i].data);
    const auto sx=std::get<std::shared_ptr<const VisualSnapshot>>((*x)[0].data);
    const auto sy=std::get<std::shared_ptr<const VisualSnapshot>>((*y)[0].data);
    Check(sx->state.car.position.x==static_cast<float>(i)-1 && sy->state.car.position.x==sx->state.car.position.x,
          "Parallel map reordered or contaminated candidate branches.");
    Check(std::get<double>((*x)[1].data)==std::get<double>((*y)[1].data),"Map random draws depend on thread scheduling.");
  }
  Program capped;
  capped.define("advance","item",{capped.block("simulation/step"),
      capped.block("procedures/return",{{"value",capped.get("item")}})});
  capped.start({capped.set("jobs",capped.block("procedures/map",{
      {"function",capped.reference("advance")},
      {"list",capped.block("data/numbers",{{"to",capped.num(8)}})},
      {"workers",capped.num(256)}}))});
  TestHost cappedHost; cappedHost.probe=std::make_shared<TestHost::Probe>();
  VisualRuntimeControl cappedControl; cappedControl.workerCount=2;
  ExecuteVisualProgram(capped.graph,cappedHost,cappedControl);
  Check(cappedHost.probe->peak.load()==2,
        "An explicit map worker count bypassed the configured runtime worker limit.");
  cappedHost.capacity=1;
  cappedHost.probe=std::make_shared<TestHost::Probe>();
  ExecuteVisualProgram(capped.graph,cappedHost,cappedControl);
  Check(cappedHost.probe->peak.load()==1,"Map fallback exceeded its host's safe concurrency.");
  parallel.operationLimit=20;
  Fails(p,"operation limit",parallel);
  Program failed;
  failed.define("fail","item",{failed.block("procedures/return",{{"value",failed.binary("math/divide",failed.num(1),failed.num(0))}})});
  failed.start({failed.set("items",failed.block("procedures/map",{{"function",failed.reference("fail")},{"list",failed.block("data/numbers")}}))});
  parallel.operationLimit=0;
  Fails(failed,"Division by zero",parallel);
  Program changingHorizon;
  changingHorizon.define("shorten","item",{changingHorizon.block("simulation/restart"),
      changingHorizon.block("simulation/set-horizon",{{"time",changingHorizon.ms(10)}}),
      changingHorizon.block("procedures/return",{{"value",changingHorizon.num(1)}})});
  changingHorizon.start({changingHorizon.block("simulation/step",{{"ticks",changingHorizon.num(10)}}),
      changingHorizon.set("results",changingHorizon.block("procedures/map",{
          {"function",changingHorizon.reference("shorten")},
          {"list",changingHorizon.block("data/numbers",{{"to",changingHorizon.num(2)}})}}))});
  TestHost horizonHost;
  ExecuteVisualProgram(changingHorizon.graph,horizonHost);
  Check(horizonHost.state.timeMs==100 && horizonHost.state.durationMs==6000,
        "A serial mapped job's shortened horizon prevented restoring the next branch or parent.");
}

void InteractiveDebugger() {
  Program p;
  p.define("advance","ticks",{p.block("simulation/step",{{"ticks",p.get("ticks")}}),
      p.block("procedures/return",{{"value",p.block("simulation/time")}})});
  p.start({p.set("answer",p.call("advance","ticks",{p.num(3)})),p.change("answer",1)});
  auto debugger=std::make_shared<VisualDebugger>();
  debugger->enable(true); debugger->begin(true);
  std::vector<VisualDebugSnapshot> observations;
  debugger->setObserver([&](const auto &snapshot) {
    observations.push_back(snapshot);
    if (snapshot.paused) {
      const auto *node=p.graph.find(snapshot.block);
      debugger->resume(node && node->definitionId=="simulation/step"
          ? VisualDebugger::Step::Tick : VisualDebugger::Step::Into);
    }
  });
  VisualRuntimeControl control; control.debugger=debugger;
  TestHost host;
  const auto result=ExecuteVisualProgram(p.graph,host,control);
  Check(Number(result,"answer")==31 && observations.back().finished,"Debugger changed the program's result or missed completion.");
  bool locals=false, tick=false;
  for (const auto &point : observations) {
    if (!point.frames.empty() && point.frames.back().locals.count("ticks")) locals=true;
    if (point.state.timeMs==20) tick=true;
  }
  Check(locals && tick,"Debug snapshots omit call-local variables or intermediate ticks.");

  Id stepBlock=0;
  for (const auto &[id,node] : p.graph.nodes) if (node.definitionId=="flow/repeat") stepBlock=id;
  debugger->begin(false); debugger->setBreakpoints({stepBlock});
  std::vector<VisualDebugSnapshot> stepped;
  debugger->setObserver([&](const auto &point) {
    if (!point.paused) return;
    stepped.push_back(point);
    if (stepped.size()==1) debugger->resume(VisualDebugger::Step::Over);
    else if (stepped.size()==2) debugger->resume(VisualDebugger::Step::Out);
    else debugger->resume();
  });
  TestHost steppingHost;
  ExecuteVisualProgram(p.graph,steppingHost,control);
  Check(stepped.size()==3 && stepped[0].block==stepBlock && stepped[1].state.timeMs==30 &&
        p.graph.find(stepped[1].block)->definitionId=="procedures/return" &&
        p.graph.find(stepped[2].block)->definitionId=="data/change",
        "Breakpoint, Step Over, or Step Out stopped inside a completed block instead of the requested boundary.");
  debugger->setBreakpoints({});

  Program endless; endless.start({endless.block("flow/forever")});
  std::atomic_bool paused{false}, stop{false};
  debugger->begin(true);
  debugger->setObserver([&](const auto &point) { if (point.paused) paused.store(true); });
  control.stopRequested=[&] { return stop.load(); };
  auto execution=std::async(std::launch::async,[&] { TestHost h; return ExecuteVisualProgram(endless.graph,h,control); });
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while (!paused.load() && std::chrono::steady_clock::now()<until) std::this_thread::yield();
  stop.store(true);
  Check(execution.wait_for(std::chrono::seconds(2))==std::future_status::ready,"Stop could not wake a paused interpreter.");
  Check(paused.load() && execution.get().stopped,"Debug pause/cancellation did not reach an empty loop.");
}

void HigherOrderAndRecovery() {
  Program p;
  p.define("double","n",{p.block("procedures/return",{{"value",p.binary("math/multiply",p.get("n"),p.num(2))}})});
  p.define("transform","function, n",{p.block("procedures/return",{{"value",p.block("procedures/apply",{
      {"function",p.get("function")},{"arguments",p.block("data/append",{{"value",p.get("n")}})}})}})});
  p.start({p.set("operation",p.reference("double")),
      p.set("answer",p.call("transform","function, n",{p.get("operation"),p.num(21)})),
      p.block("flow/try",{},{},{{"body",{p.set("bad",p.binary("math/divide",p.num(1),p.num(0)))}},
          {"error",{p.set("recovered",p.flag(true))}}}),
      p.block("simulation/step",{{"ticks",p.num(4)}}),
      p.set("equal lists",p.binary("conditions/equal",p.block("data/append",{{"value",p.block("targets/point")}}),
          p.block("data/append",{{"value",p.block("targets/point")}}))),
      p.set("different types",p.binary("conditions/equal",p.num(0),p.block("values/none")))});
  TestHost host; host.finishAtMs=20;
  const auto result=ExecuteVisualProgram(p.graph,host);
  Check(Number(result,"answer")==42,"A user-defined operation could not receive and invoke another block as a value.");
  Check(std::get<bool>(result.variables.at("equal lists").data) && !std::get<bool>(result.variables.at("different types").data),
        "Generic equality rejected structured values or confused absent values with zero.");
  Check(std::get<bool>(result.variables.at("recovered").data) &&
        std::get<std::string>(result.variables.at("error").data).find("Division by zero")!=std::string::npos,
        "Error handling did not expose the block diagnostic to the program.");
  Check(host.state.timeMs==40 && host.state.raceCompleted,"Finishing the race still forcibly stops sandbox physics.");
}

VisualProgram Macro(const std::string &name) {
  for (const auto &macro : VisualMacroCatalog()) if (macro.id==name) return macro.program;
  throw std::runtime_error("Missing macro: "+name);
}

// Equivalent to replacing an initializer reporter in the editor. Remove only
// the old expression's unreferenced children, retaining the rest of the graph.
void MacroValue(VisualProgram &program, const std::string &name, const std::string &value,
                const std::string &type="values/number") {
  bool found=false;
  for (auto &[id,node] : program.nodes) {
    (void)id;
    if (node.definitionId=="data/local" && node.fields.at("name")==name) {
      auto &expression=*program.find(node.inputs.at("value"));
      expression.definitionId=type; expression.fields={{"value",value}};
      expression.inputs.clear(); expression.statements.clear(); found=true;
      break;
    }
  }
  Check(found,"No macro parameter: "+name);
  std::set<Id> reachable;
  std::function<void(Id)> visit=[&](Id id) {
    if (!reachable.insert(id).second) return;
    const auto &node=*program.find(id);
    for (const auto &[key,child] : node.inputs) { (void)key; visit(child); }
    for (const auto &[key,children] : node.statements) { (void)key; for (auto child : children) visit(child); }
  };
  for (auto id : program.topLevel) visit(id);
  for (auto it=program.nodes.begin(); it!=program.nodes.end();)
    if (!reachable.count(it->first)) it=program.nodes.erase(it); else ++it;
}

void MacroFlag(VisualProgram &program,const std::string &name,bool value) {
  MacroValue(program,name,value ? "true" : "false","values/boolean");
}

SandboxInputEvent Input(std::int32_t userTime,SandboxInputAction action,int value) {
  using namespace forevervalidator::experimental;
  SandboxInputEvent result;
  result.timeMs=userTime+10; result.action=action;
  if (action==SandboxInputAction::Steer || action==SandboxInputAction::Gas) {
    result.value.kind=PhysicsSandboxInputValueKind::Analog; result.value.analog=value;
  } else {
    result.value.kind=PhysicsSandboxInputValueKind::Switch;
    result.value.switchState=value ? PhysicsSandboxSwitchState::Pressed : PhysicsSandboxSwitchState::Released;
  }
  return result;
}

bool SameInputs(const VisualInputs &a,const VisualInputs &b) {
  return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),SameInputEvent);
}

void ExactInputPrimitives() {
  using A=SandboxInputAction;
  TestHost host;
  host.events={Input(0,A::Steer,100),Input(10,A::Accelerate,1),Input(20,A::Steer,200),Input(30,A::Brake,1)};
  const auto original=host.events;
  Program p;
  p.start({p.set("changed",p.block("inputs/with-value",{{"index",p.num(1)},{"value",p.num(300)}})),
    p.set("moved",p.block("inputs/with-time",{{"index",p.num(3)},{"time",p.ms(0)}})),
    p.set("removed",p.block("inputs/remove",{{"index",p.num(2)}})),
    p.set("upserted",p.block("inputs/set",{{"time",p.ms(20)},{"value",p.num(400)}})),
    p.set("held",p.block("inputs/value-at",{{"time",p.ms(10)}}))});
  const auto result=ExecuteVisualProgram(p.graph,host);
  auto expected=original; expected[0].value.analog=300;
  Check(SameInputs(std::get<VisualInputs>(result.variables.at("changed").data),expected),"One value edit changed another event.");
  expected=original; expected[2].timeMs=10;
  Check(SameInputs(std::get<VisualInputs>(result.variables.at("moved").data),expected),"Moving one timestamp also sorted or deleted another event.");
  expected=original; expected.erase(expected.begin()+1);
  Check(SameInputs(std::get<VisualInputs>(result.variables.at("removed").data),expected),"Delete-one removed more than the selected record.");
  expected=original; expected[2].value.analog=400;
  Check(SameInputs(std::get<VisualInputs>(result.variables.at("upserted").data),expected),"Upsert altered unrelated records.");
  Check(Number(result,"held")==100 && SameInputs(host.events,original),"Input value reporters changed the simulation or read the wrong timeline.");
  Program unsorted;
  unsorted.start({unsorted.block("simulation/replace-inputs",{{"inputs",unsorted.block("inputs/with-time",{
      {"index",unsorted.num(1)},{"time",unsorted.ms(40)}})}})});
  try { ExecuteVisualProgram(unsorted.graph,host); Check(false,"Applying inputs silently sorted the sequence."); }
  catch (const std::runtime_error &error) { Check(std::string(error.what()).find("Sort inputs")!=std::string::npos,error.what()); }
}

void GeometryAndEmptyRanges() {
  Program p;
  p.start({p.set("yaw",p.block("targets/rotation",{{"yaw",p.num(90)}})),
    p.set("pitch",p.block("targets/rotation",{{"pitch",p.num(90)}})),
    p.set("roll",p.block("targets/rotation",{{"roll",p.num(90)}})),
    p.set("empty",p.block("data/numbers",{{"from",p.num(1)},{"to",p.num(0)},{"step",p.num(1)}}))});
  TestHost host;
  const auto result=ExecuteVisualProgram(p.graph,host);
  const double half=std::sqrt(0.5);
  const auto yaw=std::get<VisualRotation>(result.variables.at("yaw").data);
  const auto pitch=std::get<VisualRotation>(result.variables.at("pitch").data);
  const auto roll=std::get<VisualRotation>(result.variables.at("roll").data);
  Check(std::abs(yaw.y-half)<1e-12 && yaw.x==0 && yaw.z==0 && std::abs(pitch.x-half)<1e-12 &&
        pitch.y==0 && std::abs(roll.z-half)<1e-12 && roll.y==0,"Target rotation axes disagree with the viewer's car pose.");
  Check(std::get<std::shared_ptr<const VisualList>>(result.variables.at("empty").data)->empty(),"Empty input-index ranges are not empty lists.");
}

void SkyPresetConfiguration() {
  auto program = Macro("sky");
  const std::map<std::string, std::string> expected{
    {"Pass 1 / first ms","6100"}, {"Pass 1 / last ms","10500"}, {"Pass 1 / seed","3749268317"},
    {"Pass 1 / delete steering","true"}, {"Pass 1 / maximum steering deletions","12"},
    {"Pass 1 / delete accelerate","true"}, {"Pass 1 / maximum accelerate deletions","1"},
    {"Pass 1 / delete brake","true"}, {"Pass 1 / maximum brake deletions","2"},
    {"Pass 2 / first ms","6100"}, {"Pass 2 / last ms","10500"}, {"Pass 2 / seed","444721321"},
    {"Pass 2 / minimum edits","1"}, {"Pass 2 / maximum edits","12"}, {"Pass 2 / maximum shift ms","0"},
    {"Pass 2 / absolute steering","false"}, {"Pass 2 / minimum steering","-1"}, {"Pass 2 / maximum steering","1"},
    {"Pass 2 / minimum absolute steering","-1"}, {"Pass 2 / maximum absolute steering","1"},
    {"Pass 2 / toggle accelerate","true"}, {"Pass 2 / toggle brake","true"},
    {"Pass 3 / first ms","6100"}, {"Pass 3 / last ms","10500"}, {"Pass 3 / seed","4221481885"},
    {"Pass 3 / insert steering","true"}, {"Pass 3 / steering is offset","true"},
    {"Pass 3 / minimum steering insertions","0"}, {"Pass 3 / maximum steering insertions","5"},
    {"Pass 3 / maximum steering hold ms","0"}, {"Pass 3 / minimum steering","-1"}, {"Pass 3 / maximum steering","1"},
    {"Pass 3 / minimum steering offset","-1"}, {"Pass 3 / maximum steering offset","1"},
    {"Pass 3 / insert accelerate","true"}, {"Pass 3 / minimum accelerate insertions","0"},
    {"Pass 3 / maximum accelerate insertions","1"}, {"Pass 3 / maximum accelerate hold ms","200"},
    {"Pass 3 / insert brake","true"}, {"Pass 3 / minimum brake insertions","0"},
    {"Pass 3 / maximum brake insertions","2"}, {"Pass 3 / maximum brake hold ms","300"}
  };
  for (const auto &setting : expected) {
    const auto &name = setting.first;
    const auto &value = setting.second;
    const auto found = std::find_if(program.nodes.begin(), program.nodes.end(), [&](const auto &entry) {
      return entry.second.definitionId == "data/local" && entry.second.fields.at("name") == name;
    });
    Check(found != program.nodes.end(), "Missing photographed setting: " + name);
    const auto &literal = *program.find(found->second.inputs.at("value"));
    Check(literal.inputs.empty() && literal.fields.at("value") == value,
          "Sky's does not reproduce the screenshot's literal: " + name);
  }
  const auto &body = program.find(program.topLevel.front())->statements.at("body");
  std::vector<std::string> sections;
  for (auto id : body) if (program.find(id)->definitionId == "flow/section") {
    sections.push_back(program.find(id)->fields.at("name"));
    Check(program.find(id)->collapsed, "Photographed passes should start folded.");
  }
  Check(sections == std::vector<std::string>{"Pass 1 · Input deletion", "Pass 2 · Existing-event perturbation", "Pass 3 · Input insertion"},
        "Photographed input pass order changed.");
  TestHost host;
  host.events = {Input(0,SandboxInputAction::Accelerate,1), Input(6000,SandboxInputAction::Steer,100), Input(10600,SandboxInputAction::Brake,1)};
  for (int time=6100; time<=10500; time+=100) {
    host.events.push_back(Input(time,SandboxInputAction::Steer,time));
    host.events.push_back(Input(time,SandboxInputAction::Accelerate,(time/100)%2));
    host.events.push_back(Input(time,SandboxInputAction::Brake,1-(time/100)%2));
  }
  std::stable_sort(host.events.begin(),host.events.end(),[](const auto &a,const auto &b) { return a.timeMs<b.timeMs; });
  const auto original = host.events;
  TestHost repeat=host;
  VisualRuntimeControl control; control.horizonMs=11000;
  ExecuteVisualProgram(program,host,control);
  ExecuteVisualProgram(program,repeat,control);
  Check(SameInputs(host.events,repeat.events) && !SameInputs(host.events,original), "Sky's is not repeatable or did not mutate its active window.");
  for (const auto &event : original) if (event.timeMs<6110 || event.timeMs>10510)
    Check(std::any_of(host.events.begin(),host.events.end(),[&](const auto &other) {return SameInputEvent(event,other);}), "Sky's changed an event outside the photographed window.");
  for (const auto &event : host.events) if (event.action==SandboxInputAction::Steer)
    Check(event.value.analog>=-65536 && event.value.analog<=65536, "Normalized steering was not converted to valid analog units.");
  const auto countId = program.nodes.rbegin()->first+1;
  VisualNode count; count.id=countId; count.definitionId="results/count";
  program.nodes.emplace(countId,std::move(count));
  program.find(program.topLevel.front())->statements.at("body").insert(program.find(program.topLevel.front())->statements.at("body").begin(),countId);
  repeat.events=original;
  ExecuteVisualProgram(program,repeat,control);
  Check(!SameInputs(host.events,repeat.events), "Preset repeats the same random mutation for every candidate.");
  std::cout << "PASS Sky's: all 42 photographed settings, ordered passes, active-window mutations and repeatable candidate seeds\n";
}

bool EquivalentValue(const VisualValue &a,const VisualValue &b) {
  if (a.data.index()!=b.data.index()) return false;
  return std::visit([&](const auto &left) -> bool {
    using T=std::decay_t<decltype(left)>;
    const auto &right=std::get<T>(b.data);
    if constexpr (std::is_same_v<T,std::monostate>) return true;
    else if constexpr (std::is_same_v<T,VisualInputs>) return SameInputs(left,right);
    else if constexpr (std::is_same_v<T,VisualVector>) return left.x==right.x && left.y==right.y && left.z==right.z;
    else if constexpr (std::is_same_v<T,VisualRotation>) return left.x==right.x && left.y==right.y && left.z==right.z && left.w==right.w;
    else if constexpr (std::is_same_v<T,VisualRange>) return left.minimum==right.minimum && left.maximum==right.maximum;
    else if constexpr (std::is_same_v<T,VisualProcedure>) return left.name==right.name;
    else if constexpr (std::is_same_v<T,VisualVolume>) return left.plane==right.plane && left.depth==right.depth && left.polygon==right.polygon &&
        EquivalentValue(VisualValue(left.origin),VisualValue(right.origin)) && EquivalentValue(VisualValue(left.size),VisualValue(right.size));
    else if constexpr (std::is_same_v<T,std::shared_ptr<const VisualList>>) {
      if (left->size()!=right->size()) return false;
      for (std::size_t i=0;i<left->size();++i) if (!EquivalentValue((*left)[i],(*right)[i])) return false;
      return true;
    } else if constexpr (std::is_same_v<T,VisualState>) {
      for (const auto &property : VisualStateProperties())
        if (!EquivalentValue(ReadVisualStateProperty(left,property.first),ReadVisualStateProperty(right,property.first))) return false;
      return left.finishTime==right.finishTime;
    } else if constexpr (std::is_same_v<T,std::shared_ptr<const VisualSnapshot>>) {
      return EquivalentValue(VisualValue(left->state),VisualValue(right->state)) &&
          EquivalentValue(VisualValue(left->previous),VisualValue(right->previous)) &&
          SameInputs(*left->inputs,*right->inputs) && left->horizonMs==right->horizonMs && left->candidate==right->candidate;
    } else return left==right;
  },a.data);
}

void CompiledValueBoundaries() {
  Program identity;
  identity.define("identity","item",{identity.block("procedures/return",{{"value",identity.get("item")}})});
  const auto code=CompileMappedProgram(identity.graph,"identity");
  Check(code.program.has_value(),"Could not compile identity procedure.");
  TestHost identityHost;
  auto origin=std::make_shared<VisualSnapshot>(VisualSnapshot{identityHost.capture(),identityHost.state,identityHost.state,
      std::make_shared<const VisualInputs>(),std::make_shared<VisualHistoryNode>(VisualHistoryNode{identityHost.state,{},1}),6000,0});
  VisualValue shared(17.0);
  for (unsigned depth=0;depth<36;++depth)
    shared=VisualValue(std::make_shared<const VisualList>(VisualList{shared,shared}));
  HostBytecodeStorage storage; storage.capacity=256u*1024u;
  const std::map<std::string,VisualValue> shadowed={{"item",VisualValue(VisualProcedure{"not in the compiled closure"})}};
  const auto shadowedResult=ExecuteHostBytecode(*code.program,identityHost,origin,shadowed,VisualValue(17.0),1,{},storage);
  Check(!shadowedResult.needsInterpreter && std::get<double>(shadowedResult.value.data)==17,
        "An unread parameter-shadowed global forced a compiled host fallback.");
  auto legacyCode=*code.program; legacyCode.initialGlobalReads.clear();
  Check(ExecuteHostBytecode(legacyCode,identityHost,origin,shadowed,VisualValue(17.0),1,{},storage).needsInterpreter,
        "Bytecode without a global-read proof did not conservatively import globals.");
  Program uniform;
  uniform.define("constant","item",{uniform.block("procedures/return",{{"value",uniform.num(7)}})});
  const auto uniformCode=CompileMappedProgram(uniform.graph,"constant");
  Check(uniformCode.program.has_value(),"Could not compile global-import uniform fixture.");
  const auto uniformResult=TryUniformProgram(*uniformCode.program,origin,shadowed,{VisualValue(1.0),VisualValue(2.0)},{},storage);
  Check(uniformResult && uniformResult->values.size()==2 && std::get<double>(uniformResult->values[0].data)==7,
        "An unread parameter-shadowed global prevented a uniform proof.");
  Program callee;
  callee.define("global reader","",{callee.block("procedures/return",{{"value",callee.get("item")}})});
  callee.define("scoped caller","item",{callee.block("procedures/return",{{"value",callee.call("global reader","",{})}})});
  const auto calleeCode=CompileMappedProgram(callee.graph,"scoped caller");
  Check(calleeCode.program && calleeCode.program->initialGlobalReads.size()==1 && calleeCode.program->initialGlobalReads[0],
        "A caller parameter hid a callee's initial global read.");
  const auto calleeResult=ExecuteHostBytecode(*calleeCode.program,identityHost,origin,{{"item",VisualValue(17.0)}},
      VisualValue(42.0),1,{},storage);
  Check(!calleeResult.needsInterpreter && std::get<double>(calleeResult.value.data)==17,
        "A callee read its caller's parameter instead of the global.");
  Program change;
  change.define("change only","item",{change.change("counter"),change.block("procedures/return",{{"value",change.num(0)}})});
  const auto changeCode=CompileMappedProgram(change.graph,"change only");
  Check(changeCode.program.has_value(),"Could not compile change-only global fixture.");
  Check(!ExecuteHostBytecode(*changeCode.program,identityHost,origin,{{"counter",VisualValue(7.0)}},
      VisualValue(1.0),1,{},storage).needsInterpreter,"A change-only global lost its initial numeric value.");
  const auto copied=ExecuteHostBytecode(*code.program,identityHost,origin,{},shared,1,{},storage);
  Check(!copied.needsInterpreter,"A compact shared value expanded beyond the compiled arena.");
  auto value=copied.value;
  for (unsigned depth=0;depth<36;++depth) {
    const auto list=std::get<std::shared_ptr<const VisualList>>(value.data);
    Check(list->size()==2,"A shared value lost a child.");
    if (depth<35)
      Check(std::get<std::shared_ptr<const VisualList>>((*list)[0].data)==std::get<std::shared_ptr<const VisualList>>((*list)[1].data),
          "Decoding duplicated a shared subtree.");
    value=(*list)[0];
  }
  Check(std::get<double>(value.data)==17,"Shared value leaf changed.");
  Program large;
  large.define("large value","item",{large.block("procedures/return",{{"value",
      large.block("data/length",{{"list",large.get("large global")}})}})});
  large.start({large.set("large global",large.block("data/numbers",{{"to",large.num(100000)}})),
      large.set("values",large.block("procedures/map",{{"function",large.reference("large value")},
          {"list",large.block("data/numbers",{{"to",large.num(2)}})}}))});
  for (bool compiled : {false,true}) {
    TestHost host; VisualRuntimeControl control; control.compilePrograms=compiled; control.workerCount=2;
    const auto result=ExecuteVisualProgram(large.graph,host,control);
    for (const auto &item : *std::get<std::shared_ptr<const VisualList>>(result.variables.at("values").data))
      Check(std::get<double>(item.data)==100000,"Initial arena exhaustion rejected valid source instead of falling back.");
  }
  Program p;
  const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
  p.define("check values","item",{p.block("procedures/return",{{"value",
      append(append(p.block("data/list"),p.binary("conditions/equal",p.get("saved"),p.get("alias"))),
        p.block("inputs/value-at",{{"inputs",p.block("simulation/inputs")},{"time",p.ms(0)},
          {"action",p.block("inputs/action-name",{},{{"value","steer"}})}}))}})});
  p.start({p.set("saved",p.block("simulation/snapshot")),p.set("alias",p.get("saved")),
      p.set("values",p.block("procedures/map",{{"function",p.reference("check values")},
          {"list",p.block("data/numbers",{{"from",p.num(1)},{"to",p.num(3)},{"step",p.num(1)}})}}))});
  for (bool compiled : {false,true}) {
    TestHost host; host.events={Input(0,SandboxInputAction::Steer,12345)}; host.events.front().timeMs=-10;
    VisualRuntimeControl control; control.compilePrograms=compiled; control.workerCount=3;
    const auto result=ExecuteVisualProgram(p.graph,host,control);
    const auto values=std::get<std::shared_ptr<const VisualList>>(result.variables.at("values").data);
    for (const auto &item : *values) {
      const auto pair=std::get<std::shared_ptr<const VisualList>>(item.data);
      Check(std::get<bool>((*pair)[0].data),"Compiled snapshot alias lost reference identity.");
      Check(std::get<double>((*pair)[1].data)==12345,"Compiled held-input lookup lost a negative-time event.");
    }
  }
  Program dynamic;
  dynamic.define("callback","",{dynamic.block("procedures/return",{{"value",dynamic.num(42)}})});
  dynamic.define("indirect branch","item",{dynamic.block("procedures/return",{{"value",
      dynamic.block("procedures/apply",{{"function",dynamic.get("function")},{"arguments",dynamic.block("data/list")}})}})});
  dynamic.start({dynamic.set("function",dynamic.reference("callback")),dynamic.set("values",dynamic.block("procedures/map",
      {{"function",dynamic.reference("indirect branch")},{"list",dynamic.block("data/numbers",{{"from",dynamic.num(1)},
          {"to",dynamic.num(3)},{"step",dynamic.num(1)}})}}))});
  TestHost host; VisualRuntimeControl control; control.workerCount=3;
  const auto dynamicCode=CompileMappedProgram(dynamic.graph,"indirect branch");
  Check(dynamicCode.program.has_value(),"Indirect procedure calls unexpectedly require source execution.");
  std::string dynamicMode;
  control.executionModeChanged=[&](const std::string &mode) { dynamicMode=mode; };
  const auto result=ExecuteVisualProgram(dynamic.graph,host,control);
  for (const auto &item : *std::get<std::shared_ptr<const VisualList>>(result.variables.at("values").data))
    Check(std::get<double>(item.data)==42,"Dynamic procedure value did not retain interpreter behavior.");
  Check(dynamicMode=="Compiled block program on CPU" || dynamicMode=="Uniform compiled block program",
        "Indirect calls silently fell back to the interpreter.");
  std::cout << "PASS compiled snapshot identity, negative-time inputs and dynamic procedure calls\n";
}

void ExecutorLifetimeAndCancellation() {
  ParallelExecutor executor;
  std::map<std::size_t,std::thread::id> identities;
  std::mutex mutex;
  for (const auto count : {1u,2u,4u,2u,1u,4u}) {
    std::atomic_uint calls{0};
    executor.run(count,[&](std::size_t worker) {
      ++calls;
      std::lock_guard<std::mutex> lock(mutex);
      const auto [it,inserted]=identities.emplace(worker,std::this_thread::get_id());
      Check(inserted || it->second==std::this_thread::get_id(),"A pool worker was recreated between maps.");
    });
    Check(calls==count,"Pool resizing lost or duplicated a worker.");
  }
  try {
    executor.run(4,[](std::size_t worker) { if (worker==2) throw std::runtime_error("worker failure"); });
    Check(false,"Pool swallowed a worker exception.");
  } catch (const std::runtime_error &error) { Check(std::string(error.what())=="worker failure","Wrong pool failure."); }
  std::atomic_uint recovered{0};
  executor.run(4,[&](std::size_t) { ++recovered; });
  Check(recovered==4,"Pool could not be reused after a failed batch.");

  struct Probe {
    unsigned calls=0,stopAfter=0;
    bool cancelled() { return stopAfter && ++calls>=stopAfter; }
    bool interpreterRequested() const { return false; }
    vm::Value apply(vm::Op,vm::Machine<Probe> &,vm::Value *,std::uint32_t) {
      throw std::runtime_error("Unexpected physics in collection test.");
    }
  };
  {
    const vm::Instruction code[]={{vm::Op::Load,0,1,11,{}},{vm::Op::Return,0,0,12,{}}};
    const vm::Procedure procedures[]={{0,0,1,1}};
    const std::uint32_t arguments[]={0};
    Probe probe;
    vm::Machine<Probe> machine({code,procedures,arguments,2,1,1},probe);
    std::vector<unsigned char> arena(4096);
    vm::Value stack[8];
    machine.memory.bytes=arena.data(); machine.memory.capacity=arena.size();
    machine.stack=stack; machine.stackCapacity=8;
    machine.begin(0,vm::number(42),7);
    Check(!machine.resume([](const vm::Instruction &) { return true; }) && machine.pc==0 && machine.operations==0,
        "Suspending before an instruction changed VM state.");
    Check(!machine.resume([](const vm::Instruction &instruction) { return instruction.op==vm::Op::Return; }) &&
        machine.pc==1 && machine.operations==1 && machine.stackSize==1,"Resuming lost a pending return value.");
    Check(machine.resume([](const vm::Instruction &) { return false; }),"Resumed VM did not finish.");
    const auto result=machine.result();
    Check(result.error==vm::Error::None && result.value.x==42 && result.operations==2 && result.source==12,
        "Suspension changed result or instruction accounting.");
  }
  {
    const vm::Instruction code[]={{vm::Op::End,0,0,1,{}}};
    const vm::Procedure procedures[]={{0,0,0,0}};
    Probe probe;
    vm::Machine<Probe> machine({code,procedures,nullptr,1,1,0},probe);
    vm::Value stack[1]; machine.stack=stack; machine.stackCapacity=1;
    machine.pc=machine.program.codeSize;
    const auto initialUsed=machine.memory.used;
    for (unsigned depth=0;depth<64;++depth)
      Check(machine.call(0,0,false) && !machine.locals.handle && machine.memory.used==initialUsed,
            "An empty call frame required arena storage.");
    Check(machine.resume([](const vm::Instruction &) { return false; }) && machine.frameCount==0 &&
          machine.error==vm::Error::None && machine.memory.used==initialUsed,
          "Allocation-free frames failed to unwind without arena storage.");
    for (unsigned depth=0;depth<64;++depth)
      Check(machine.call(0,0,false),"An empty call frame could not be reused.");
    Check(!machine.call(0,0,false) && machine.error==vm::Error::Recursion && machine.frameCount==64,
          "Allocation-free frames changed the recursion limit.");
  }
  for (bool cancel : {false,true}) {
    Probe probe; probe.stopAfter=cancel ? 2 : 0;
    vm::Machine<Probe> machine({},probe);
    std::vector<unsigned char> arena(4u*1024u*1024u);
    machine.memory.bytes=arena.data(); machine.memory.capacity=static_cast<std::uint32_t>(arena.size());
    auto inputs=machine.memory.allocate(vm::Kind::Inputs,8192);
    for (unsigned i=0;i<8192;++i) machine.memory.items(inputs)[i]={vm::Kind::Event,0,
        static_cast<double>((8191-i)/2),static_cast<double>(i)};
    const auto sorted=machine.builtin(vm::Op::SortInputs,&inputs,0);
    if (cancel) {
      Check(machine.error==vm::Error::Cancelled && probe.calls==2,"Sorting did not poll inside a long builtin.");
    } else {
      Check(machine.error==vm::Error::None,"Compiled stable sort failed.");
      const auto *items=machine.memory.items(sorted);
      for (unsigned i=1;i<8192;++i)
        Check(items[i-1].x<items[i].x || (items[i-1].x==items[i].x && items[i-1].y<items[i].y),
            "Compiled input sorting changed equal-time ordering.");
    }
  }
  Program infinite;
  infinite.define("arbitrary loop","item",{infinite.block("flow/forever")});
  infinite.start({infinite.set("values",infinite.block("procedures/map",{{"function",infinite.reference("arbitrary loop")},
      {"list",infinite.block("data/numbers",{{"to",infinite.num(4)}})}, {"workers",infinite.num(4)}}))});
  for (bool compiled : {false,true}) {
    TestHost host; VisualRuntimeControl control; control.compilePrograms=compiled; control.workerCount=4;
    std::atomic_uint polls{0};
    control.stopRequested=[&] { return ++polls>1000; };
    const auto result=ExecuteVisualProgram(infinite.graph,host,control);
    Check(result.stopped,"An empty mapped loop ignored cancellation.");
  }
  std::cout << "PASS pool reuse, resizing, failure recovery and compiled loop/collection cancellation\n";
}

void UniformProgramProofs() {
  TestHost host;
  auto origin=std::make_shared<VisualSnapshot>(VisualSnapshot{host.capture(),host.state,host.state,
      std::make_shared<const VisualInputs>(),std::make_shared<VisualHistoryNode>(VisualHistoryNode{host.state,{},1}),6000,0});
  HostBytecodeStorage storage;
  const VisualList arguments{VisualValue(0.0),VisualValue(1.0),VisualValue(12345.0)};
  for (int kind=0;kind<8;++kind) {
    Program p;
    Id output=p.num(42);
    std::vector<Id> body{p.block("results/add-count",{{"amount",p.get("item")}})};
    if (kind==0) output=p.binary("math/min",p.binary("math/random-integer",p.num(1),p.num(3)),p.num(0));
    if (kind==1) output=p.get("item");
    if (kind==2) body.push_back(p.set("unused",p.binary("math/divide",p.num(1),p.get("item"))));
    if (kind==3) output=p.binary("math/random-integer",p.num(1),p.num(3));
    if (kind==4) body.push_back(p.block("simulation/step"));
    if (kind==5) {
      body.push_back(p.block("math/seed",{{"value",p.num(7)}}));
      output=p.binary("math/random-integer",p.num(1),p.num(1000000));
    }
    if (kind==6) body.push_back(p.set("unused",p.binary("math/divide",p.num(1),p.num(0))));
    if (kind==7) output=p.block("data/append",{{"list",p.block("data/list")},{"value",p.get("item")}});
    body.push_back(p.block("procedures/return",{{"value",output}}));
    p.define("arbitrary proof candidate","item",std::move(body));
    const auto code=CompileMappedProgram(p.graph,"arbitrary proof candidate");
    Check(code.program.has_value(),"Could not compile proof test.");
    const auto uniform=TryUniformProgram(*code.program,origin,{},arguments,{},storage);
    Check(uniform.has_value()==(kind==0 || kind==5),"Uniform proof accepted varying values/effects/errors or rejected a valid proof.");
    if (uniform) for (const auto &argument : arguments) for (unsigned seed=0;seed<100;++seed) {
      host.restore(*origin->native);
      const auto concrete=ExecuteHostBytecode(*code.program,host,origin,{},argument,seed,{},storage);
      Check(EquivalentValue(concrete.value,uniform->values.front()),"Uniform proof disagreed with concrete execution.");
    }
  }
  std::cout << "PASS uniform proofs against concrete RNG streams; varying results, errors and physics stay concrete\n";
}

void HistoryReachability() {
  using vm::Op;
  const auto i=[](Op op,std::uint32_t target=0) { return vm::Instruction{op,target,0,0,{}}; };
  const auto verify=[](std::vector<vm::Instruction> code,std::vector<vm::Procedure> procedures,
      std::vector<std::uint8_t> expected) {
    ProgramBytecode program; program.code=std::move(code); program.procedures=std::move(procedures);
    Check(HistoryReadReachability(program)==expected,"History reachability lost a branch, call, loop or terminal boundary.");
    for (auto &instruction : program.code) if (instruction.op==Op::History) instruction.op=Op::Restore;
    Check(RestoreReachability(program)==expected,"Restore reachability lost a branch, call, loop or terminal boundary.");
  };
  verify({i(Op::History),i(Op::Step),i(Op::Return)},{{0}},{1,0,0});
  verify({i(Op::History),i(Op::Step),i(Op::Jump,0)},{{0}},{1,1,1});
  verify({i(Op::IfFalse,3),i(Op::History),i(Op::Return),i(Op::Step),i(Op::Return)},{{0}},{1,1,0,0,0});
  verify({i(Op::Call,1),i(Op::History),i(Op::Return),i(Op::Step),i(Op::Return)},{{0},{3}},{1,1,0,0,0});
  verify({i(Op::CallValue,1),i(Op::Step),i(Op::Return),i(Op::History),i(Op::Return)},{{0},{3}},{1,0,0,1,0});
  verify({i(Op::Call,1),i(Op::Return),i(Op::IfTrue,5),i(Op::Call,0),i(Op::Return),i(Op::History),i(Op::Return)},
      {{0},{2}},{1,0,1,1,0,1,0});
  verify({i(Op::Apply),i(Op::Return),i(Op::Do),i(Op::Return),i(Op::History),i(Op::Return)},{{0},{2},{4}},{1,0,1,0,1,0});
  verify({i(Op::Stop),i(Op::History),i(Op::Interpret),i(Op::History),i(Op::End),i(Op::History)},{{0}},{0,1,0,1,0,1});
  verify({i(Op::Apply),i(Op::Do),i(Op::Step),i(Op::Return)},{{0}},{0,0,0,0});
  ProgramBytecode contexts;
  contexts.code={i(Op::Restart),i(Op::Snapshot),i(Op::SetHorizon),i(Op::History),i(Op::Return)};
  Check(RestoreReachability(contexts)==std::vector<std::uint8_t>(contexts.code.size(),0),
      "Restart or horizon contexts were mistaken for a future snapshot restore.");
  std::cout << "PASS history and restore reachability: loops, calls, recursion, indirect targets and termination\n";
}

Program NestedMapFixture() {
  Program p;
  const auto list=[&](std::initializer_list<Id> values) {
    auto result=p.block("data/list");
    for (auto value : values) result=p.block("data/append",{{"list",result},{"value",value}});
    return result;
  };
  const auto map=[&](const char *function,Id items) {
    return p.block("procedures/map",{{"function",p.reference(function)},{"list",items},{"workers",p.num(2)}});
  };
  const auto random=[&]() { return p.binary("math/random-integer",p.num(0),p.num(1000000)); };
  const auto scope=[&]() { return list({p.get("counter"),p.block("results/iterations"),p.block("results/has-result"),
      p.block("events/value"),p.block("events/state"),p.block("runtime/workers")}); };
  p.define("read only map helper","item",{
      p.set("counter",p.num(99)),p.block("results/publish",{{"score",p.num(11)}}),
      p.block("procedures/return",{{"value",list({scope(),p.block("simulation/history"),random()})}})});
  p.define("read only map child","item",{
      p.block("procedures/return",{{"value",p.call("read only map helper","item",{p.get("item")})}})});
  p.define("unobserving map child","item",{
      p.block("procedures/return",{{"value",p.binary("math/add",p.get("item"),p.get("counter"))}})});
  p.define("snapshot only map child","item",{
      p.block("procedures/return",{{"value",p.block("simulation/snapshot")}})});
  p.event("events/on-tick",{p.change("counter")});
  p.define("map child","item",{
      p.set("before",scope()),p.set("draw",random()),
      p.input(p.binary("math/add",p.block("simulation/time"),p.ms(10)),p.binary("math/multiply",p.get("item"),p.num(1000))),
      p.repeat(p.get("item"),{p.block("simulation/step"),p.block("results/count")}),
      p.block("results/publish",{{"score",p.get("item")}}),
      p.block("procedures/return",{{"value",list({p.get("before"),scope(),p.get("draw"),random(),
          p.block("simulation/history"),p.block("simulation/inputs"),p.block("simulation/snapshot")})}})});
  p.define("map middle","item",{
      p.set("before",scope()),p.set("children",map("map child",list({p.num(1),p.num(2)}))),
      p.block("procedures/return",{{"value",list({p.get("before"),scope(),p.get("children"),random()})}})});
  p.event("events/on-message",{
      p.set("event before",p.block("events/state")),
      p.set("read only children",map("read only map child",list({p.num(1),p.num(2)}))),
      p.set("event children",map("map child",list({p.num(1),p.num(2)}))),
      p.set("event after",list({p.block("events/value"),p.block("events/state")}))}, {}, {{"name","nested"}});
  p.define("map parent","item",{
      p.block("math/seed",{{"value",p.num(77)}}),p.set("counter",p.num(17)),
      p.block("results/add-count",{{"amount",p.num(5)}}),p.block("results/publish",{{"score",p.num(7)}}),
      p.set("before",scope()),p.set("children",map("map middle",list({p.num(1),p.num(2)}))),
      p.set("empty",map("map child",p.block("data/list"))),
      p.set("pure children",map("unobserving map child",list({p.num(1),p.num(2)}))),
      p.set("snapshot children",map("snapshot only map child",list({p.num(1),p.num(2)}))),
      p.set("draw",random()),p.block("events/send",{
          {"message",p.block("values/text",{},{{"value","nested"}})},{"value",p.num(91)}}),
      p.block("procedures/return",{{"value",list({p.get("before"),scope(),p.get("children"),p.get("empty"),p.get("draw"),
          p.get("event before"),p.get("event after"),p.get("event children"),p.block("results/best-score"),
          p.block("results/snapshot"),p.block("simulation/history"),random(),p.get("read only children"),p.get("pure children"),
          p.get("snapshot children")})}})});
  p.start({p.set("counter",p.num(17)),p.set("values",map("map parent",list({p.num(1),p.num(2)})))});
  return p;
}

void CompiledNestedMaps() {
  auto p=NestedMapFixture();
  const auto code=CompileMappedProgram(p.graph,"map parent");
  Check(code.program.has_value() && std::none_of(code.program->code.begin(),code.program->code.end(),
      [](const auto &instruction) { return instruction.op==vm::Op::Interpret; }),"Nested maps retained interpreter instructions.");
  for (const auto *name : {"read only map helper","read only map child","map child","map middle"}) {
    const auto found=std::find(code.program->procedureNames.begin(),code.program->procedureNames.end(),name);
    Check(found!=code.program->procedureNames.end(),"Missing nested map effect-analysis procedure.");
    const auto effects=code.program->procedures[found-code.program->procedureNames.begin()].effects;
    const bool effect=(effects&vm::Procedure::Physics)!=0;
    Check(effect==(std::string(name).rfind("read only",0)!=0),"Nested map effect analysis missed a transitive physics change.");
    Check((effects&(vm::Procedure::RandomState|vm::Procedure::Globals))==(vm::Procedure::RandomState|vm::Procedure::Globals),
        "Nested map effect analysis lost transitive global writes or RNG observations.");
  }
  const auto pure=std::find(code.program->procedureNames.begin(),code.program->procedureNames.end(),"unobserving map child");
  Check(pure!=code.program->procedureNames.end() && code.program->procedures[pure-code.program->procedureNames.begin()].effects==0,
      "Read-only global lookup was mistaken for a child state effect.");
  const auto snapshot=std::find(code.program->procedureNames.begin(),code.program->procedureNames.end(),"snapshot only map child");
  Check(snapshot!=code.program->procedureNames.end() &&
      code.program->procedures[snapshot-code.program->procedureNames.begin()].effects==vm::Procedure::Results,
      "Snapshot capture did not isolate the child's candidate count.");
  VisualValue expected;
  for (bool compiled : {false,true}) {
    TestHost host;
    VisualRuntimeControl control; control.compilePrograms=compiled; control.workerCount=2;
    std::vector<std::string> modes;
    control.executionModeChanged=[&](const auto &mode) { modes.push_back(mode); };
    const auto result=ExecuteVisualProgram(p.graph,host,control);
    if (!compiled) expected=result.variables.at("values");
    else {
      Check(EquivalentValue(expected,result.variables.at("values")),
          "Nested maps changed isolated globals, results, random streams, event state, inputs, histories or snapshots.");
      Check(std::find(modes.begin(),modes.end(),"Compiled block program on CPU")!=modes.end() &&
          std::none_of(modes.begin(),modes.end(),[](const auto &mode) { return mode.find("interpreter")!=std::string::npos; }),
          "Nested map scope required the source interpreter.");
    }
  }
  std::cout << "PASS compiled nested maps: isolated scopes, all child seeds, events, empty maps and physics restoration\n";
}

void CompiledHistory() {
  for (const unsigned prefix : {0u,3u,130u}) {
    Program p;
    const auto history=[&]() { return p.block("simulation/history"); };
    const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
    auto output=p.block("data/list");
    for (const auto *name : {"retained","later","times","edited"}) output=append(output,p.get(name));
    output=append(output,history());
    p.define("history branch","steps",{
        p.set("retained",history()),p.set("saved",p.block("simulation/snapshot")),
        p.repeat(p.get("steps"),{p.block("simulation/step")}),p.set("later",history()),
        p.set("times",p.block("data/list")),
        p.block("flow/for-each",{{"list",p.get("later")}},{{"name","frame"}},{{"body",{
            p.set("times",append(p.get("times"),p.block("simulation/read",{{"state",p.get("frame")}},{{"property","time"}})))}}}),
        p.set("edited",append(p.get("retained"),p.block("simulation/state"))),
        p.block("simulation/restore",{{"snapshot",p.get("saved")}}),
        p.block("procedures/return",{{"value",output}})});
    p.start({p.repeat(p.num(prefix),{p.block("simulation/step")}),
        p.set("entry",p.block("simulation/snapshot")),
        p.set("got",p.call("history branch","steps",{p.num(131)}))});
    const auto code=CompileMappedProgram(p.graph,"history branch");
    Check(code.program.has_value() && std::any_of(code.program->code.begin(),code.program->code.end(),
        [](const auto &instruction) { return instruction.op==vm::Op::History; }) &&
        std::none_of(code.program->code.begin(),code.program->code.end(),
        [](const auto &instruction) { return instruction.op==vm::Op::Interpret; }),
        "History was not lowered to a generic VM operation.");
    TestHost source,native;
    VisualRuntimeControl control; control.compilePrograms=false;
    const auto expected=ExecuteVisualProgram(p.graph,source,control);
    const auto baseline=std::get<std::shared_ptr<const VisualSnapshot>>(expected.variables.at("entry").data);
    native.restore(*baseline->native); native.replaceInputs(*baseline->inputs);
    HostBytecodeStorage storage;
    const auto actual=ExecuteHostBytecode(*code.program,native,baseline,expected.variables,VisualValue(131.0),1,control,storage);
    Check(!actual.needsInterpreter && !actual.stopped && EquivalentValue(expected.variables.at("got"),actual.value),
        "Compiled history lost pending ticks, immutable lists, state properties or a restored prefix.");
    Check(!TryUniformProgram(*code.program,baseline,expected.variables,{VisualValue(131.0),VisualValue(131.0)},control,storage),
        "Uniform execution accepted physical history observations.");
    control.stopRequested=[] { return true; };
    native.restore(*baseline->native);
    const auto stopped=ExecuteHostBytecode(*code.program,native,baseline,expected.variables,VisualValue(131.0),1,control,storage);
    Check(stopped.stopped && !stopped.needsInterpreter,"History cancellation requested source fallback.");
    if (prefix==130) {
      Program reader;
      reader.define("read history","unused",{
          reader.block("procedures/return",{{"value",reader.block("simulation/history")}})});
      const auto readCode=CompileMappedProgram(reader.graph,"read history");
      Check(readCode.program.has_value(),"Could not compile history reader.");
      unsigned polls=0;
      control.stopRequested=[&] { return ++polls>3; };
      const auto interrupted=ExecuteHostBytecode(*readCode.program,native,baseline,{},VisualValue(0.0),1,control,storage);
      Check(interrupted.stopped && !interrupted.needsInterpreter && polls==4,
          "History materialization ignored cancellation between state encodings.");
      control.stopRequested={};
      HostBytecodeStorage bounded; bounded.capacity=32768;
      const auto exhausted=ExecuteHostBytecode(*readCode.program,native,baseline,{},VisualValue(0.0),1,control,bounded);
      Check(exhausted.needsInterpreter && !exhausted.stopped,
          "History arena exhaustion returned a truncated successful list.");
    }
  }
  std::cout << "PASS compiled history: immutable prefix, pending ticks, generic list traversal, restore and cancellation\n";
}

Program RestartFixture(std::uint32_t anchor,bool observeHistory,bool indirect) {
  Program p;
  const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
  const auto item=[&](Id list,Id index) { return p.block("data/item",{{"list",list},{"index",index}}); };
  const auto at=[&](const char *name,unsigned index) { return item(p.get(name),p.num(index)); };
  const auto horizon=[&](Id value) { return p.block("simulation/set-horizon",{{"time",value}}); };
  const auto equal=[&](Id a,Id b) { return p.binary("conditions/equal",a,b); };
  const auto require=[&](Id condition) { return p.branch(p.block("conditions/not",{{"value",condition}}),{
      p.set("invalid restart",p.binary("math/divide",p.num(1),p.num(0)))}); };
  const auto capture=[&](const char *name) {
    const auto snapshot=indirect ? p.call("restart snapshot helper","unused",{p.num(0)}) : p.block("simulation/snapshot");
    auto stage=append(append(p.block("data/list"),snapshot),p.block("simulation/previous-state"));
    if (observeHistory) stage=append(stage,p.block("simulation/history"));
    return p.set(name,stage);
  };
  p.define("restart snapshot helper","unused",{p.block("procedures/return",{{"value",p.block("simulation/snapshot")}})});
  p.define("restart helper","unused",{p.block("simulation/restart"),
      p.block("procedures/return",{{"value",p.num(0)}})});
  const auto restart=[&]() {
    return indirect ? p.block("procedures/do",{{"function",item(append(p.block("data/list"),p.reference("restart helper")),p.num(1))},
        {"arguments",append(p.block("data/list"),p.num(0))}}) : p.block("simulation/restart");
  };
  p.event("events/on-tick",{p.branch(p.get("restart armed"),{
      p.set("restart armed",p.flag(false)),p.set("event before restart",p.block("events/state")),restart(),
      horizon(p.binary("math/add",p.block("simulation/horizon"),p.ms(100))),
      p.set("event after restart",p.block("events/state"))})});
  auto row=append(p.block("data/list"),p.get("lane"));
  for (const auto *name : {"restart first","restart resized","restart event","restart advanced","restart restored","restart last",
                           "event before restart","event after restart"}) row=append(row,p.get(name));
  p.define("restart branch","lane",{
      p.set("restart armed",p.flag(false)),p.block("simulation/restore",{{"snapshot",p.get("restart entry")}}),
      p.input(p.ms(anchor+90),p.binary("math/multiply",p.get("lane"),p.num(7000))),
      p.repeat(p.num(2),{p.block("simulation/step")}),
      horizon(p.binary("math/add",p.ms(anchor),p.binary("math/multiply",p.get("lane"),p.num(2000)))),
      restart(),capture("restart first"),horizon(p.binary("math/add",p.block("simulation/horizon"),p.ms(100))),
      capture("restart resized"),p.set("restart armed",p.flag(true)),p.block("simulation/step"),capture("restart event"),
      p.repeat(p.num(131),{p.block("simulation/step")}),capture("restart advanced"),
      p.block("simulation/restore",{{"snapshot",at("restart resized",1)}}),p.block("simulation/step"),capture("restart restored"),
      restart(),capture("restart last"),p.block("procedures/return",{{"value",row}})});
  const auto args=append(append(append(p.block("data/list"),p.num(1)),p.num(2)),p.num(1));
  std::vector<Id> checks{
      p.set("actual stage",item(p.get("actual row"),p.get("stage index"))),
      p.set("expected stage",item(p.get("expected row"),p.get("stage index"))),
      p.block("simulation/restore",{{"snapshot",at("expected stage",1)}}),
      p.set("expected state",p.block("simulation/state")),p.set("expected previous",p.block("simulation/previous-state")),
      p.set("expected history",p.block("simulation/history")),p.set("expected inputs",p.block("simulation/inputs")),
      require(equal(at("actual stage",2),p.get("expected previous"))),
      p.block("simulation/step"),p.set("expected next",p.block("simulation/state")),
      p.set("expected next history",p.block("simulation/history")),
      p.block("simulation/restore",{{"snapshot",at("actual stage",1)}}),
      require(equal(p.block("simulation/state"),p.get("expected state"))),
      require(equal(p.block("simulation/previous-state"),p.get("expected previous"))),
      require(equal(p.block("simulation/history"),p.get("expected history"))),
      require(equal(p.block("simulation/inputs"),p.get("expected inputs")))};
  if (observeHistory) checks.push_back(require(equal(at("actual stage",3),p.get("expected history"))));
  checks.push_back(p.block("simulation/step"));
  checks.push_back(require(equal(p.block("simulation/state"),p.get("expected next"))));
  checks.push_back(require(equal(p.block("simulation/history"),p.get("expected next history"))));
  checks.push_back(p.block("results/count"));
  checks.push_back(p.block("results/publish",{{"score",p.block("results/iterations")}}));
  p.start({p.set("restart armed",p.flag(false)),p.input(p.ms(0),p.num(1),"accelerate"),
      p.input(p.ms(anchor+70),p.num(15000)),p.repeat(p.num((anchor+80)/10),{p.block("simulation/step")}),
      p.set("restart entry",p.block("simulation/snapshot")),
      p.set("rows",p.block("procedures/map",{{"function",p.reference("restart branch")},{"list",args}})),
      p.block("flow/for-each",{{"list",p.get("rows")}},{{"name","actual row"}},{{"body",{
          p.set("expected row",p.call("restart branch","lane",{at("actual row",1)})),
          require(equal(at("actual row",8),at("expected row",8))),
          require(equal(at("actual row",9),at("expected row",9))),
          require(equal(at("actual row",8),at("actual row",9))),
          p.block("flow/for-each",{{"list",p.block("data/numbers",{{"from",p.num(2)},{"to",p.num(7)}})}},
              {{"name","stage index"}},{{"body",checks}})}}})});
  return p;
}

void CompiledRestart() {
  for (const auto anchor : {0u,1300u}) for (bool history : {false,true}) {
    const auto p=RestartFixture(anchor,history,anchor!=0);
    const auto code=CompileMappedProgram(p.graph,"restart branch");
    Check(code.program.has_value() && std::any_of(code.program->code.begin(),code.program->code.end(),
        [](const auto &instruction) { return instruction.op==vm::Op::Restart; }) &&
        std::none_of(code.program->code.begin(),code.program->code.end(),
        [](const auto &instruction) { return instruction.op==vm::Op::Interpret; }),
        "Restart was not lowered to a generic VM operation.");
    VisualValue expected;
    for (bool compiled : {false,true}) {
      TestHost host; VisualRuntimeControl control; control.compilePrograms=compiled;
      control.horizonMs=anchor+3000; control.workerCount=3;
      std::string mode; control.executionModeChanged=[&](const auto &value) { mode=value; };
      const auto result=ExecuteVisualProgram(p.graph,host,control);
      Check(result.candidates==18,"Restart fixture did not finish every native/history restoration check.");
      if (!compiled) expected=result.variables.at("rows");
      else Check(EquivalentValue(expected,result.variables.at("rows")) && mode=="Compiled block program on CPU",
          "Restart changed root origin, retained inputs, horizon, previous state or frozen events, or required fallback: "+mode);
    }
  }
  TestHost host; host.setHorizon(80);
  const auto origin=std::make_shared<const VisualSnapshot>(VisualSnapshot{host.capture(),host.state,host.state,
      std::make_shared<const VisualInputs>(),std::make_shared<const VisualHistoryNode>(VisualHistoryNode{host.state,{},1}),80,0});
  auto prefix=origin->history;
  auto previous=host.state;
  for (unsigned i=0;i<4;++i) {
    previous=host.state; host.advance();
    prefix=std::make_shared<const VisualHistoryNode>(VisualHistoryNode{host.state,prefix,prefix->size+1});
  }
  const auto baseline=std::make_shared<const VisualSnapshot>(VisualSnapshot{host.capture(),host.state,previous,origin->inputs,prefix,80,0});
  Program bounded;
  bounded.define("reset limit","unused",{bounded.block("simulation/restart"),
      bounded.repeat(bounded.num(2),{bounded.block("simulation/step")}),
      bounded.block("procedures/return",{{"value",bounded.block("simulation/history")}})});
  const auto code=CompileMappedProgram(bounded.graph,"reset limit");
  Check(code.program.has_value(),"Could not compile restart limit fixture.");
  VisualRuntimeControl control; control.collectionLimit=3; control.restartOrigin=origin;
  HostBytecodeStorage storage;
  Check(!TryUniformProgram(*code.program,baseline,{}, {VisualValue(0.0),VisualValue(0.0)},control,storage),
      "Uniform proof accepted restart physics.");
  const auto result=ExecuteHostBytecode(*code.program,host,baseline,{},VisualValue(0.0),1,control,storage);
  Check(!result.needsInterpreter && !result.stopped &&
      std::get<std::shared_ptr<const VisualList>>(result.value.data)->size()==3,
      "Restart retained the old history size for collection limits.");
  Program infinite;
  infinite.define("reset forever","unused",{infinite.block("flow/forever",{}, {},{{"body",{infinite.block("simulation/restart")}}})});
  const auto infiniteCode=CompileMappedProgram(infinite.graph,"reset forever");
  Check(infiniteCode.program.has_value(),"Could not compile restart cancellation fixture.");
  unsigned polls=0; control.stopRequested=[&] { return ++polls>8; };
  const auto stopped=ExecuteHostBytecode(*infiniteCode.program,host,baseline,{},VisualValue(0.0),1,control,storage);
  Check(stopped.stopped && !stopped.needsInterpreter,"An infinite compiled restart loop did not cancel.");
  std::cout << "PASS compiled restart: root origin, immutable history, inputs, horizons, pending ticks and frozen events\n";
}

void CompiledHorizon() {
  Program p;
  const auto horizon=[&](Id value) { return p.block("simulation/set-horizon",{{"time",value}}); };
  const auto capture=[&](const char *name) { return p.set(name,p.block("simulation/snapshot")); };
  const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
  p.event("events/on-tick",{
      p.branch(p.get("resize event"),{p.set("resize event",p.flag(false)),horizon(p.ms(120))}),
      p.set("observed event",p.block("events/state"))});
  auto row=p.block("data/list");
  for (const auto *name : {"after event","before shrink","after shrink","extended"}) row=append(row,p.get(name));
  row=append(row,p.block("simulation/snapshot")); row=append(row,p.get("retained event"));
  row=append(row,p.block("results/iterations")); row=append(row,p.block("results/best-score"));
  row=append(row,p.block("results/snapshot"));
  row=append(row,p.get("initial history")); row=append(row,p.get("extended history"));
  row=append(row,p.block("simulation/history"));
  p.define("horizon branch","extent",{p.set("initial history",p.block("simulation/history")),
      p.set("resize event",p.flag(true)),p.block("simulation/step"),
      p.set("retained event",p.get("observed event")),capture("after event"),horizon(p.ms(120)),
      p.repeat(p.num(3),{p.block("simulation/step")}),capture("before shrink"),
      p.block("results/add-count",{{"amount",p.num(2)}}),p.block("results/publish",{{"score",p.num(17)}}),
      horizon(p.ms(40)),capture("after shrink"),horizon(p.get("extent")),
      p.input(p.ms(50),p.num(-12000)),p.repeat(p.num(6),{p.block("simulation/step")}),capture("extended"),
      p.set("extended history",p.block("simulation/history")),
      p.block("simulation/restore",{{"snapshot",p.get("before shrink")}}),horizon(p.ms(80)),
      p.block("simulation/step"),p.block("procedures/return",{{"value",row}})});
  p.start({p.set("resize event",p.flag(false)),horizon(p.ms(80)),p.input(p.ms(0),p.num(12000)),
      capture("entry"),p.set("got",p.call("horizon branch","extent",{p.ms(200)}))});
  const auto code=CompileMappedProgram(p.graph,"horizon branch");
  Check(code.program.has_value() && std::any_of(code.program->code.begin(),code.program->code.end(),
      [](const auto &instruction) { return instruction.op==vm::Op::SetHorizon; }),
      "Horizon changes were not lowered to a generic VM operation.");
  TestHost source,native;
  VisualRuntimeControl control; control.compilePrograms=false;
  const auto expected=ExecuteVisualProgram(p.graph,source,control);
  const auto baseline=std::get<std::shared_ptr<const VisualSnapshot>>(expected.variables.at("entry").data);
  native.restore(*baseline->native); native.replaceInputs(*baseline->inputs);
  HostBytecodeStorage storage;
  const auto actual=ExecuteHostBytecode(*code.program,native,baseline,expected.variables,VisualValue(200.0),1,control,storage);
  Check(!actual.needsInterpreter && !actual.stopped && EquivalentValue(expected.variables.at("got"),actual.value),
      "Compiled horizon changes lost event state, previous state, snapshots, inputs or selection.");
  const auto &left=*std::get<std::shared_ptr<const VisualList>>(expected.variables.at("got").data);
  const auto &right=*std::get<std::shared_ptr<const VisualList>>(actual.value.data);
  for (unsigned i=0;i<5;++i) {
    const auto a=std::get<std::shared_ptr<const VisualSnapshot>>(left[i].data);
    const auto b=std::get<std::shared_ptr<const VisualSnapshot>>(right[i].data);
    const auto history=VisualHistoryStates(a->history), compiled=VisualHistoryStates(b->history);
    Check(history.size()==compiled.size(),"Horizon changes added or removed history ticks.");
    for (std::size_t tick=0;tick<history.size();++tick)
      Check(EquivalentValue(VisualValue(history[tick]),VisualValue(compiled[tick])),
          "Horizon changes retroactively modified a historical state.");
  }
  const auto first=std::get<std::shared_ptr<const VisualSnapshot>>(right[0].data);
  const auto shrunk=std::get<std::shared_ptr<const VisualSnapshot>>(right[2].data);
  Check(first->state.durationMs==120 && VisualHistoryStates(first->history).back().durationMs==80 &&
      shrunk->state.durationMs==40 && VisualHistoryStates(shrunk->history).back().durationMs==120,
      "Horizon fixture no longer distinguishes current state from its historical prefix.");
  Program setter;
  setter.define("set horizon","time",{setter.block("simulation/set-horizon",{{"time",setter.get("time")}}),
      setter.block("procedures/return",{{"value",setter.block("simulation/horizon")}})});
  const auto setterCode=CompileMappedProgram(setter.graph,"set horizon");
  Check(setterCode.program.has_value(),"Could not compile horizon validation fixture.");
  Check(!TryUniformProgram(*setterCode.program,baseline,{},
      {VisualValue(80.0),VisualValue(80.0)},control,storage),"Uniform execution accepted a horizon mutation.");
  for (const auto value : {0.0,-10.0,15.0,std::numeric_limits<double>::infinity()}) {
    native.restore(*baseline->native); native.replaceInputs(*baseline->inputs);
    Check(ExecuteHostBytecode(*setterCode.program,native,baseline,{},VisualValue(value),1,control,storage).needsInterpreter,
        "Invalid horizons did not preserve source diagnostic fallback.");
  }
  std::cout << "PASS compiled horizon changes: extension, shrinkage, event contexts and exact historical durations\n";
}

void CompiledRestore() {
  Program p;
  const auto restore=[&](const char *name) { return p.block("simulation/restore",{{"snapshot",p.get(name)}}); };
  const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
  p.event("events/on-tick",{p.set("event held",p.block("events/state"))});
  auto row=p.block("data/list");
  for (const auto *name : {"frozen event","generated","after early","after late"}) row=append(row,p.get(name));
  for (const auto *id : {"simulation/state","simulation/previous-state","simulation/inputs",
      "simulation/horizon","results/iterations","results/best-score","results/snapshot","simulation/snapshot"})
    row=append(row,p.block(id));
  p.define("restore branch","index",{
      p.block("simulation/step"),p.set("frozen event",p.get("event held")),
      p.block("results/count"),p.block("results/publish",{{"score",p.num(17)}}),
      p.set("generated",p.block("simulation/snapshot")),restore("early"),
      p.set("after early",p.block("simulation/snapshot")),
      p.input(p.ms(10),p.num(-12000)),p.block("simulation/step"),
      restore("late"),p.set("after late",p.block("simulation/snapshot")),
      p.block("simulation/step"),restore("generated"),p.block("simulation/step"),
      restore("early"),p.block("results/count"),p.block("simulation/step"),
      p.block("procedures/return",{{"value",row}})});
  p.start({p.block("simulation/set-horizon",{{"time",p.ms(40)}}),p.input(p.ms(0),p.num(12000)),
      p.set("early",p.block("simulation/snapshot")),p.repeat(p.num(2),{p.block("simulation/step")}),
      p.block("simulation/set-horizon",{{"time",p.ms(120)}}),p.input(p.ms(30),p.num(23000)),
      p.set("late",p.block("simulation/snapshot")),p.repeat(p.num(3),{p.block("simulation/step")}),
      p.set("entry",p.block("simulation/snapshot")),
      p.set("got",p.call("restore branch","index",{p.num(1)}))});
  const auto code=CompileMappedProgram(p.graph,"restore branch");
  Check(code.program.has_value(),"Could not compile restore fixture.");
  Check(std::count_if(code.program->code.begin(),code.program->code.end(),
      [](const auto &instruction) { return instruction.op==vm::Op::Restore; })==4,
      "Restore was not lowered to a generic VM operation.");
  TestHost source,native;
  VisualRuntimeControl control; control.compilePrograms=false;
  const auto expected=ExecuteVisualProgram(p.graph,source,control);
  const auto baseline=std::get<std::shared_ptr<const VisualSnapshot>>(expected.variables.at("entry").data);
  native.restore(*baseline->native); native.replaceInputs(*baseline->inputs);
  HostBytecodeStorage storage;
  const auto actual=ExecuteHostBytecode(*code.program,native,baseline,expected.variables,VisualValue(1.0),1,control,storage);
  Check(!actual.needsInterpreter && !actual.stopped && EquivalentValue(expected.variables.at("got"),actual.value),
      "Compiled restore changed state, inputs, horizon, event state, history, candidates or selected result.");
  Program proof;
  proof.define("restore only","saved",{proof.block("simulation/restore",{{"snapshot",proof.get("saved")}}),
      proof.block("procedures/return",{{"value",proof.num(1)}})});
  const auto proofCode=CompileMappedProgram(proof.graph,"restore only");
  Check(proofCode.program.has_value(),"Could not compile restore-only uniform fixture.");
  const auto uniform=TryUniformProgram(*proofCode.program,baseline,{},
      {VisualValue(baseline),VisualValue(baseline)},control,storage);
  Check(!uniform,"A restore program was incorrectly accepted as uniform.");
  std::cout << "PASS generic compiled restore: imported/generated snapshots, horizons, retained events and results\n";
}

void CompiledMacros() {
  std::mt19937_64 standard;
  vm::Random portable;
  for (const auto seed : {0ull,1ull,65535ull,0xfedcba9876543210ull}) {
    standard.seed(seed); portable.seed(seed);
    for (unsigned i=0;i<10000;++i) Check(standard()==portable.next(),"Compiled RNG differs from mt19937_64.");
  }
  HostBytecodeStorage storage;
  std::size_t compiled=0;
  for (const auto &macro : VisualMacroCatalog()) {
    if (macro.id=="bruteforce") continue;
    auto graph=macro.program;
    const auto definitionId=graph.topLevel.front();
    auto *definition=graph.find(definitionId);
    definition->definitionId="procedures/define";
    definition->fields={{"name","arbitrary renamed operation"},{"parameters","item"}};
    auto next=graph.nodes.rbegin()->first;
    const auto add=[&](const std::string &id,std::map<std::string,VisualNodeId> inputs={},
                       std::map<std::string,std::string> fields={}) {
      VisualNode node; node.id=++next; node.definitionId=id; node.inputs=std::move(inputs); node.fields=std::move(fields);
      graph.nodes.emplace(node.id,node); return node.id;
    };
    auto value=add("data/list");
    for (const auto &id : {"simulation/inputs","simulation/state","simulation/previous-state","results/has-result"})
      value=add("data/append",{{"list",value},{"value",add(id)}});
    const auto output=add("data/local",{{"value",value}},{{"name","output"}});
    definition->statements["body"].push_back(output);
    const auto resultPair=add("data/append",{{"list",add("data/append",{{"list",add("data/get",{},{{"name","output"}})},
       {"value",add("results/best-score")}})},{"value",add("results/snapshot")}});
    const auto branch=add("flow/if",{{"condition",add("results/has-result")}});
    graph.find(branch)->statements["body"]={add("data/set",{{"value",resultPair}},{{"name","output"}})};
    definition->statements["body"].push_back(branch);
    definition->statements["body"].push_back(add("procedures/return",{{"value",add("data/get",{},{{"name","output"}})}}));
    const auto call=add("procedures/value",{{"arg0",add("values/number",{},{{"value","7"}})}},
                        {{"name","arbitrary renamed operation"},{"parameters","item"}});
    const auto start=add("flow/when-start");
    graph.find(start)->statements["body"]={add("data/set",{{"value",call}},{{"name","got"}})};
    graph.topLevel.insert(graph.topLevel.begin(),start);
    const auto code=CompileMappedProgram(graph,"arbitrary renamed operation");
    Check(code.program.has_value(),macro.id+": "+code.reason);
    for (bool empty : {false,true}) {
      TestHost interpreted,native;
      interpreted.setHorizon(80);
      if (!empty) interpreted.events={Input(0,SandboxInputAction::Steer,65536),Input(20,SandboxInputAction::Accelerate,1),Input(40,SandboxInputAction::Steer,1000)};
      native=interpreted;
      auto baseline=std::make_shared<VisualSnapshot>(VisualSnapshot{native.capture(),native.state,native.state,
        std::make_shared<const VisualInputs>(native.events),std::make_shared<VisualHistoryNode>(VisualHistoryNode{native.state,{},1}),80,0});
      VisualRuntimeControl control; control.horizonMs=80;
      const auto expected=ExecuteVisualProgram(graph,interpreted,control);
      const auto actual=ExecuteHostBytecode(*code.program,native,baseline,{},VisualValue(7.0),1,control,storage);
      Check(!actual.stopped && EquivalentValue(expected.variables.at("got"),actual.value),macro.id+": compiled result differs from source execution");
    }
    ++compiled;
  }
  std::cout << "PASS " << compiled << " macro programs through generic bytecode; exact RNG, inputs, state, scores and snapshots\n";
}

void ExpandedMacroExecution() {
  VisualRuntimeControl control; control.horizonMs=80; control.operationLimit=2000000;
  for (const auto &macro : VisualMacroCatalog()) {
    for (bool empty : {false,true}) {
      TestHost host,again;
      host.setHorizon(80);
      if (!empty) host.events={Input(0,SandboxInputAction::Steer,65536),Input(20,SandboxInputAction::Accelerate,1),Input(40,SandboxInputAction::Steer,1000)};
      again=host;
      try {
        auto program=macro.program;
        if (macro.id=="bruteforce") MacroValue(program,"iterations","3");
        const auto first=ExecuteVisualProgram(program,host,control);
        const auto second=ExecuteVisualProgram(program,again,control);
        Check(SameInputs(host.events,again.events) && host.state.timeMs==again.state.timeMs,"Expanded macro is not deterministic.");
        Check(first.published.has_value()==second.published.has_value(),"Expanded macro selection is not deterministic.");
      } catch (const std::exception &error) { throw std::runtime_error(macro.id+": "+error.what()); }
    }
  }
  std::cout << "PASS " << VisualMacroCatalog().size() << " expanded macroblocks on populated and empty input sequences\n";
}

void MutationMacroSemantics() {
  using A=SandboxInputAction;
  VisualRuntimeControl control; control.horizonMs=200;
  TestHost initial;
  initial.events={Input(0,A::Steer,1000),Input(0,A::Accelerate,1),Input(30,A::Accelerate,0),Input(60,A::Accelerate,1),Input(100,A::Steer,2000)};
  auto deletion=Macro("input-deletion");
  for (const auto &channel : {"steering","accelerate","brake"}) MacroValue(deletion,std::string("maximum ")+channel+" deletions","0");
  TestHost host=initial;
  ExecuteVisualProgram(deletion,host,control);
  Check(SameInputs(host.events,initial.events),"Zero deletion limits changed inputs.");
  MacroValue(deletion,"maximum accelerate deletions","3");
  MacroFlag(deletion,"delete steering",false); MacroFlag(deletion,"delete brake",false);
  ExecuteVisualProgram(deletion,host,control);
  for (const auto &event : host.events)
    Check(std::any_of(initial.events.begin(),initial.events.end(),[&](const auto &original){return SameInputEvent(event,original);}),"Deletion generated or altered an event.");
  Check(SteeringStateAt(host.events,110)==2000,"Accelerate deletion changed steering.");

  auto existing=Macro("existing-events");
  MacroValue(existing,"minimum edits","100"); MacroValue(existing,"maximum edits","100");
  MacroValue(existing,"maximum shift ms","0"); MacroValue(existing,"minimum absolute steering","5000"); MacroValue(existing,"maximum absolute steering","5000");
  MacroFlag(existing,"absolute steering",true); MacroFlag(existing,"toggle accelerate",false); MacroFlag(existing,"toggle brake",false);
  host=initial; ExecuteVisualProgram(existing,host,control);
  auto expected=initial.events;
  for (auto &event:expected) if (event.action==A::Steer) event.value.analog=5000;
  Check(SameInputs(host.events,expected),"Existing-event macro does not honor channel toggles, absolute mode or without-replacement selection.");
  MacroValue(existing,"minimum steering","999999");
  MacroValue(existing,"maximum steering","-999999");
  host=initial; ExecuteVisualProgram(existing,host,control);
  Check(SameInputs(host.events,expected),"An inactive delta range affects absolute steering mode.");
  MacroValue(existing,"maximum shift ms","10000");
  host=initial; ExecuteVisualProgram(existing,host,control);
  std::set<std::pair<std::int32_t,int>> keys;
  for (const auto &event:host.events) Check(keys.emplace(event.timeMs,static_cast<int>(event.action)).second,"Visible collision resolution retained duplicate time/action keys.");

  auto insertion=Macro("input-insertion");
  MacroFlag(insertion,"insert steering",false); MacroFlag(insertion,"insert brake",false);
  MacroValue(insertion,"start ms","20"); MacroValue(insertion,"end ms","40");
  host=initial; ExecuteVisualProgram(insertion,host,control);
  expected={Input(0,A::Steer,1000),Input(0,A::Accelerate,1),Input(20,A::Accelerate,0),Input(40,A::Accelerate,0),Input(60,A::Accelerate,1),Input(100,A::Steer,2000)};
  Check(SameInputs(host.events,expected),"Expanded hold failed to remove intervening events or restore the original end value.");
  auto offset=Macro("input-insertion");
  MacroValue(offset,"minimum steering insertions","2"); MacroValue(offset,"maximum steering insertions","2");
  MacroFlag(offset,"steering is offset",true);
  TestHost offsetA=initial, offsetB=initial;
  ExecuteVisualProgram(offset,offsetA,control);
  MacroValue(offset,"minimum steering","999999"); MacroValue(offset,"maximum steering","-999999");
  ExecuteVisualProgram(offset,offsetB,control);
  Check(SameInputs(offsetA.events,offsetB.events),"Inactive absolute bounds affect offset insertions.");

  auto smooth=Macro("smooth-steering");
  MacroValue(smooth,"first ms","20"); MacroValue(smooth,"last ms","20");
  MacroValue(smooth,"radius ms","20"); MacroValue(smooth,"deformations","3");
  MacroValue(smooth,"minimum amplitude","300"); MacroValue(smooth,"maximum amplitude","300");
  host=initial; ExecuteVisualProgram(smooth,host,control);
  Check(SteeringStateAt(host.events,30)==1300 && SteeringStateAt(host.events,40)==1000 && SteeringStateAt(host.events,110)==2000,
        "Cosine bumps accumulated into a ramp or leaked past the clipped window.");

  // A macro is still the same source when moved inside a user procedure.
  auto wrapped=Macro("random-steering");
  auto &definition=*wrapped.find(wrapped.topLevel.front());
  definition.definitionId="procedures/define"; definition.fields={{"name","edit"},{"parameters",""}};
  const auto base=wrapped.nodes.rbegin()->first;
  wrapped.nodes.emplace(base+1,VisualNode{base+1,"values/text",{{"value","caller"}},{},{}});
  wrapped.nodes.emplace(base+2,VisualNode{base+2,"data/set",{{"name","inputs"}},{{"value",base+1}},{}});
  wrapped.nodes.emplace(base+3,VisualNode{base+3,"procedures/call",{{"name","edit"},{"parameters",""}},{},{}});
  wrapped.nodes.emplace(base+4,VisualNode{base+4,"flow/when-start",{},{},{{"body",{base+2,base+3}}}});
  wrapped.topLevel.insert(wrapped.topLevel.begin(),base+4);
  TestHost a=initial,b=initial;
  ExecuteVisualProgram(Macro("random-steering"),a,control);
  const auto called=ExecuteVisualProgram(wrapped,b,control);
  Check(SameInputs(a.events,b.events) && std::get<std::string>(called.variables.at("inputs").data)=="caller" && !called.variables.count("index"),
        "Wrapping a macro in a procedure leaked temporaries or changed its random/input behavior.");
}

void ExplicitTargetsAndPublication() {
  TestHost host;
  host.events={Input(0,SandboxInputAction::Steer,65536),Input(20,SandboxInputAction::Steer,0)};
  VisualRuntimeControl control; control.horizonMs=80;
  const auto speed=ExecuteVisualProgram(Macro("speed-target"),host,control);
  Check(speed.published && speed.published->score==100 && speed.published->evaluationState.timeMs==10 &&
        speed.published->snapshot->state.timeMs==10 && host.state.timeMs==80,
        "Explicit maximum loop lost the original best snapshot while advancing further.");
  auto rejected=Macro("require-each-tick"); MacroValue(rejected,"minimum speed (m/s)","1");
  TestHost stopped;
  const auto failure=ExecuteVisualProgram(rejected,stopped,control);
  Check(!std::get<bool>(failure.variables.at("passed").data) && stopped.state.timeMs==0,"Condition loop advanced after rejecting its current state.");
  TestHost finish; finish.finishAtMs=30;
  const auto precise=ExecuteVisualProgram(Macro("finish-target"),finish,control);
  Check(precise.published && precise.published->score==30000000 && precise.published->snapshot->state.timeMs==30,
        "Precise finish macro invented an absent score or selected a different state.");
  TestHost absent;
  Check(!ExecuteVisualProgram(Macro("finish-target"),absent,control).published,"Unfinished run was published as zero finish time.");
  Program p;
  const auto disabled=p.set("value",p.get("undefined")); p.graph.find(disabled)->enabled=false;
  p.start({disabled,p.input(p.ms(0),p.num(65536)),p.block("simulation/step",{{"ticks",p.num(3)}}),
      p.set("saved",p.block("simulation/snapshot")),p.block("simulation/restart"),
      p.block("results/publish-snapshot",{{"snapshot",p.get("saved")},{"score",p.num(-123)}})});
  TestHost publication;
  const auto result=ExecuteVisualProgram(p.graph,publication);
  Check(result.published && result.published->score==-123 && result.published->snapshot->state.timeMs==30 && publication.state.timeMs==0,
        "Explicit saved-result publication imposed a native policy or published the current branch.");
  p.graph.find(disabled)->enabled=true; Fails(p,"undefined");
}

void AcceleratedSimulationParity(const std::string &packs,const std::string &scenario) {
  Program p;
  const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
  // Cross a pending-tick batch boundary, then read each property before a full
  // previous-state read can populate its cached public view.
  std::vector<Id> previousChecks{p.repeat(p.num(137),{p.block("simulation/step")})};
  for (const auto &property : VisualStateProperties()) {
    previousChecks.push_back(p.block("simulation/step"));
    previousChecks.push_back(p.set("direct previous property",p.block("simulation/read",
        {{"state",p.block("simulation/previous-state")}},{{"property",property.first}})));
    previousChecks.push_back(p.set("materialized previous",p.block("simulation/previous-state")));
    previousChecks.push_back(p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",
        p.get("direct previous property"),p.block("simulation/read",{{"state",p.get("materialized previous")}},
            {{"property",property.first}}))}}),
        {p.set("failed",p.binary("math/divide",p.num(1),p.num(0)))}));
  }
  for (const auto &property : VisualStateProperties()) {
    previousChecks.push_back(p.set("expected predecessor",p.block("simulation/state")));
    previousChecks.push_back(p.block("simulation/step"));
    previousChecks.push_back(p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",
        p.block("simulation/read",{{"state",p.block("simulation/previous-state")}},{{"property",property.first}}),
        p.block("simulation/read",{{"state",p.get("expected predecessor")}},{{"property",property.first}}))}}),
        {p.set("failed",p.binary("math/divide",p.num(1),p.num(0)))}));
  }
  p.define("check previous properties","",previousChecks);
  p.define("arbitrary feedback branch","index",{
      p.block("procedures/do",{{"function",p.reference("check previous properties")},{"arguments",p.block("data/list")}}),
      p.block("results/add-count",{{"amount",p.get("index")}}),
      p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",
          p.block("data/item",{{"list",p.get("large shared input")},{"index",p.num(8000)}}),p.num(8000))}}),
          {p.set("failed",p.binary("math/divide",p.num(1),p.num(0)))}),
      p.set("left",p.block("data/list")),p.set("right",p.block("data/list")),
      p.repeat(p.num(48),{
          p.set("left",append(p.block("data/list"),p.get("left"))),
          p.set("right",append(p.block("data/list"),p.get("right")))}),
      p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",p.get("left"),p.get("right"))}}),
          {p.set("failed",p.binary("math/divide",p.num(1),p.num(0)))}),
      p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",p.block("runtime/batch-size"),p.get("outer batch size"))}}),
          {p.set("failed",p.binary("math/divide",p.num(1),p.num(0)))}),
      p.repeat(p.num(20),{p.block("simulation/step")}),
      p.repeat(p.binary("math/add",p.num(10),p.get("index")),{
          p.block("simulation/step"),
          p.input(p.block("simulation/time"),p.binary("math/random-integer",p.num(-20000),p.num(20000))),
          p.block("results/publish",{{"score",p.block("simulation/car-speed")}})}),
      p.set("shared tree",p.block("data/list")),
      p.repeat(p.num(36),{p.set("shared tree",append(append(p.block("data/list"),p.get("shared tree")),p.get("shared tree")))}),
      // The wide value exceeds the initial compact GPU output buffer, while
      // the shared tree must stay linear in depth rather than expand per edge.
      p.block("procedures/return",{{"value",append(append(append(p.block("data/list"),p.get("shared tree")),
          p.block("results/snapshot")),p.block("data/numbers",{{"to",p.num(3000)}}))}})});
  const auto selected=[&] { return p.block("data/item",{{"list",p.get("job")},{"index",p.num(2)}}); };
  p.start({p.set("large shared input",p.block("data/numbers",{{"to",p.num(8000)}})),
      p.input(p.ms(0),p.num(1),"accelerate"),p.repeat(p.num(100),{p.block("simulation/step")}),
      p.set("outer batch size",p.block("runtime/batch-size")),
      p.set("jobs",p.block("procedures/map",{{"function",p.reference("arbitrary feedback branch")},
          {"list",p.block("data/numbers",{{"from",p.num(1)},{"to",p.num(5)},{"step",p.num(1)}})}})),
      p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","job"}},{{"body",{
          p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",
              p.block("data/item",{{"list",p.block("data/item",{{"list",p.get("job")},{"index",p.num(3)}})},
                  {"index",p.num(3000)}}),p.num(3000))}}),
              {p.set("failed",p.binary("math/divide",p.num(1),p.num(0)))}),
          p.block("results/count"),
          p.block("results/publish-snapshot",{{"snapshot",selected()},{"score",p.num(1)}}),
          p.block("simulation/restore",{{"snapshot",selected()}}),p.block("simulation/step"),
          p.block("results/publish",{{"score",p.block("simulation/car-speed")}})}}}),
      p.set("saved time",p.block("simulation/time")),
      p.set("earlier",p.block("simulation/snapshot")),
      p.repeat(p.num(2),{p.block("simulation/step")}),
      p.set("later",p.block("simulation/snapshot")),
      p.block("simulation/restore",{{"snapshot",p.get("earlier")}}),
      p.set("restored",p.block("simulation/snapshot")),
      p.block("simulation/restore",{{"snapshot",p.get("later")}}),
      p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",p.block("simulation/time"),
          p.binary("math/add",p.get("saved time"),p.num(20)))}}),
          {p.set("failed",p.binary("math/divide",p.num(1),p.num(0)))}),
      p.block("simulation/restore",{{"snapshot",p.get("restored")}}),
      p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",p.block("simulation/time"),p.get("saved time"))}}),
          {p.set("failed",p.binary("math/divide",p.num(1),p.num(0)))})});
  std::vector<SearchLiveUpdate> expected;
  for (auto backend : {PhysicsBackend::Reference,PhysicsBackend::OptimizedCpu,PhysicsBackend::MultiThreadedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                       ,PhysicsBackend::Cuda
#endif
                       }) {
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=4;
    request.simulationHorizonMs=4000; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::Reference;
    control.sampleImprovementTimelines=false;
    std::vector<SearchLiveUpdate> observed;
    std::string mode;
    control.visualExecutionModeChanged=[&](const std::string &value) {
      if (value!="CUDA resident physics with source block control") mode=value;
    };
    control.liveChanged=[&](const SearchLiveUpdate &value) { observed.push_back(value); };
    const auto result=RunSearch(request,&control);
    Check(result.iterations==5 && observed.size()==10,"Mapped candidates or publications were lost.");
    if (expected.empty()) expected=observed;
    for (std::size_t i=0;i<observed.size();++i) {
      const auto label=std::string(PhysicsBackendId(backend))+" candidate "+std::to_string(i);
      Check(SameInputs(expected[i].bestInputs,observed[i].bestInputs),label+" changed the random/input stream.");
      for (const auto &property : VisualStateProperties())
        Check(EquivalentValue(ReadVisualStateProperty(expected[i].bestState,property.first),
                              ReadVisualStateProperty(observed[i].bestState,property.first)),label+" differs at "+property.first);
      Check(expected[i].bestScore==observed[i].bestScore,label+" score differs.");
    }
    Check(result.bestTimeline.size()==result.bestState.tick+1,"Deferred branch history lost ticks.");
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend==PhysicsBackend::Cuda)
      Check(mode=="CUDA block-program kernel","CUDA parity test fell back instead of executing on the GPU: "+mode);
#endif
    std::cout << "PASS accelerated feedback, nested values, snapshot restore/continuation and all state fields: "
              << PhysicsBackendId(backend) << '\n';
  }
}

void AcceleratedEventParity(const std::string &packs,const std::string &scenario,bool mixed=false) {
  Program p;
  const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
  p.define("positive edit","amount",{p.input(p.block("simulation/time"),p.get("amount")),
      p.block("procedures/return",{{"value",p.get("amount")}})});
  p.define("negative edit","amount",{p.input(p.block("simulation/time"),p.binary("math/subtract",p.num(0),p.get("amount"))),
      p.block("procedures/return",{{"value",p.get("amount")}})});
  p.event("events/on-tick",{p.change("ticks"),p.block("events/send",{
      {"message",p.block("values/text",{},{{"value","feedback"}})},{"value",p.block("simulation/time")}})});
  p.event("events/on-message",{
      p.set("last payload",p.block("events/value")),
      p.set("last emitted",p.block("simulation/read",{{"state",p.block("events/state")}},{{"property","time"}})),
      p.set("applied",p.block("procedures/apply",{
          {"function",p.block("data/item",{{"list",p.get("callbacks")},{"index",p.binary("math/add",p.num(1),
              p.binary("math/modulo",p.get("ticks"),p.num(2)))}})},
          {"arguments",append(p.block("data/list"),p.binary("math/multiply",p.get("branch index"),p.num(1000)))}}))},
      {},{{"name","feedback"}});
  p.event("events/when",{p.change("edges")},{{"condition",p.binary("conditions/greater-equal",p.block("simulation/time"),p.num(1100))}});
  p.define("dynamic event branch","index",{
      p.set("branch index",p.get("index")),p.set("ticks",p.num(0)),p.set("edges",p.num(0)),
      p.repeat(p.binary("math/add",p.num(120),p.get("index")),{p.block("simulation/step")}),
      p.branch(p.binary("conditions/equal",p.get("index"),p.num(mixed ? 3 : -1)),{
          p.set("observed history",p.block("simulation/history"))}),
      p.branch(p.block("conditions/not",{{"value",p.binary("conditions/and",
          p.binary("conditions/equal",p.get("edges"),p.num(1)),
          p.binary("conditions/and",p.binary("conditions/equal",p.get("last payload"),p.block("simulation/time")),
              p.binary("conditions/equal",p.get("last emitted"),p.block("simulation/time"))))}}),
          {p.set("failed",p.binary("math/divide",p.num(1),p.num(0)))}),
      p.block("procedures/return",{{"value",p.block("simulation/snapshot")}})});
  p.start({p.input(p.ms(0),p.num(1),"accelerate"),
      p.set("callbacks",append(append(p.block("data/list"),p.reference("positive edit")),p.reference("negative edit"))),
      p.set("jobs",p.block("procedures/map",{{"function",p.reference("dynamic event branch")},
          {"list",p.block("data/numbers",{{"to",p.num(4)}})}})),
      p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","saved"}},{{"body",{
          p.block("results/count"),p.block("results/publish-snapshot",{{"snapshot",p.get("saved")},{"score",p.num(1)}})}}})});
  std::vector<SearchLiveUpdate> expected;
  for (auto backend : {PhysicsBackend::Reference,PhysicsBackend::OptimizedCpu,PhysicsBackend::MultiThreadedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                       ,PhysicsBackend::Cuda
#endif
                       }) {
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=2;
    request.simulationHorizonMs=2000; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::Reference;
    control.sampleImprovementTimelines=false; control.sampleBestTimeline=false;
    std::vector<SearchLiveUpdate> observed; std::string mode;
    control.liveChanged=[&](const auto &value) { observed.push_back(value); };
    control.visualExecutionModeChanged=[&](const auto &value) { mode=value; };
    const auto result=RunSearch(request,&control);
    Check(result.iterations==4 && observed.size()==4,"Dynamic event map lost outputs.");
    if (expected.empty()) expected=observed;
    for (std::size_t i=0;i<observed.size();++i) {
      Check(EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)),
            "Dynamic event physics differs on "+std::string(PhysicsBackendId(backend)));
      Check(SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Dynamic event input stream differs.");
    }
    if (backend!=PhysicsBackend::Reference) {
      std::string expectedMode="Compiled block program on CPU";
#if FOREVERVALIDATOR_HAS_CUDA
      if (backend==PhysicsBackend::Cuda) expectedMode="CUDA block-program kernel";
#endif
      Check(mode==expectedMode,"Dynamic event branch silently fell back: "+mode);
    }
    std::cout << "PASS dynamic event physics, payloads, indirect calls, all state fields: " << PhysicsBackendId(backend) << '\n';
  }
}

void DivergentLaneParity(const std::string &packs,const std::string &scenario) {
  for (const auto count : {1u,3u,31u,32u,33u,65u}) {
    Program p;
    const auto mod=[&](const char *name,unsigned value) { return p.binary("math/modulo",p.get(name),p.num(value)); };
    p.define("recursive snapshot","depth",{
        p.branch(p.binary("conditions/greater",p.get("depth"),p.num(0)),{
            p.block("procedures/return",{{"value",p.call("recursive snapshot","depth",{
                p.binary("math/subtract",p.get("depth"),p.num(1))})}})}),
        p.block("simulation/step"),p.block("procedures/return",{{"value",p.block("simulation/snapshot")}})});
    p.define("divergent lane","index",{
        p.branch(p.binary("conditions/equal",p.get("index"),p.num(1)),{
            p.block("procedures/return",{{"value",p.get("origin")}})}),
        p.input(p.ms(0),p.binary("math/multiply",p.get("index"),p.num(1000))),p.set("angle",p.num(0)),
        p.repeat(p.binary("math/add",mod("index",7),p.num(1)),{
            p.branch(p.binary("conditions/equal",mod("index",3),p.num(0)),{
                p.set("angle",p.block("math/sin",{{"value",p.binary("math/add",p.get("angle"),p.get("index"))}})),
                p.branch(p.binary("conditions/greater",p.get("angle"),p.num(0)),{
                    p.input(p.block("simulation/time"),p.binary("math/multiply",p.get("index"),p.num(-1000)))})}),
            p.block("simulation/step")}),
        p.branch(p.binary("conditions/equal",mod("index",2),p.num(0)),{
            p.block("procedures/return",{{"value",p.get("index")}})}),
        p.block("procedures/return",{{"value",p.call("recursive snapshot","depth",{mod("index",4)})}})});
    const auto score=[&]() { return p.binary("math/multiply",p.get("publication index"),p.num(2)); };
    p.start({p.set("origin",p.block("simulation/snapshot")),
        p.set("jobs",p.block("procedures/map",{{"function",p.reference("divergent lane")},
            {"list",p.block("data/numbers",{{"to",p.num(count)}})}})),p.set("publication index",p.num(0)),
        p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","job"}},{{"body",{
            p.change("publication index"),p.block("results/count"),
            p.branch(p.binary("conditions/equal",mod("publication index",2),p.num(0)),{
                p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",p.get("job"),p.get("publication index"))}}),{
                    p.set("bad returned value",p.binary("math/divide",p.num(1),p.num(0)))}),
                p.block("results/publish",{{"score",score()}})},
                {p.block("results/publish-snapshot",{{"snapshot",p.get("job")},{"score",score()}}),
                 p.block("simulation/restore",{{"snapshot",p.get("job")}}),p.block("simulation/step"),
                 p.block("results/publish",{{"score",p.binary("math/add",score(),p.num(1))}})})}}})});
    std::vector<SearchLiveUpdate> expected;
    for (const auto backend : {PhysicsBackend::OptimizedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                              ,PhysicsBackend::Cuda
#endif
                              }) {
      SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=2;
      request.simulationHorizonMs=1000; request.executable=std::make_shared<const VisualProgram>(p.graph);
      SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::OptimizedCpu;
      control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
      std::vector<SearchLiveUpdate> observed;
      bool usedCuda=false,fellBack=false;
      control.liveChanged=[&](const auto &value) { observed.push_back(value); };
      control.visualExecutionModeChanged=[&](const auto &value) {
        usedCuda=usedCuda || value=="CUDA block-program kernel";
        fellBack=fellBack || value.find("fallback")!=std::string::npos ||
            value.rfind("CUDA physics with CPU block control:",0)==0 ||
            value=="Source interpreter: dynamic values or compiled arena capacity";
      };
      const auto result=RunSearch(request,&control);
      Check(result.iterations==count && observed.size()==count+(count+1)/2,"Divergent lanes lost publications.");
      if (expected.empty()) expected=observed;
      for (std::size_t i=0;i<observed.size();++i) {
        Check(expected[i].bestScore==observed[i].bestScore,"Divergent lanes changed publication order.");
        Check(SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Divergent lanes changed inputs.");
        Check(EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)),
              "Divergent lanes changed snapshot or restored state.");
      }
#if FOREVERVALIDATOR_HAS_CUDA
      if (backend==PhysicsBackend::Cuda) Check(usedCuda && !fellBack,"Divergent lanes silently fell back from CUDA.");
#endif
    }
    std::cout << "PASS divergent loops, recursion, host math, early and deferred returns: " << count << " lanes\n";
  }
  Program infinite;
  infinite.define("one long lane","index",{
      infinite.branch(infinite.binary("conditions/equal",infinite.get("index"),infinite.num(1)),{
          infinite.repeat(infinite.num(1e12),{})}),
      infinite.block("procedures/return",{{"value",infinite.get("index")}})});
  infinite.start({infinite.set("jobs",infinite.block("procedures/map",{
      {"function",infinite.reference("one long lane")},
      {"list",infinite.block("data/numbers",{{"to",infinite.num(33)}})}})),infinite.block("results/count")});
  for (const auto backend : {PhysicsBackend::OptimizedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                            ,PhysicsBackend::Cuda
#endif
                            }) {
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=2;
    request.simulationHorizonMs=1000; request.executable=std::make_shared<const VisualProgram>(infinite.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::OptimizedCpu;
    control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
    std::atomic_bool stop{false};
    std::optional<std::chrono::steady_clock::time_point> stoppedAt;
    std::string mode;
    control.visualExecutionModeChanged=[&](const auto &value) { mode=value; };
    control.stopRequested=[&] { return stop.load(); };
    control.statisticsChanged=[&](const auto &) {
      if (!stop.exchange(true)) stoppedAt=std::chrono::steady_clock::now();
    };
    const auto result=RunSearch(request,&control);
    Check(stoppedAt && std::chrono::steady_clock::now()-*stoppedAt<std::chrono::seconds(5),
          "A divergent long lane ignored cancellation.");
    Check(result.iterations==0,"A cancelled divergent batch committed its continuation.");
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend==PhysicsBackend::Cuda) Check(mode=="Preparing CUDA block program","Divergent cancellation bypassed CUDA.");
#endif
    std::cout << "PASS divergent long-lane cancellation: " << PhysicsBackendId(backend) << '\n';
  }
}

void OutputGrowthParity(const std::string &packs,const std::string &scenario) {
  for (const auto payloadSize : {8193u,20000u})
      for (const auto [count,growWorking] : {std::pair{3u,false},std::pair{33u,false},std::pair{3u,true}}) {
    if (payloadSize==20000 && (count!=3 || !growWorking)) continue;
    Program p;
    const auto item=[&](unsigned index) { return p.block("data/item",{{"list",p.get("job")},{"index",p.num(index)}}); };
    const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
    Id fields=p.block("data/list");
    for (const auto *name : {"returned payload","saved","returned payload","saved"}) fields=append(fields,p.get(name));
    p.define("large output","index",{
        p.input(p.ms(0),p.binary("math/multiply",p.get("index"),p.num(1000))),
        p.repeat(p.num(40),{p.block("simulation/step")}),
        p.set("saved",p.get("origin")),p.set("returned payload",p.block("data/numbers",{{"to",p.num(3)}})),
        p.branch(p.binary("conditions/greater",p.get("index"),p.num(1)),{
            p.set("saved",p.block("simulation/snapshot")),p.set("returned payload",p.get("shared payload"))}),
        p.branch(p.binary("conditions/equal",p.get("index"),p.num(growWorking ? count : 0)),{
            p.set("working scratch",p.block("data/numbers",{{"to",p.num(20000)}}))}),
        p.set("wrapped",fields),
        p.repeat(p.num(50),{p.set("wrapped",append(p.block("data/list"),p.get("wrapped")))}),
        p.block("procedures/return",{{"value",p.get("wrapped")}})});
    p.start({p.set("shared payload",p.block("data/numbers",{{"to",p.num(payloadSize)}})),
        p.set("origin",p.block("simulation/snapshot")),
        p.set("jobs",p.block("procedures/map",{{"function",p.reference("large output")},
            {"list",p.block("data/numbers",{{"to",p.num(count)}})}})),p.set("publication index",p.num(0)),
        p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","job"}},{{"body",{
            p.repeat(p.num(50),{p.set("job",item(1))}),
            p.change("publication index"),p.set("expected payload",p.get("shared payload")),
            p.branch(p.binary("conditions/equal",p.get("publication index"),p.num(1)),{
                p.set("expected payload",p.block("data/numbers",{{"to",p.num(3)}})),
                p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",item(2),p.get("origin"))}}),
                    {p.set("bad origin",p.binary("math/divide",p.num(1),p.num(0)))})}),
            p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",item(1),p.get("expected payload"))}}),
                {p.set("bad contents",p.binary("math/divide",p.num(1),p.num(0)))}),
            p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",item(1),item(3))}}),
                {p.set("bad payload",p.binary("math/divide",p.num(1),p.num(0)))}),
            p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",item(2),item(4))}}),
                {p.set("bad alias",p.binary("math/divide",p.num(1),p.num(0)))}),
            p.block("results/count"),
            p.block("results/publish-snapshot",{{"snapshot",item(2)},{"score",p.binary("math/multiply",p.get("publication index"),p.num(2))}}),
            p.block("simulation/restore",{{"snapshot",item(2)}}),p.block("simulation/step"),
            p.block("results/publish",{{"score",p.binary("math/add",p.binary("math/multiply",p.get("publication index"),p.num(2)),p.num(1))}})
        }}})});
    std::vector<SearchLiveUpdate> expected;
    for (auto backend : {PhysicsBackend::OptimizedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                         ,PhysicsBackend::Cuda
#endif
                         }) {
      SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=2;
      request.simulationHorizonMs=1000; request.executable=std::make_shared<const VisualProgram>(p.graph);
      SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::OptimizedCpu;
      control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
      std::vector<SearchLiveUpdate> observed;
      bool usedCuda=false,fellBack=false;
      control.liveChanged=[&](const auto &value) { observed.push_back(value); };
      control.visualExecutionModeChanged=[&](const auto &value) {
        usedCuda=usedCuda || value=="CUDA block-program kernel";
        fellBack=fellBack || value.find("fallback")!=std::string::npos ||
            value.rfind("CUDA physics with CPU block control:",0)==0 ||
            value=="Source interpreter: dynamic values or compiled arena capacity";
      };
      const auto result=RunSearch(request,&control);
      Check(result.iterations==count && observed.size()==count*2,"Output growth lost lanes or publications.");
      if (expected.empty()) expected=observed;
      for (std::size_t i=0;i<observed.size();++i) {
        Check(SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Output growth changed inputs.");
        Check(EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)),
              "Output growth changed snapshot or restored state at publication "+std::to_string(i));
        Check(expected[i].bestScore==observed[i].bestScore,"Output growth changed publication ordering.");
      }
#if FOREVERVALIDATOR_HAS_CUDA
      if (backend==PhysicsBackend::Cuda) Check(usedCuda && !fellBack,"Output growth silently fell back from CUDA.");
#endif
    }
    std::cout << "PASS output growth, deep/shared values, snapshot aliases and restored physics: " << count
              << " lanes, mixed working growth=" << growWorking << ", values=" << payloadSize << '\n';
  }
}

void AcceleratedInitializationParity(const std::string &packs,const std::string &scenario) {
  for (const auto count : {31u,32u,33u,65u}) {
    Program p;
    const auto payload=[&]() { return p.block("data/numbers",{{"to",p.num(8193)}}); };
    p.define("initial storage branch","index",{
        p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",p.get("shared payload"),payload())}}),
            {p.set("failed",p.binary("math/divide",p.num(1),p.num(0)))}),
        p.input(p.ms(0),p.binary("math/multiply",p.get("index"),p.num(1000))),
        p.block("simulation/step"),
        p.block("procedures/return",{{"value",p.block("simulation/snapshot")}})});
    p.start({p.set("shared payload",payload()),
        p.set("jobs",p.block("procedures/map",{{"function",p.reference("initial storage branch")},
            {"list",p.block("data/numbers",{{"to",p.num(count)}})}})),
        p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","saved"}},{{"body",{
            p.block("results/count"),p.block("results/publish-snapshot",{{"snapshot",p.get("saved")},{"score",p.num(1)}})}}})});
    std::vector<SearchLiveUpdate> expected;
    for (const auto backend : {PhysicsBackend::OptimizedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                              ,PhysicsBackend::Cuda
#endif
                              }) {
      SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=2;
      request.simulationHorizonMs=1000; request.executable=std::make_shared<const VisualProgram>(p.graph);
      SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::OptimizedCpu;
      control.sampleImprovementTimelines=false; control.sampleBestTimeline=false;
      std::vector<SearchLiveUpdate> observed; std::string mode;
      control.liveChanged=[&](const auto &value) { observed.push_back(value); };
      control.visualExecutionModeChanged=[&](const auto &value) { mode=value; };
      const auto result=RunSearch(request,&control);
      Check(result.iterations==count && observed.size()==count,"Initialization lost mapped lanes or publications.");
      if (expected.empty()) expected=observed;
      for (std::size_t i=0;i<observed.size();++i) {
        Check(EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)),
              "Initialization changed physical state at lane "+std::to_string(i));
        Check(SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Initialization changed a lane's input stream.");
      }
#if FOREVERVALIDATOR_HAS_CUDA
      if (backend==PhysicsBackend::Cuda)
        Check(mode=="CUDA block-program kernel","Initialization coverage fell back instead of using CUDA: "+mode);
#endif
    }
    std::cout << "PASS initialized globals, native state and partial warps: " << count << " lanes\n";
  }
}

void DensePrefixParity(const std::string &packs,const std::string &scenario) {
  for (const bool eventViews : {false,true}) for (const bool respawn : {false,true}) {
  Program p;
  std::vector<Id> observer{p.change("observed ticks")};
  if (eventViews) {
    observer.push_back(p.set("observed current",p.block("simulation/state")));
    observer.push_back(p.set("observed previous",p.block("simulation/previous-state")));
  }
  p.event("events/on-tick",std::move(observer));
  p.define("prefix branch","ticks",{
      p.branch(p.binary("conditions/equal",p.get("ticks"),p.num(33)),{p.input(p.ms(0),p.num(10000))}),
      p.branch(p.binary("conditions/equal",p.get("ticks"),p.num(129)),{p.input(p.ms(1280),p.num(-12000))}),
      p.branch(p.binary("conditions/equal",p.get("ticks"),p.num(161)),{p.input(p.ms(1600),p.num(15000))}),
      p.block("simulation/step"),p.set("held snapshot",p.block("simulation/snapshot")),
      p.repeat(p.binary("math/subtract",p.get("ticks"),p.num(1)),{p.block("simulation/step")}),
      p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",p.get("observed ticks"),p.get("ticks"))}}),
          {p.set("failed",p.binary("math/divide",p.num(1),p.num(0)))}),
      p.block("procedures/return",{{"value",p.block("data/append",{{"list",p.block("data/append",
          {{"value",p.get("held snapshot")}})},{"value",p.block("simulation/snapshot")}})}})});
  auto arguments=p.block("data/list");
  const std::array<unsigned,22> ticks{1,31,32,33,63,64,65,127,128,129,159,160,161,
      511,512,513,543,544,545,575,576,577};
  for (const auto count : ticks)
    arguments=p.block("data/append",{{"list",arguments},{"value",p.num(count)}});
  p.start({p.input(p.ms(0),p.num(1),"accelerate"),
      p.input(p.ms(0),p.num(0)),p.input(p.ms(1280),p.num(0)),p.input(p.ms(1600),p.num(0)),
      p.input(p.ms(400),p.num(1),"left"),p.input(p.ms(500),p.num(0),"left"),
      p.input(p.ms(600),p.num(1),"right"),p.input(p.ms(700),p.num(0),"right"),
      p.input(p.ms(800),p.num(20000),"gas"),p.input(p.ms(900),p.num(1),"brake"),
      p.input(p.ms(1000),p.num(0),"brake"),
      p.input(p.ms(640),p.num(respawn ? 1 : 0),"respawn"),p.input(p.ms(650),p.num(0),"respawn"),
      p.set("observed ticks",p.num(0)),
      p.set("origin",p.block("simulation/snapshot")),
      p.set("reference states",p.block("data/append",{{"value",p.block("simulation/state")}})),
      p.repeat(p.num(576),{p.block("simulation/step"),p.set("reference states",p.block("data/append",
          {{"list",p.get("reference states")},{"value",p.block("simulation/state")}}))}),
      p.block("simulation/restore",{{"snapshot",p.get("origin")}}),p.set("observed ticks",p.num(0)),
      p.set("targets",arguments),p.set("entry",p.num(0)),
      p.set("jobs",p.block("procedures/map",{{"function",p.reference("prefix branch")},{"list",p.get("targets")}})),
      p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","pair"}},{{"body",{
          p.change("entry"),p.set("target",p.block("data/item",{{"list",p.get("targets")},{"index",p.get("entry")}})),
          p.set("snapshot target",p.num(1)),
          p.block("flow/for-each",{{"list",p.get("pair")}},{{"name","saved"}},{{"body",{
          p.block("simulation/restore",{{"snapshot",p.get("saved")}}),
          p.branch(p.binary("conditions/and",
              p.binary("conditions/and",p.block("conditions/not",{{"value",p.binary("conditions/equal",p.get("target"),p.num(33))}}),
                  p.block("conditions/not",{{"value",p.binary("conditions/equal",p.get("target"),p.num(129))}})),
              p.block("conditions/not",{{"value",p.binary("conditions/equal",p.get("target"),p.num(161))}})),{
              p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",p.block("simulation/previous-state"),
                  p.block("data/item",{{"list",p.get("reference states")},{"index",p.get("snapshot target")}}))}}),
                  {p.set("failed",p.binary("math/divide",p.num(1),p.num(0)))})}),
          p.block("results/count"),p.block("results/publish-snapshot",{{"snapshot",p.get("saved")},
              {"score",p.block("results/iterations")}}),
          p.block("simulation/step"),p.set("resumed",p.block("simulation/snapshot")),
          p.block("results/count"),p.block("results/publish-snapshot",{{"snapshot",p.get("resumed")},
              {"score",p.block("results/iterations")}}),p.set("snapshot target",p.get("target"))}}})}}})});
  std::vector<SearchLiveUpdate> expected;
  for (const auto backend : {PhysicsBackend::OptimizedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                            ,PhysicsBackend::Cuda
#endif
                            }) {
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=2;
    request.simulationHorizonMs=7000; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::OptimizedCpu;
    control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
    std::vector<SearchLiveUpdate> observed; std::vector<std::string> modes;
    control.liveChanged=[&](const auto &value) { observed.push_back(value); };
    control.visualExecutionModeChanged=[&](const auto &value) { modes.push_back(value); };
    const auto result=RunSearch(request,&control);
    Check(result.iterations==4*ticks.size() && observed.size()==4*ticks.size(),"Prefix observations lost publications.");
    if (expected.empty()) expected=observed;
    for (std::size_t i=0;i<observed.size();++i)
      Check(observed[i].bestState.timeMs==((i%4>=2 ? ticks[i/4] : 1)+(i%2))*10 &&
          EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)) &&
          SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Prefix observation changed a tick or input boundary.");
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend==PhysicsBackend::Cuda)
      Check(std::find(modes.begin(),modes.end(),"CUDA block-program kernel")!=modes.end() &&
          std::all_of(modes.begin(),modes.end(),[](const auto &mode) {
            return mode=="Preparing CUDA block program" || mode=="CUDA block-program kernel" ||
                mode=="CUDA resident physics with source block control" || mode=="CUDA physics with source block control" ||
                mode=="Uniform compiled block program";
          }),"Prefix observations fell back from CUDA.");
#endif
    std::cout << "PASS exact prefix observations, retained snapshots, callbacks and input boundaries: "
        << PhysicsBackendId(backend) << ", respawn=" << respawn << ", event views=" << eventViews << '\n';
  }
  }
}

void LargeValueImportParity(const std::string &packs,const std::string &scenario) {
  for (const auto payloadSize : {70000u,1000000u})
      for (const auto count : {1u,3u,33u}) for (const auto argument : {false,true}) {
    const bool exceedsImportLimit=payloadSize==1000000;
    if ((argument && count==33) || (exceedsImportLimit && (count!=1 || argument))) continue;
    Program p;
    const auto item=[&](const std::string &name,Id index) {
      return p.block("data/item",{{"list",p.get(name)},{"index",index}});
    };
    const auto length=[&](const std::string &name) { return p.block("data/length",{{"list",p.get(name)}}); };
    p.event("events/on-tick",{p.input(p.block("simulation/time"),item("changed",length("changed")))});
    p.define("large import branch",argument ? "payload" : "index",{
        p.set("draw",p.binary("math/random-integer",p.num(-100),p.num(100))),
        p.set("changed",p.block("data/replace-item",{{"list",p.get("payload")},{"index",length("payload")},
            {"value",p.binary("math/add",argument ? p.num(123) : p.get("index"),p.get("draw"))}})),
        p.input(p.ms(0),p.binary("math/subtract",item("payload",length("payload")),
            p.binary("math/subtract",length("payload"),p.num(1000)))),
        p.block("simulation/step"),p.block("simulation/step"),
        p.block("procedures/return",{{"value",p.block("data/append",{{"list",p.block("data/append",
            {{"value",p.get("a saved")}})},{"value",p.block("simulation/snapshot")}})}})});
    std::vector<Id> body={p.set("a saved",p.block("simulation/snapshot")),
        p.set("payload",p.block("data/numbers",{{"to",p.num(payloadSize)}}))};
    if (argument) {
      body.push_back(p.set("arguments",p.block("data/list")));
      for (unsigned i=0;i<count;++i) body.push_back(p.set("arguments",p.block("data/append",
          {{"list",p.get("arguments")},{"value",p.get("payload")}})));
    }
    const auto mapAndPublish=[&] {
      body.push_back(p.set("jobs",p.block("procedures/map",{{"function",p.reference("large import branch")},
          {"list",argument ? p.get("arguments") : p.block("data/numbers",{{"to",p.num(count)}})}})));
      body.push_back(p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","saved"}},{{"body",{
          p.block("flow/for-each",{{"list",p.get("saved")}},{{"name","snapshot"}},{{"body",{
              p.block("results/count"),p.block("results/publish-snapshot",{{"snapshot",p.get("snapshot")},
                  {"score",p.block("results/iterations")}})}}})}}}));
    };
    mapAndPublish();
    unsigned maps=1;
    if (!argument && count==3 && !exceedsImportLimit) {
      for (const auto size : {3u,140000u,5u,70000u}) {
        body.push_back(p.set("payload",p.block("data/numbers",{{"to",p.num(size)}})));
        mapAndPublish(); ++maps;
      }
    }
    p.start(std::move(body));
    std::vector<SearchLiveUpdate> expected;
    for (const auto backend : {PhysicsBackend::OptimizedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                              ,PhysicsBackend::Cuda
#endif
                              }) {
      SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=2;
      request.simulationHorizonMs=1000; request.executable=std::make_shared<const VisualProgram>(p.graph);
      SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::OptimizedCpu;
      control.sampleImprovementTimelines=false; control.sampleBestTimeline=false;
      std::vector<SearchLiveUpdate> observed; std::vector<std::string> modes;
      control.liveChanged=[&](const auto &value) { observed.push_back(value); };
      control.visualExecutionModeChanged=[&](const auto &value) { modes.push_back(value); };
      const auto result=RunSearch(request,&control);
      Check(result.iterations==2*count*maps && observed.size()==2*count*maps,"Large value imports lost publications.");
      if (expected.empty()) expected=observed;
      for (std::size_t i=0;i<observed.size();++i)
        Check(EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)) &&
            SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Large value imports changed source state or inputs.");
#if FOREVERVALIDATOR_HAS_CUDA
      if (backend==PhysicsBackend::Cuda && exceedsImportLimit) {
        Check(std::find(modes.begin(),modes.end(),
            "CUDA physics with CPU block control: initial values exceed the GPU arena")!=modes.end(),
            "Oversized imports did not report their bounded source recovery.");
      } else if (backend==PhysicsBackend::Cuda) {
        Check(!modes.empty() && modes.back()=="CUDA block-program kernel",
            "Large values did not use CUDA: "+(modes.empty() ? std::string("no mode") : modes.back()));
        for (const auto &mode : modes) Check(mode=="Preparing CUDA block program" || mode=="CUDA block-program kernel",
            "Large required values forced fallback: "+mode);
        if (argument && count==3) {
          bool preparing=false;
          unsigned importPolls=0;
          std::optional<std::chrono::steady_clock::time_point> stoppedAt;
          observed.clear(); modes.clear();
          control.visualExecutionModeChanged=[&](const auto &value) {
            modes.push_back(value); preparing=value=="Preparing CUDA block program";
          };
          control.stopRequested=[&] {
            if (!stoppedAt && preparing && ++importPolls>=2) stoppedAt=std::chrono::steady_clock::now();
            return stoppedAt.has_value();
          };
          const auto started=std::chrono::steady_clock::now();
          const auto stopped=RunSearch(request,&control);
          const auto finished=std::chrono::steady_clock::now();
          Check(stopped.iterations==0 && observed.empty() && importPolls>=2 && modes.size()==1 &&
              modes.front()=="Preparing CUDA block program",
              "Cancellation during import growth executed or published a candidate.");
          Check(stoppedAt && finished-*stoppedAt<std::chrono::seconds(5),
              "Cancellation during import growth returned too slowly.");
          std::cout << "PASS import cancellation: request-to-return "
              << std::chrono::duration_cast<std::chrono::milliseconds>(finished-*stoppedAt).count()
              << " ms, including setup " << std::chrono::duration_cast<std::chrono::milliseconds>(finished-started).count()
              << " ms\n";
        }
      }
#endif
    }
    std::cout << "PASS large required values, copy-on-write, imported snapshot and tick callback: " << count
        << " lanes, argument=" << argument << ", values=" << payloadSize << ", maps=" << maps << '\n';
  }
}

void SharedImportExportParity(const std::string &packs,const std::string &scenario) {
  Program p;
  const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
  const auto pair=[&](Id a,Id b) { return append(append(p.block("data/list"),a),b); };
  const auto replace=[&](Id list,Id value) {
    return p.block("data/replace-item",{{"list",list},{"index",p.num(16000)},{"value",value}});
  };
  const auto item=[&](const std::string &name,unsigned index) {
    return p.block("data/item",{{"list",p.get(name)},{"index",p.num(index)}});
  };
  auto returned=p.block("data/list");
  for (const auto &name : {"original","nested","consistent","held","held"}) returned=append(returned,p.get(name));
  returned=append(returned,item("payload",16000));
  p.define("shared import export","index",{
      p.set("original",p.get("payload")),p.set("payload",replace(p.get("payload"),p.get("index"))),
      p.set("restored",replace(p.get("payload"),p.num(16000))),
      p.set("consistent",p.binary("conditions/equal",p.get("nested"),pair(p.get("restored"),p.get("restored")))),
      p.input(p.ms(0),p.get("index")),p.block("simulation/step"),
      p.set("held",p.block("simulation/snapshot")),p.block("procedures/return",{{"value",returned}})});
  auto valid=p.binary("conditions/equal",item("job",1),p.get("payload"));
  valid=p.binary("conditions/and",valid,p.binary("conditions/equal",item("job",2),p.get("nested")));
  valid=p.binary("conditions/and",valid,item("job",3));
  valid=p.binary("conditions/and",valid,p.binary("conditions/equal",item("job",4),item("job",5)));
  valid=p.binary("conditions/and",valid,p.binary("conditions/equal",item("job",6),p.get("index")));
  p.start({p.set("payload",p.block("data/numbers",{{"to",p.num(16000)}})),
      p.set("nested",pair(p.get("payload"),p.get("payload"))),
      p.set("jobs",p.block("procedures/map",{{"function",p.reference("shared import export")},
          {"list",p.block("data/numbers",{{"to",p.num(4)}})}})),p.set("index",p.num(0)),
      p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","job"}},{{"body",{p.change("index"),
          p.branch(valid,{p.block("results/count"),p.block("results/publish-snapshot",
              {{"snapshot",item("job",4)},{"score",p.get("index")}})})}}})});
  std::vector<SearchLiveUpdate> expected;
  for (const auto backend : {PhysicsBackend::OptimizedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                            ,PhysicsBackend::Cuda
#endif
                            }) {
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=4;
    request.simulationHorizonMs=1000; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::OptimizedCpu;
    control.sampleImprovementTimelines=false; control.sampleBestTimeline=false;
    std::vector<SearchLiveUpdate> observed; std::vector<std::string> modes;
    control.liveChanged=[&](const auto &value) { observed.push_back(value); };
    control.visualExecutionModeChanged=[&](const auto &value) { modes.push_back(value); };
    const auto result=RunSearch(request,&control);
    Check(result.iterations==4 && observed.size()==4,"Shared imports changed full collections, mutation isolation or snapshot identity.");
    if (expected.empty()) expected=observed;
    for (std::size_t i=0;i<observed.size();++i)
      Check(EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)) &&
          SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Shared import exports changed native state or inputs.");
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend==PhysicsBackend::Cuda)
      Check(!modes.empty() && modes.back()=="CUDA block-program kernel","Shared import export fell back.");
#endif
    std::cout << "PASS shared collection exports, nested equality, mutation isolation and snapshot identity: "
        << PhysicsBackendId(backend) << '\n';
  }
}

void InitialGlobalImportParity(const std::string &packs,const std::string &scenario) {
  for (const auto count : {1u,3u,33u}) {
    Program p;
    p.define("read shared","",{p.block("procedures/return",{{"value",p.get("shared")}})});
    p.define("shadowed import branch","shadow",{
        p.set("write only",p.num(0)),
        p.set("overwritten",p.num(11)),p.change("overwritten"),
        p.branch(p.binary("conditions/greater",p.get("shadow"),p.num(1)),{
            p.block("data/local",{{"value",p.num(9)}},{{"name","assigned"}})},
            {p.set("assigned",p.num(3))}),
        p.branch(p.binary("conditions/greater",p.get("shadow"),p.num(1)),{
            p.block("data/local",{{"value",p.num(9)}},{{"name","conditional"}})}),
        p.input(p.ms(0),p.binary("math/add",p.binary("math/multiply",p.get("shadow"),p.num(1000)),
            p.binary("math/add",p.binary("math/add",p.get("overwritten"),p.get("assigned")),
                p.binary("math/add",p.get("conditional"),p.call("read shared","",{}))))),
        p.block("simulation/step"),
        p.block("procedures/return",{{"value",p.block("simulation/snapshot")}})});
    p.start({p.set("shadow",p.block("data/numbers",{{"to",p.num(70000)}})),
        p.set("write only",p.get("shadow")),p.set("overwritten",p.get("shadow")),p.set("assigned",p.get("shadow")),
        p.set("conditional",p.num(3)),p.set("shared",p.num(7)),
        p.set("jobs",p.block("procedures/map",{{"function",p.reference("shadowed import branch")},
            {"list",p.block("data/numbers",{{"to",p.num(count)}})}})),
        p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","saved"}},{{"body",{
            p.block("results/count"),p.block("results/publish-snapshot",{{"snapshot",p.get("saved")},{"score",p.num(1)}})}}})});
    std::vector<SearchLiveUpdate> expected;
    for (const auto backend : {PhysicsBackend::OptimizedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                              ,PhysicsBackend::Cuda
#endif
                              }) {
      SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=2;
      request.simulationHorizonMs=1000; request.executable=std::make_shared<const VisualProgram>(p.graph);
      SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::OptimizedCpu;
      control.sampleImprovementTimelines=false; control.sampleBestTimeline=false;
      std::vector<SearchLiveUpdate> observed; std::string mode;
      control.liveChanged=[&](const auto &value) { observed.push_back(value); };
      control.visualExecutionModeChanged=[&](const auto &value) { mode=value; };
      const auto result=RunSearch(request,&control);
      Check(result.iterations==count && observed.size()==count,"Initial global import lost a publication.");
      if (expected.empty()) expected=observed;
      for (std::size_t i=0;i<observed.size();++i)
        Check(EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)) &&
            SameInputs(expected[i].bestInputs,observed[i].bestInputs),
            "Initial global pruning changed parameter or conditional local scope.");
#if FOREVERVALIDATOR_HAS_CUDA
      if (backend==PhysicsBackend::Cuda)
        Check(mode=="CUDA block-program kernel","Unread initial globals forced a CUDA fallback: "+mode);
#endif
    }
    std::cout << "PASS large shadowed/overwritten globals, assigned branches, conditional locals and callee globals: " << count << " lanes\n";
  }
}

void DynamicGlobalImportParity(const std::string &packs,const std::string &scenario) {
  for (const auto count : {1u,3u,33u}) for (const auto eventful : {false,true}) {
    Program p;
    const auto args=[&](Id value) { return p.block("data/append",{{"list",p.block("data/list")},{"value",value}}); };
    for (const auto second : {false,true}) p.define(second ? "second callback" : "active callback","index",{
        p.set("reset scratch",p.num(0)),p.change("reset scratch"),
        p.input(p.block("simulation/time"),p.binary("math/add",p.binary("math/multiply",p.get("index"),p.num(second ? 1000 : 500)),
            p.get(second ? "second required" : "active required"))),p.block("simulation/step"),
        p.block("procedures/return",{{"value",p.block("simulation/snapshot")}})});
    p.define("unused callback","index",{p.block("procedures/return",{{"value",
        p.block("data/length",{{"list",p.get("unrelated payload")}})}})});
    p.define("aaa reader","index",{p.block("procedures/return",{{"value",p.get("reset scratch")}})});
    p.define("return callback","",{p.block("procedures/return",{{"value",p.get("selection")}})});
    p.define("dispatch","index",{p.block("procedures/return",{{"value",p.block("procedures/apply",
        {{"function",p.block("procedures/apply",{{"function",p.block("data/item",{{"list",p.get("callbacks")},{"index",p.num(1)}})},
            {"arguments",p.block("data/list")}})},{"arguments",args(p.get("index"))}})}})});
    p.define("dispatch argument","callback",{p.block("procedures/return",{{"value",p.block("procedures/apply",
        {{"function",p.get("callback")},{"arguments",args(p.num(5))}})}})});
    if (eventful) {
      p.event("events/on-tick",{p.set("selection",p.reference("second callback"))});
      p.define("dispatch transition","index",{
          p.block("procedures/do",{{"function",p.get("selection")},{"arguments",args(p.get("index"))}}),
          p.block("procedures/return",{{"value",p.block("procedures/apply",
              {{"function",p.get("selection")},{"arguments",args(p.get("index"))}})}})});
    }
    const auto map=[&](const std::string &name,Id items) { return p.set("jobs",p.block("procedures/map",
        {{"function",p.reference(name)},{"list",items}})); };
    const auto publish=[&]() { return p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","saved"}},{{"body",{
        p.block("results/count"),p.block("results/publish-snapshot",{{"snapshot",p.get("saved")},
            {"score",p.block("results/iterations")}})}}}); };
    std::vector<Id> body={p.set("unrelated payload",p.block("data/numbers",{{"to",p.num(70000)}})),
        p.set("reset scratch",p.get("unrelated payload")),
        p.set("active required",p.num(7)),p.set("second required",eventful ? p.num(11) : p.get("unrelated payload")),
        p.set("callbacks",args(p.reference("return callback"))),p.set("selection",p.reference("active callback")),
        map("dispatch",p.block("data/numbers",{{"to",p.num(count)}})),publish(),
        p.set("active required",eventful ? p.num(7) : p.get("unrelated payload")),p.set("second required",p.num(11)),
        p.set("selection",p.reference("second callback")),
        map("dispatch",p.block("data/numbers",{{"to",p.num(count)}})),publish(),
        p.set("active required",p.num(7)),p.set("callback arguments",p.block("data/list"))};
    for (unsigned i=0;i<count;++i) body.push_back(p.set("callback arguments",p.block("data/append",
        {{"list",p.get("callback arguments")},{"value",p.reference(i%2 ? "second callback" : "active callback")}})));
    body.push_back(map("dispatch argument",p.get("callback arguments"))); body.push_back(publish());
    if (eventful) {
      body.push_back(p.set("selection",p.reference("active callback")));
      body.push_back(map("dispatch transition",p.block("data/numbers",{{"to",p.num(count)}})));
      body.push_back(publish());
    }
    p.start(std::move(body));
    std::vector<SearchLiveUpdate> expected;
    for (const auto backend : {PhysicsBackend::OptimizedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                              ,PhysicsBackend::Cuda
#endif
                              }) {
      SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=2;
      request.simulationHorizonMs=1000; request.executable=std::make_shared<const VisualProgram>(p.graph);
      SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::OptimizedCpu;
      control.sampleImprovementTimelines=false; control.sampleBestTimeline=false;
      std::vector<SearchLiveUpdate> observed; std::vector<std::string> modes;
      control.liveChanged=[&](const auto &value) { observed.push_back(value); };
      control.visualExecutionModeChanged=[&](const auto &value) { modes.push_back(value); };
      const auto result=RunSearch(request,&control);
      const auto publications=count*(eventful ? 4u : 3u);
      Check(result.iterations==publications && observed.size()==publications,"Dynamic callback maps lost publications.");
      if (expected.empty()) expected=observed;
      for (std::size_t i=0;i<observed.size();++i)
        Check(EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)) &&
            SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Dynamic callback reachability changed source state or inputs.");
#if FOREVERVALIDATOR_HAS_CUDA
      if (backend==PhysicsBackend::Cuda) {
        Check(!modes.empty() && modes.back()=="CUDA block-program kernel","Dynamic callbacks did not use CUDA.");
        for (const auto &mode : modes) Check(mode=="Preparing CUDA block program" || mode=="CUDA block-program kernel",
            "An unrelated dynamic definition forced fallback: "+mode);
      }
#endif
    }
    std::cout << "PASS returned/global/argument callbacks, changed targets and unrelated large globals: " << count
        << " lanes, events=" << eventful << '\n';
  }
}

void AcceleratedPublicationParity(const std::string &packs,const std::string &scenario) {
  Program p;
  p.define("constant-score branch","index",{
      p.input(p.ms(0),p.binary("math/multiply",p.get("index"),p.num(1000))),
      p.repeat(p.binary("math/add",p.num(128),p.get("index")),{p.block("simulation/step")}),
      p.block("results/publish",{{"score",p.num(7)}}),
      p.block("procedures/return",{{"value",p.block("results/snapshot")}})});
  p.start({p.input(p.ms(0),p.num(1),"accelerate"),
      p.set("jobs",p.block("procedures/map",{{"function",p.reference("constant-score branch")},
          {"list",p.block("data/numbers",{{"to",p.num(4)}})}})),
      p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","saved"}},{{"body",{
          p.block("results/count"),p.block("results/publish-snapshot",{{"snapshot",p.get("saved")},{"score",p.num(7)}})}}})});
  std::vector<SearchLiveUpdate> expected;
  for (auto backend : {PhysicsBackend::Reference,PhysicsBackend::MultiThreadedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                       ,PhysicsBackend::Cuda
#endif
                       }) {
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=2;
    request.simulationHorizonMs=2000; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::Reference;
    control.sampleImprovementTimelines=false; control.sampleBestTimeline=false;
    std::vector<SearchLiveUpdate> observed; std::string mode;
    control.liveChanged=[&](const auto &value) { observed.push_back(value); };
    control.visualExecutionModeChanged=[&](const auto &value) { mode=value; };
    RunSearch(request,&control);
    Check(observed.size()==4,"Constant-score publication lost results.");
    if (expected.empty()) expected=observed;
    for (std::size_t i=0;i<observed.size();++i)
      Check(EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)),
            "Publication captured unflushed physics on "+std::string(PhysicsBackendId(backend)));
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend==PhysicsBackend::Cuda) Check(mode=="CUDA block-program kernel","Constant-score publication fell back: "+mode);
#endif
    std::cout << "PASS implicit publication barrier and full snapshot state: " << PhysicsBackendId(backend) << '\n';
  }
}

void ImportedSnapshotParity(const std::string &packs,const std::string &scenario,bool restoreInMap=false) {
  Program p;
  const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
  const auto item=[&](double index) { return p.block("data/item",{{"list",p.get("row")},{"index",p.num(index)}}); };
  auto row=append(p.block("data/list"),p.get("saved"));
  row=append(row,p.block("simulation/read",{{"state",p.block("simulation/snapshot-state",{{"snapshot",p.get("saved")}})}},{{"property","time"}}));
  row=append(row,p.block("inputs/count",{{"inputs",p.block("simulation/snapshot-inputs",{{"snapshot",p.get("saved")}})}}));
  row=append(row,p.binary("conditions/equal",p.get("saved"),p.get("root")));
  std::vector<Id> inspect;
  if (restoreInMap) {
    inspect.push_back(p.block("simulation/restore",{{"snapshot",p.get("saved")}}));
    inspect.push_back(p.block("simulation/step"));
  }
  inspect.push_back(p.block("procedures/return",{{"value",row}}));
  p.define("inspect saved","saved",inspect);
  p.start({p.input(p.ms(0),p.num(1),"accelerate"),p.set("root",p.block("simulation/snapshot")),
      p.repeat(p.num(4),{p.block("simulation/step")}),p.input(p.ms(100),p.num(1234)),
      p.set("later",p.block("simulation/snapshot")),p.repeat(p.num(4),{p.block("simulation/step")}),
      p.set("jobs",p.block("procedures/map",{{"function",p.reference("inspect saved")},
          {"list",append(append(append(p.block("data/list"),p.get("root")),p.get("later")),p.get("root"))}})),
      p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","row"}},{{"body",{
          p.set("score",p.binary("math/add",item(2),item(3))),p.branch(item(4),{p.change("score",1000)}),
          p.branch(p.binary("conditions/equal",item(1),p.get("root")),{p.change("score",2000)}),
          p.block("results/count"),p.block("results/publish-snapshot",{{"snapshot",item(1)},{"score",p.get("score")}}),
          p.block("simulation/restore",{{"snapshot",item(1)}}),p.block("simulation/step"),
          p.block("results/count"),p.block("results/publish",{{"score",p.get("score")}})}}})});
  std::vector<SearchLiveUpdate> expected;
  for (auto backend : {PhysicsBackend::Reference,PhysicsBackend::MultiThreadedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                       ,PhysicsBackend::Cuda
#endif
                       }) {
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=2;
    request.simulationHorizonMs=200; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::Reference;
    control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
    std::vector<SearchLiveUpdate> observed; std::string mode;
    std::vector<std::string> modes;
    control.liveChanged=[&](const auto &value) { observed.push_back(value); };
    control.visualExecutionModeChanged=[&](const auto &value) { mode=value; modes.push_back(value); };
    RunSearch(request,&control);
    Check(observed.size()==6,"Imported snapshot map lost publications.");
    if (expected.empty()) expected=observed;
    for (std::size_t i=0;i<observed.size();++i)
      Check(expected[i].bestScore==observed[i].bestScore &&
            EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)) &&
            SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Imported snapshots changed metadata, identity or publication state.");
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend==PhysicsBackend::Cuda) {
      Check(std::find(modes.begin(),modes.end(),"CUDA block-program kernel")!=modes.end() &&
                 std::none_of(modes.begin(),modes.end(),[](const auto &value) { return value.find("source fallback")!=std::string::npos; }),
                 "Imported snapshots forced source fallback: "+mode);
    }
#endif
    std::cout << "PASS imported snapshot arguments, globals, identity and earlier-state restore (mapped restore="
              << restoreInMap << "): " << PhysicsBackendId(backend) << '\n';
  }
}

void MappedHistoryImports(const std::string &packs,const std::string &scenario) {
#if FOREVERVALIDATOR_HAS_CUDA
  using namespace forevervalidator;
  using namespace forevervalidator::experimental;
  PhysicsSandboxOptions options;
  options.backend=ToForeverValidatorBackend(PhysicsBackend::Cuda);
  options.tickDurationMs=10; options.timelineMode=PhysicsSandboxTimelineMode::Canonical;
  options.simulationHorizonMs=80;
  auto source=OpenInstalledPackDirectory(packs);
  Check(static_cast<bool>(source),"Could not open history-import Packs.");
  auto created=CreatePhysicsSandbox(std::move(source).Value(),options);
  Check(static_cast<bool>(created),"Could not create history-import sandbox.");
  auto sandbox=std::move(created).Value();
  const ReplayIdentity identity{scenario};
  auto bytes=ReadReplayFileUtf8(scenario,identity);
  Check(static_cast<bool>(bytes),"Could not read history-import scenario.");
  const auto &data=bytes.Value();
  Check(static_cast<bool>(sandbox.LoadScenario({data.data(),data.size()},identity)),"Could not load history-import scenario.");
  auto read=sandbox.ReadState(); auto captured=sandbox.CaptureState(); auto rawInputs=sandbox.ReadInputs();
  Check(read && captured && rawInputs,"Could not capture history-import baseline.");
  const auto state=read.Value();
  const auto native=std::make_shared<const VisualPhysicsSnapshot>(std::move(captured).Value());
  const auto inputs=std::make_shared<const VisualInputs>(std::move(rawInputs).Value());
  const auto history=std::make_shared<const VisualHistoryNode>(VisualHistoryNode{state,{},1,{}});
  const auto baseline=std::make_shared<const VisualSnapshot>(VisualSnapshot{native,state,state,inputs,history,80,0});
  auto foreign=std::make_shared<VisualSnapshot>(*baseline);
  unsigned samples=0;
  foreign->history=std::make_shared<const VisualHistoryNode>(VisualHistoryNode{state,{},1,
      DeferredVisualHistory([&samples,state] { ++samples; return std::vector<VisualState>{state}; })});
  const std::shared_ptr<const VisualSnapshot> argument=foreign;
  auto executor=CreateCudaProgramExecutor(sandbox,[](const VisualSnapshot &,const VisualInputs &,std::uint32_t) -> std::vector<VisualState> {
    throw std::runtime_error("Unexpected history-import replay.");
  });
  Check(static_cast<bool>(executor),"Could not create history-import CUDA executor.");
  for (const bool restore : {false,true}) {
    Program p;
    auto output=p.block("data/append",{{"list",p.block("data/list")},{"value",p.block("simulation/history")}});
    output=p.block("data/append",{{"list",output},{"value",p.block("simulation/snapshot-state",{{"snapshot",p.get("foreign")}})}});
    std::vector<Id> body;
    if (restore) body.push_back(p.block("simulation/restore",{{"snapshot",p.get("foreign")}}));
    body.push_back(p.block("procedures/return",{{"value",output}}));
    p.define("inspect foreign metadata","foreign",body);
    const auto code=CompileMappedProgram(p.graph,"inspect foreign metadata");
    Check(code.program.has_value(),"Could not compile history-import fixture.");
    const auto actual=executor->execute(*code.program,baseline,{},VisualList{VisualValue(argument)},{1},{});
    const auto frames=std::make_shared<const VisualList>(VisualList{VisualValue(state)});
    const auto expected=std::make_shared<const VisualList>(VisualList{VisualValue(frames),VisualValue(state)});
    Check(actual && !actual->stopped && actual->fallbackLanes.empty() && actual->values.size()==1 &&
        EquivalentValue(VisualValue(expected),actual->values.front()),"History metadata imports changed values or fell back.");
    Check(samples==(restore ? 1u : 0u),"An unobservable foreign history was materialized, or a restored history was skipped.");
  }
  std::cout << "PASS CUDA history imports: unrelated deferred prefixes stay lazy; restored prefixes are materialized\n";
#else
  (void)packs; (void)scenario;
#endif
}

void MappedRestartParity(const std::string &packs,const std::string &scenario,std::uint32_t anchor,bool history,bool cudaOnly=false) {
  const auto p=RestartFixture(anchor,history,anchor!=0);
  std::vector<SearchLiveUpdate> expected;
  for (auto backend : {PhysicsBackend::Reference,PhysicsBackend::MultiThreadedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                       ,PhysicsBackend::Cuda
#endif
                       }) {
#if FOREVERVALIDATOR_HAS_CUDA
    if (cudaOnly && backend!=PhysicsBackend::Cuda) continue;
#else
    Check(!cudaOnly,"CUDA restart checks require a CUDA-enabled build.");
#endif
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=3;
    request.simulationHorizonMs=anchor+3000; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::Reference;
    control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
    std::vector<SearchLiveUpdate> observed; std::vector<std::string> modes;
    control.liveChanged=[&](const auto &value) { observed.push_back(value); };
    control.visualExecutionModeChanged=[&](const auto &value) { modes.push_back(value); };
    RunSearch(request,&control);
    Check(observed.size()==18,"Mapped restart lost source replay checks or publications.");
    if (expected.empty()) expected=observed;
    for (std::size_t i=0;i<observed.size();++i)
      Check(expected[i].bestScore==observed[i].bestScore &&
          EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)) &&
          SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Mapped restart differs from source physics.");
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend==PhysicsBackend::Cuda)
      Check(std::find(modes.begin(),modes.end(),"CUDA block-program kernel")!=modes.end() &&
          std::none_of(modes.begin(),modes.end(),[](const auto &mode) { return mode.find("source fallback")!=std::string::npos; }),
          "Generic mapped restart required source fallback.");
#endif
    std::cout << "PASS mapped restart: root origin, retained inputs, divergent horizons, frozen events, native restore and "
        << (history ? "explicit" : "implicit") << " history: " << PhysicsBackendId(backend) << '\n';
  }
}

void NestedMapParity(const std::string &packs,const std::string &scenario) {
  auto p=NestedMapFixture();
  const auto item=[&](const char *name,unsigned index) {
    return p.block("data/item",{{"list",p.get(name)},{"index",p.num(index)}});
  };
  const auto equal=[&](Id a,Id b) { return p.binary("conditions/equal",a,b); };
  const auto require=[&](Id condition) { return p.branch(p.block("conditions/not",{{"value",condition}}),{
      p.set("invalid nested map",p.binary("math/divide",p.num(1),p.num(0)))}); };
  p.define("inspect nested leaf","leaf",{
      p.block("simulation/restore",{{"snapshot",item("leaf",7)}}),
      require(equal(p.block("simulation/history"),item("leaf",5))),
      require(equal(p.block("simulation/inputs"),item("leaf",6))),
      p.block("simulation/step"),p.block("results/count"),
      p.block("results/publish",{{"score",p.binary("math/add",item("leaf",3),item("leaf",4))}})});
  const auto inspect=[&]() {
    return p.block("procedures/call",{{"arg0",p.get("leaf")}},{{"name","inspect nested leaf"},{"parameters","leaf"}});
  };
  const auto inspectLeaves=[&](Id values) { return p.block("flow/for-each",{{"list",values}},{{"name","leaf"}},
      {{"body",{inspect()}}}); };
  const auto checks=p.block("flow/for-each",{{"list",p.get("values")}},{{"name","row"}},{{"body",{
      require(equal(item("row",1),item("row",2))),
      require(equal(p.block("data/length",{{"list",item("row",4)}}),p.num(0))),
      require(equal(item("row",9),p.num(7))),
      require(equal(item("row",14),p.block("data/numbers",{{"from",p.num(18)},{"to",p.num(19)}}))),
      p.set("event after",item("row",7)),require(equal(item("event after",1),p.num(91))),
      require(equal(item("event after",2),item("row",6))),
      p.block("flow/for-each",{{"list",item("row",3)}},{{"name","middle"}},{{"body",{
          require(equal(item("middle",1),item("middle",2))),inspectLeaves(item("middle",3))}}}),
      inspectLeaves(item("row",8)),
      p.block("simulation/restore",{{"snapshot",item("row",10)}}),
      require(equal(p.block("simulation/history"),item("row",11))),
      p.block("flow/for-each",{{"list",item("row",13)}},{{"name","read only"}},{{"body",{
          require(equal(item("read only",2),item("row",11))),
          p.block("results/publish",{{"score",item("read only",3)}})}}}),
      p.block("simulation/step"),p.block("results/count"),
      p.block("results/publish",{{"score",p.binary("math/add",item("row",5),item("row",12))}})}}});
  for (auto id : p.graph.topLevel) {
    auto *node=p.graph.find(id);
    if (node->definitionId=="flow/when-start") node->statements["body"].push_back(checks);
  }
  std::vector<SearchLiveUpdate> expected;
  for (auto backend : {PhysicsBackend::Reference,PhysicsBackend::MultiThreadedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                       ,PhysicsBackend::Cuda
#endif
                       }) {
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=2;
    request.simulationHorizonMs=200; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::Reference;
    control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
    std::vector<SearchLiveUpdate> observed; std::vector<std::string> modes;
    control.liveChanged=[&](const auto &value) { observed.push_back(value); };
    control.visualExecutionModeChanged=[&](const auto &value) { modes.push_back(value); };
    RunSearch(request,&control);
    Check(observed.size()==18,"Nested maps lost isolated native snapshots or publications.");
    if (expected.empty()) expected=observed;
    for (std::size_t i=0;i<observed.size();++i)
      Check(expected[i].bestScore==observed[i].bestScore &&
          EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)) &&
          SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Nested maps changed random values or native physics.");
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend==PhysicsBackend::Cuda)
      Check(std::find(modes.begin(),modes.end(),"CUDA block-program kernel")!=modes.end() &&
          std::none_of(modes.begin(),modes.end(),[](const auto &mode) { return mode.find("fallback")!=std::string::npos; }),
          "Nested maps required source fallback.");
#endif
    std::cout << "PASS nested map physics: branch isolation, event context, random streams, snapshots and continued histories: "
        << PhysicsBackendId(backend) << '\n';
  }
}

void MappedHistoryParity(const std::string &packs,const std::string &scenario,std::uint32_t anchor) {
  Program p;
  p.define("sample retained history","unused",{
      p.block("procedures/return",{{"value",p.block("simulation/history")}})});
  p.define("advance sampled history","ticks",{p.repeat(p.get("ticks"),{p.block("simulation/step")}),
      p.block("procedures/return",{{"value",p.num(0)}})});
  const auto history=[&]() { return anchor==2990 ? p.call("sample retained history","unused",{p.num(0)}) : p.block("simulation/history"); };
  const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
  const auto item=[&](unsigned index) { return p.block("data/item",{{"list",p.get("row")},{"index",p.num(index)}}); };
  const auto edited=[&](Id list) { return p.block("data/replace-item",{{"list",list},{"index",p.num(1)},{"value",p.block("simulation/state")}}); };
  auto output=p.block("data/list");
  for (const auto *name : {"saved","prefix","long snapshot","long history","restored history","edited","sum",
      "branch snapshot","branch history","continued history"})
    output=append(output,p.get(name));
  const auto advance=anchor ? p.block("procedures/do",{{"function",p.reference("advance sampled history")},
      {"arguments",append(p.block("data/list"),p.num(131))}}) : p.repeat(p.num(131),{p.block("simulation/step")});
  p.define("read sampled history","saved",{
      p.block("simulation/restore",{{"snapshot",p.get("saved")}}),p.set("prefix",history()),
      advance,p.set("long snapshot",p.block("simulation/snapshot")),
      p.set("long history",history()),p.set("edited",edited(p.get("prefix"))),p.set("sum",p.num(0)),
      p.block("flow/for-each",{{"list",history()}},{{"name","frame"}},{{"body",{
          p.set("sum",p.binary("math/add",p.get("sum"),p.block("simulation/read",{{"state",p.get("frame")}},{{"property","time"}})))}}}),
      p.input(p.binary("math/add",p.block("simulation/time"),p.ms(10)),p.num(-12000)),
      p.repeat(p.num(3),{p.block("simulation/step")}),
      p.block("simulation/restore",{{"snapshot",p.get("long snapshot")}}),p.set("restored history",history()),
      p.input(p.binary("math/add",p.block("simulation/time"),p.ms(10)),p.num(11000)),
      p.repeat(p.num(2),{p.block("simulation/step")}),
      p.set("branch snapshot",p.block("simulation/snapshot")),p.set("branch history",history()),
      p.block("simulation/restore",{{"snapshot",p.get("long snapshot")}}),p.block("simulation/step"),
      p.set("continued history",history()),
      p.block("simulation/restore",{{"snapshot",p.get("branch snapshot")}}),p.block("simulation/step"),
      p.block("simulation/restore",{{"snapshot",p.get("saved")}}),
      p.block("procedures/return",{{"value",output}})});
  const auto require=[&](Id condition) { return p.branch(p.block("conditions/not",{{"value",condition}}),{
      p.set("invalid sampled history",p.binary("math/divide",p.num(1),p.num(0)))}); };
  const auto equal=[&](Id a,Id b) { return p.binary("conditions/equal",a,b); };
  const auto arguments=append(append(append(p.block("data/list"),p.get("early")),p.get("late")),p.get("early"));
  p.start({p.input(p.ms(0),p.num(1),"accelerate"),p.input(p.ms(anchor+70),p.num(15000)),
      p.input(p.ms(anchor+100),p.num(1),"respawn"),p.input(p.ms(anchor+110),p.num(0),"respawn"),
      p.repeat(p.num(anchor/10),{p.block("simulation/step")}),p.set("early",p.block("simulation/snapshot")),
      p.repeat(p.num(4),{p.block("simulation/step")}),p.set("late",p.block("simulation/snapshot")),
      p.repeat(p.num(4),{p.block("simulation/step")}),
      p.set("jobs",p.block("procedures/map",{{"function",p.reference("read sampled history")},{"list",arguments}})),
      p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","row"}},{{"body",{
          p.block("simulation/restore",{{"snapshot",item(1)}}),p.set("source prefix",history()),
          require(equal(history(),item(2))),p.repeat(p.num(131),{p.block("simulation/step")}),
          require(equal(history(),item(4))),require(equal(history(),item(5))),require(equal(edited(p.get("source prefix")),item(6))),
          p.set("sum",p.num(0)),p.block("flow/for-each",{{"list",history()}},{{"name","frame"}},{{"body",{
              p.set("sum",p.binary("math/add",p.get("sum"),p.block("simulation/read",{{"state",p.get("frame")}},{{"property","time"}})))}}}),
          require(equal(p.get("sum"),item(7))),p.block("results/count"),p.block("results/publish",{{"score",p.get("sum")}}),
          p.block("simulation/restore",{{"snapshot",item(3)}}),require(equal(history(),item(4))),
          p.block("simulation/step"),require(equal(history(),item(10))),
          p.block("simulation/restore",{{"snapshot",item(8)}}),require(equal(history(),item(9))),
          p.block("simulation/step"),p.block("results/count"),p.block("results/publish",{{"score",p.get("sum")}})}}})});
  std::vector<SearchLiveUpdate> expected;
  for (auto backend : {PhysicsBackend::Reference,PhysicsBackend::MultiThreadedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                       ,PhysicsBackend::Cuda
#endif
                       }) {
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=3;
    request.simulationHorizonMs=anchor+2000; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::Reference;
    control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
    std::vector<SearchLiveUpdate> observed; std::vector<std::string> modes;
    control.liveChanged=[&](const auto &value) { observed.push_back(value); };
    control.visualExecutionModeChanged=[&](const auto &value) { modes.push_back(value); };
    RunSearch(request,&control);
    Check(observed.size()==6,"Mapped history lost retained values or publications.");
    if (expected.empty()) expected=observed;
    for (std::size_t i=0;i<observed.size();++i)
      Check(expected[i].bestScore==observed[i].bestScore &&
          EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)) &&
          SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Mapped history differs from source physics.");
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend==PhysicsBackend::Cuda)
      Check(std::find(modes.begin(),modes.end(),"CUDA block-program kernel")!=modes.end() &&
          std::none_of(modes.begin(),modes.end(),[](const auto &mode) { return mode.find("source fallback")!=std::string::npos; }),
          "Generic mapped history required source fallback.");
#endif
    std::cout << "PASS mapped history: independent full-state replay, pending ticks, lists and imported/generated restore: "
        << PhysicsBackendId(backend) << '\n';
  }
}

void MappedHorizonParity(const std::string &packs,const std::string &scenario,std::uint32_t anchor=0,bool observeHistory=false) {
  Program p;
  const auto horizon=[&](Id time) { return p.block("simulation/set-horizon",{{"time",time}}); };
  const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
  const auto capture=[&](const char *name) {
    auto value=append(append(p.block("data/list"),p.block("simulation/snapshot")),p.get("recorded"));
    if (observeHistory) value=append(value,p.block("simulation/history"));
    return p.set(name,value);
  };
  const auto part=[&](const char *name,int index) { return p.block("data/item",{{"list",p.get(name)},{"index",p.num(index)}}); };
  p.event("events/on-tick",{p.branch(p.get("resize event"),{
      p.set("resize event",p.flag(false)),horizon(p.ms(anchor+120))}),p.set("event held",p.block("events/state")),
      p.set("recorded",append(p.get("recorded"),p.get("event held")))});
  auto row=p.block("data/list");
  for (const auto *name : {"after event","before shrink","after shrink","extended","restored"}) row=append(row,p.get(name));
  p.define("horizon mapped","extent",{horizon(p.ms(anchor+80)),
      p.set("recorded",p.get("origin history")),
      p.set("resize event",p.flag(true)),p.block("simulation/step"),
      p.set("frozen",p.get("event held")),capture("after event"),horizon(p.ms(anchor+120)),
      p.repeat(p.num(3),{p.block("simulation/step")}),capture("before shrink"),
      p.branch(p.binary("conditions/equal",p.get("extent"),p.num(anchor+200)),{
          p.repeat(p.num(4),{p.block("simulation/step")}),
          p.block("simulation/restore",{{"snapshot",part("before shrink",1)}}),p.set("recorded",part("before shrink",2))}),
      horizon(p.ms(anchor+40)),capture("after shrink"),
      p.repeat(p.num(2048),{horizon(p.ms(anchor+50)),horizon(p.get("extent"))}),
      p.input(p.ms(anchor+50),p.num(-12000)),p.repeat(p.num(6),{p.block("simulation/step")}),capture("extended"),
      p.block("simulation/restore",{{"snapshot",part("before shrink",1)}}),p.set("recorded",part("before shrink",2)),horizon(p.ms(anchor+80)),
      p.block("simulation/step"),capture("restored"),
      p.branch(p.binary("conditions/equal",p.block("simulation/read",{{"state",p.get("frozen")}},{{"property","duration"}}),p.ms(anchor+80)),
          {p.block("procedures/return",{{"value",row}})}),p.block("procedures/return",{{"value",p.block("data/list")}})});
  std::vector<Id> previousChecks{p.set("sampled history",p.block("simulation/history")),
      p.set("previous frame",p.block("data/item",{{"list",p.get("sampled history")},{"index",p.binary("math/subtract",
          p.block("data/length",{{"list",p.get("sampled history")}}),p.num(1))}}))};
  for (const auto &property : VisualStateProperties()) {
    if (property.first=="duration") continue;
    previousChecks.push_back(p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",
        p.block("simulation/read",{{"state",p.block("simulation/previous-state")}},{{"property",property.first}}),
        p.block("simulation/read",{{"state",p.get("previous frame")}},{{"property",property.first}}))}}),
        {p.set("invalid previous "+property.first,p.binary("math/divide",p.num(1),p.num(0)))}));
  }
  previousChecks.push_back(p.block("results/publish",{{"score",p.block("simulation/read",
      {{"state",p.block("simulation/previous-state")}},{{"property","duration"}})}}));
  p.define("check horizon predecessor","",previousChecks);
  const auto historyValid=observeHistory ? p.binary("conditions/and",
      p.binary("conditions/equal",p.block("simulation/history"),part("saved",2)),
      p.binary("conditions/equal",part("saved",3),part("saved",2))) :
      p.binary("conditions/equal",p.block("simulation/history"),part("saved",2));
  p.start({p.set("resize event",p.flag(false)),p.set("recorded",p.block("data/list")),p.input(p.ms(0),p.num(1),"accelerate"),
      p.input(p.ms(anchor+90),p.num(1),"respawn"),p.input(p.ms(anchor+100),p.num(0),"respawn"),
      p.repeat(p.num(anchor/10),{p.block("simulation/step")}),p.set("origin history",p.block("simulation/history")),
      p.set("jobs",p.block("procedures/map",{{"function",p.reference("horizon mapped")},
          {"list",p.block("data/numbers",{{"from",p.num(anchor+200)},{"to",p.num(anchor+240)},{"step",p.num(20)}})}})),
      p.set("score",p.num(0)),p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","row"}},{{"body",{
          p.block("flow/for-each",{{"list",p.get("row")}},{{"name","saved"}},{{"body",{
              p.block("simulation/restore",{{"snapshot",part("saved",1)}}),
              p.block("procedures/do",{{"function",p.reference("check horizon predecessor")},{"arguments",p.block("data/list")}}),
              p.branch(p.block("conditions/not",{{"value",historyValid}}),
                  {p.set("invalid full history",p.binary("math/divide",p.num(1),p.num(0)))}),
              p.block("flow/for-each",{{"list",p.block("simulation/history")}},{{"name","frame"}},{{"body",{
                  p.change("score"),p.block("results/count"),p.block("results/publish",{{"score",p.binary("math/add",
                      p.binary("math/multiply",p.get("score"),p.num(1000)),
                      p.block("simulation/read",{{"state",p.get("frame")}},{{"property","duration"}}))}})}}}),
              horizon(p.ms(anchor+300)),p.block("simulation/step"),p.change("score"),
              p.block("results/publish",{{"score",p.binary("math/multiply",p.get("score"),p.num(1000))}})}}})}}})});
  std::vector<SearchLiveUpdate> expected;
  for (auto backend : {PhysicsBackend::Reference,PhysicsBackend::MultiThreadedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                       ,PhysicsBackend::Cuda
#endif
                       }) {
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=3;
    request.simulationHorizonMs=anchor+80; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::Reference;
    control.sampleImprovementTimelines=false; control.sampleBestTimeline=false;
    std::vector<SearchLiveUpdate> observed; std::vector<std::string> modes;
    control.liveChanged=[&](const auto &value) { observed.push_back(value); };
    control.visualExecutionModeChanged=[&](const auto &value) { modes.push_back(value); };
    RunSearch(request,&control);
    Check(observed.size()==117+15*(anchor/10),"Mapped horizon lost snapshots or historical durations: "+std::to_string(observed.size()));
    if (expected.empty()) expected=observed;
    for (std::size_t i=0;i<observed.size();++i)
      Check(expected[i].bestScore==observed[i].bestScore &&
          EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)) &&
          SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Mapped horizon changed native state or historical duration.");
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend==PhysicsBackend::Cuda)
      Check(std::find(modes.begin(),modes.end(),"CUDA block-program kernel")!=modes.end() &&
          std::none_of(modes.begin(),modes.end(),[](const auto &mode) { return mode.find("source fallback")!=std::string::npos; }),
          "Generic mapped horizon required source fallback.");
#endif
    std::cout << "PASS mapped horizon: extend, shrink, divergent plans, retained snapshots, event and historical durations: "
        << PhysicsBackendId(backend) << '\n';
  }
}

void MappedRestoreParity(const std::string &packs,const std::string &scenario,std::uint32_t anchor=0,bool resize=false) {
  Program p;
  const auto restore=[&](const char *name) {
    const auto operation=p.block("simulation/restore",{{"snapshot",p.get(name)}});
    return resize ? p.branch(p.flag(true),{operation,p.block("simulation/set-horizon",{{"time",p.binary("math/add",
        p.block("simulation/horizon"),p.ms(100))}})}) : operation;
  };
  const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
  const auto item=[&](double index) { return p.block("data/item",{{"list",p.get("row")},{"index",p.num(index)}}); };
  p.event("events/on-tick",{
      p.branch(p.get("restore in event"),{p.set("restore in event",p.flag(false)),restore("generated")}),
      p.set("event held",p.block("events/state"))});
  auto row=append(p.block("data/list"),p.block("results/snapshot"));
  row=append(row,p.get("generated")); row=append(row,p.block("simulation/snapshot"));
  row=append(row,p.block("simulation/read",{{"state",p.get("frozen")}},{{"property","time"}}));
  row=append(row,p.block("results/iterations")); row=append(row,p.block("results/best-score"));
  for (const auto *property : {"position","wheel-contact"})
    row=append(row,p.block("simulation/read",{{"state",p.get("frozen")}},{{"property",property}}));
  row=append(row,p.block("simulation/read",{{"state",p.get("event held")}},{{"property","time"}}));
  p.define("restore mapped","saved",{
      p.block("simulation/step"),p.set("frozen",p.get("event held")),
      p.block("results/add-count",{{"amount",p.num(2)}}),p.block("results/publish",{{"score",p.num(17)}}),
      p.set("local origin",p.block("simulation/snapshot")),
      p.set("discarded",p.block("simulation/snapshot")),p.set("discarded",p.num(0)),restore("saved"),
      p.input(p.binary("math/add",p.block("simulation/time"),p.ms(10)),p.num(-12000)),p.block("simulation/step"),
      p.set("generated",p.block("simulation/snapshot")),p.repeat(p.num(2),{p.block("simulation/step")}),
      restore("generated"),restore("local origin"),p.repeat(p.num(2),{p.block("simulation/step")}),
      p.branch(p.binary("conditions/greater",p.block("simulation/read",{{"state",p.block("simulation/snapshot-state",{{"snapshot",p.get("saved")}})}},{{"property","time"}}),p.num(0)),
          {p.set("host math",p.block("math/sin",{{"value",p.num(0.75)}}))}),
      restore("generated"),p.set("restore in event",p.flag(true)),p.block("simulation/step"),
      p.block("procedures/return",{{"value",row}})});
  auto arguments=append(append(append(p.block("data/list"),p.get("early")),p.get("late")),p.get("early"));
  auto valid=p.binary("conditions/and",p.binary("conditions/equal",item(4),p.num(anchor+90)),
      p.binary("conditions/and",p.binary("conditions/equal",item(5),p.num(2)),p.binary("conditions/equal",item(6),p.num(17))));
  unsigned field=7;
  for (const auto *property : {"position","wheel-contact"})
    valid=p.binary("conditions/and",valid,p.binary("conditions/equal",item(field++),
        p.block("simulation/read",{{"state",p.block("simulation/snapshot-state",{{"snapshot",item(1)}})}},{{"property",property}})));
  valid=p.binary("conditions/and",valid,p.binary("conditions/equal",item(9),p.binary("math/add",p.num(10),
      p.block("simulation/read",{{"state",p.block("simulation/snapshot-state",{{"snapshot",item(3)}})}},{{"property","time"}}))));
  p.start({p.set("restore in event",p.flag(false)),p.input(p.ms(0),p.num(1),"accelerate"),
      p.input(p.ms(anchor+10),p.num(1),"respawn"),p.input(p.ms(anchor+20),p.num(0),"respawn"),
      p.repeat(p.num(anchor/10),{p.block("simulation/step")}),p.set("early",p.block("simulation/snapshot")),
      p.repeat(p.num(4),{p.block("simulation/step")}),p.block("simulation/set-horizon",{{"time",p.ms(anchor+300)}}),
      p.input(p.ms(anchor+80),p.num(23000)),p.set("late",p.block("simulation/snapshot")),
      p.repeat(p.num(4),{p.block("simulation/step")}),
      p.set("jobs",p.block("procedures/map",{{"function",p.reference("restore mapped")},{"list",arguments}})),
      p.set("score",p.num(0)),p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","row"}},{{"body",{
          p.branch(valid,{p.block("flow/for-each",{{"list",p.block("data/numbers",{{"to",p.num(3)}})}},{{"name","part"}},{{"body",{
              p.set("exported",p.block("data/item",{{"list",p.get("row")},{"index",p.get("part")}})),
              restore("exported"),p.block("simulation/step"),p.block("results/count"),p.change("score"),
              p.block("results/publish",{{"score",p.get("score")}})}}})})}}})});
  std::vector<SearchLiveUpdate> expected;
  std::vector<SearchTimelineFrame> expectedHistory;
  for (auto backend : {PhysicsBackend::Reference,PhysicsBackend::MultiThreadedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                       ,PhysicsBackend::Cuda
#endif
                       }) {
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=3;
    request.simulationHorizonMs=anchor+200; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::Reference;
    control.sampleImprovementTimelines=false;
    std::vector<SearchLiveUpdate> observed; std::vector<std::string> modes;
    control.liveChanged=[&](const auto &value) { observed.push_back(value); };
    control.visualExecutionModeChanged=[&](const auto &value) { modes.push_back(value); };
    const auto result=RunSearch(request,&control);
    Check(observed.size()==9 && result.bestTimeline.size()==anchor/10+3,"Mapped restore lost event state, counts, selection or history.");
    if (expected.empty()) { expected=observed; expectedHistory=result.bestTimeline; }
    for (std::size_t i=0;i<observed.size();++i)
      Check(expected[i].bestScore==observed[i].bestScore &&
          EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)) &&
          SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Mapped restore changed a continued native snapshot.");
    for (std::size_t i=0;i<expectedHistory.size();++i) {
      const auto &a=expectedHistory[i], &b=result.bestTimeline[i];
      Check(a.timeMs==b.timeMs && a.positionX==b.positionX && a.positionY==b.positionY && a.positionZ==b.positionZ &&
          a.rotationX==b.rotationX && a.rotationY==b.rotationY && a.rotationZ==b.rotationZ && a.rotationW==b.rotationW &&
          a.linearSpeedX==b.linearSpeedX && a.linearSpeedY==b.linearSpeedY && a.linearSpeedZ==b.linearSpeedZ,
          "Mapped restore reconstructed different history.");
    }
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend==PhysicsBackend::Cuda)
      Check(std::find(modes.begin(),modes.end(),"CUDA block-program kernel")!=modes.end() &&
          std::none_of(modes.begin(),modes.end(),[](const auto &mode) { return mode.find("source fallback")!=std::string::npos; }),
          "Generic mapped restore required source fallback.");
#endif
    std::cout << "PASS mapped restore: distinct imported contexts, native overlays, copies, events, host math and history: "
        << PhysicsBackendId(backend) << '\n';
  }
}

void AcceleratedSnapshotSharingParity(const std::string &packs,const std::string &scenario) {
  MappedRestoreParity(packs,scenario);
  ImportedSnapshotParity(packs,scenario);
  ImportedSnapshotParity(packs,scenario,true);
  Program p;
  const auto capture=[&] {
    return p.set("held",p.block("data/append",{{"list",p.get("held")},
        {"value",p.block("simulation/snapshot")}}));
  };
  p.define("retain versions","index",{
      p.set("held",p.block("data/list")),
      // Recycle similarly sized storage while a discarded snapshot may still
      // have a scheduled native copy, then pause for exact host math.
      p.set("discarded",p.block("simulation/snapshot")),p.set("discarded",p.num(0)),
      p.set("scratch values",p.block("data/numbers",{{"to",p.num(64)}})),capture(),
      p.branch(p.binary("conditions/equal",p.get("index"),p.num(1)),{
          p.set("math guard",p.block("math/sin",{{"value",p.get("index")}}))}),
      p.branch(p.binary("conditions/equal",p.block("data/item",{{"list",p.get("scratch values")},{"index",p.num(64)}}),p.num(64)),
          {},{p.block("simulation/step")}),
      p.set("scratch values",p.block("data/list")),
      p.repeat(p.num(8),{p.block("simulation/step"),capture(),
          p.input(p.block("simulation/time"),p.binary("math/multiply",p.get("index"),p.num(3000))),capture()}),
      p.block("procedures/return",{{"value",p.get("held")}})});
  p.start({p.input(p.ms(0),p.num(1),"accelerate"),
      p.set("jobs",p.block("procedures/map",{{"function",p.reference("retain versions")},
          {"list",p.block("data/numbers",{{"to",p.num(4)}})}})),
      p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","versions"}},{{"body",{
          p.block("flow/for-each",{{"list",p.get("versions")}},{{"name","saved"}},{{"body",{
              p.block("simulation/restore",{{"snapshot",p.get("saved")}}),p.block("simulation/step"),
              p.block("results/count"),p.block("results/publish",{{"score",p.block("simulation/car-speed")}})}}})}}})});
  std::vector<SearchLiveUpdate> expected;
  for (auto backend : {PhysicsBackend::Reference,PhysicsBackend::MultiThreadedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                       ,PhysicsBackend::Cuda
#endif
                       }) {
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=4;
    request.simulationHorizonMs=1000; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::Reference;
    control.sampleImprovementTimelines=false; control.sampleBestTimeline=false;
    std::vector<SearchLiveUpdate> observed; std::string mode;
    control.liveChanged=[&](const auto &value) { observed.push_back(value); };
    control.visualExecutionModeChanged=[&](const auto &value) {
      if (value!="CUDA resident physics with source block control") mode=value;
    };
    RunSearch(request,&control);
    Check(observed.size()==68,"Shared snapshot versions were lost.");
    if (expected.empty()) expected=observed;
    for (std::size_t i=0;i<observed.size();++i) {
      Check(EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)),
          "A retained snapshot changed after later physics or input edits on "+std::string(PhysicsBackendId(backend)));
      Check(SameInputs(expected[i].bestInputs,observed[i].bestInputs),"A retained snapshot changed its input version.");
    }
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend==PhysicsBackend::Cuda) Check(mode=="CUDA block-program kernel","Shared snapshot test fell back: "+mode);
#endif
    std::cout << "PASS retained snapshot versions, independent inputs and exact continuation: " << PhysicsBackendId(backend) << '\n';
  }
}

void NestedMapResources(const std::string &packs,const std::string &scenario) {
#if FOREVERVALIDATOR_HAS_CUDA
  for (unsigned variant=0;variant<3;++variant) {
    Program p;
    const auto map=[&](const char *name) { return p.block("procedures/map",{{"function",p.reference(name)},
        {"list",p.block("data/numbers",{{"from",p.num(1)},{"to",p.num(2)}})}}); };
    const auto item=[&](const char *name,unsigned index) { return p.block("data/item",{{"list",p.get(name)},{"index",p.num(index)}}); };
    std::vector<Id> child{p.input(p.ms(10),p.binary("math/multiply",p.get("item"),p.num(1000))),
        p.block("simulation/step"),p.block("results/count")};
    if (variant==2) child.push_back(p.block("flow/forever",{}, {},{{"body",{p.block("results/count")}}}));
    child.push_back(p.block("procedures/return",{{"value",p.block("simulation/snapshot")}}));
    p.define("bounded map child","item",child);
    const auto body=p.set("children",map("bounded map child"));
    const auto loop=variant ? p.block("flow/forever",{}, {},{{"body",{body}}}) : p.repeat(p.num(2048),{body});
    p.define("bounded map parent","unused",{p.set("held",p.block("simulation/snapshot")),loop,
        p.block("procedures/return",{{"value",p.block("data/append",{
            {"list",p.block("data/append",{{"list",p.get("children")},{"value",p.get("held")}})},
            {"value",p.block("simulation/snapshot")}})}})});
    p.start({p.set("jobs",map("bounded map parent")),
        p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","row"}},{{"body",{
            p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",
                p.block("simulation/snapshot-state",{{"snapshot",item("row",3)}}),
                p.block("simulation/snapshot-state",{{"snapshot",item("row",4)}}))}}),
                {p.set("invalid parent map state",p.binary("math/divide",p.num(1),p.num(0)))}),
            p.block("simulation/restore",{{"snapshot",item("row",1)}}),p.block("simulation/step"),
            p.block("results/publish",{{"score",p.block("simulation/time")}}),
            p.block("simulation/restore",{{"snapshot",item("row",2)}}),p.block("simulation/step"),
            p.block("results/publish",{{"score",p.block("simulation/time")}})}}})});
    SearchRequest request{packs,scenario}; request.backend=PhysicsBackend::Cuda; request.parallelSampleCount=2;
    request.simulationHorizonMs=80; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
    bool kernel=false,fallback=false,cancelled=false;
    std::optional<std::chrono::steady_clock::time_point> started;
    std::vector<SearchLiveUpdate> observed;
    control.liveChanged=[&](const auto &value) { observed.push_back(value); };
    control.visualExecutionModeChanged=[&](const auto &mode) {
      kernel=kernel || mode=="CUDA block-program kernel";
      fallback=fallback || mode.find("fallback")!=std::string::npos;
      if (mode=="Preparing CUDA block program" && !started) started=std::chrono::steady_clock::now();
    };
    if (variant) control.stopRequested=[&] {
      cancelled=cancelled || (started && std::chrono::steady_clock::now()-*started>=std::chrono::milliseconds(200));
      return cancelled;
    };
    RunSearch(request,&control);
    if (variant) Check(started && cancelled && !fallback &&
        std::chrono::steady_clock::now()-*started<std::chrono::seconds(5),"Nested map cancellation was delayed or fell back.");
    else {
      Check(kernel && !fallback && observed.size()==4,"Repeated nested map scopes exhausted their CUDA arena.");
      for (const auto &value : observed)
        Check(value.bestScore==20 && value.bestState.timeMs==20,"Nested map scope lost an immutable child snapshot.");
    }
  }
  std::cout << "PASS bounded CUDA nested maps: 2048 isolated scopes per lane, retained snapshots and parent/child cancellation\n";
#else
  (void)packs; (void)scenario;
#endif
}

void MappedHistoryResources(const std::string &packs,const std::string &scenario) {
#if FOREVERVALIDATOR_HAS_CUDA
  Program p;
  const auto history=[&]() { return p.block("simulation/history"); };
  const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
  const auto item=[&](unsigned index) { return p.block("data/item",{{"list",p.get("row")},{"index",p.num(index)}}); };
  auto output=p.block("data/list");
  for (const auto *name : {"held","prefix"}) output=append(output,p.get(name));
  output=append(output,p.block("simulation/snapshot")); output=append(output,history());
  p.define("bounded sampled history","index",{p.repeat(p.num(2),{p.block("simulation/step")}),
      p.set("held",p.block("simulation/snapshot")),p.set("prefix",history()),
      p.repeat(p.num(32768),{p.set("copy",history()),p.set("edited",append(p.get("copy"),p.block("simulation/state"))),
          p.set("trimmed",p.block("data/delete-item",{{"list",p.get("edited")},
              {"index",p.block("data/length",{{"list",p.get("edited")}})}})),
          p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",p.get("copy"),p.get("trimmed"))}}),
              {p.set("invalid history copy",p.binary("math/divide",p.num(1),p.num(0)))})}),
      p.block("procedures/return",{{"value",output}})});
  std::vector<Id> checks;
  for (const auto offset : {1u,3u}) {
    checks.push_back(p.block("simulation/restore",{{"snapshot",item(offset)}}));
    checks.push_back(p.branch(p.block("conditions/not",{{"value",p.binary("conditions/equal",history(),item(offset+1))}}),
        {p.set("invalid retained history",p.binary("math/divide",p.num(1),p.num(0)))}));
    checks.push_back(p.block("results/publish",{{"score",p.block("data/length",{{"list",history()}})}}));
  }
  p.start({p.set("jobs",p.block("procedures/map",{{"function",p.reference("bounded sampled history")},
      {"list",p.block("data/numbers",{{"to",p.num(3)}})}})),
      p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","row"}},{{"body",checks}})});
  SearchRequest request{packs,scenario}; request.backend=PhysicsBackend::Cuda; request.parallelSampleCount=3;
  request.simulationHorizonMs=80; request.executable=std::make_shared<const VisualProgram>(p.graph);
  SearchRunControl control; control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
  std::vector<SearchLiveUpdate> observed; bool kernel=false,fallback=false;
  control.liveChanged=[&](const auto &value) { observed.push_back(value); };
  control.visualExecutionModeChanged=[&](const auto &mode) {
    kernel=kernel || mode=="CUDA block-program kernel";
    fallback=fallback || mode.find("source fallback")!=std::string::npos;
  };
  RunSearch(request,&control);
  Check(kernel && !fallback && observed.size()==6,"A bounded history-copy loop exhausted its CUDA arena.");
  for (const auto &value : observed)
    Check(value.bestScore==3 && value.bestState.timeMs==20 &&
        EquivalentValue(VisualValue(observed.front().bestState),VisualValue(value.bestState)),
        "Repeated history edits changed a retained snapshot or the live prefix.");
  std::cout << "PASS bounded CUDA sampled history: 32768 copy/edit cycles per lane with retained lists and snapshots\n";
#else
  (void)packs; (void)scenario;
#endif
}

void MappedRestartResources(const std::string &packs,const std::string &scenario) {
#if FOREVERVALIDATOR_HAS_CUDA
  Program p;
  const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
  const auto horizon=[&](unsigned value) { return p.block("simulation/set-horizon",{{"time",p.ms(value)}}); };
  const auto require=[&](Id condition) { return p.branch(p.block("conditions/not",{{"value",condition}}),{
      p.set("invalid restart allocation",p.binary("math/divide",p.num(1),p.num(0)))}); };
  const auto equal=[&](Id a,Id b) { return p.binary("conditions/equal",a,b); };
  const auto at=[&](const char *name,unsigned index) { return p.block("data/item",{{"list",p.get(name)},{"index",p.num(index)}}); };
  p.define("bounded restart","lane",{horizon(240),p.block("simulation/restart"),
      p.set("held restart",p.block("simulation/snapshot")),p.set("held history",p.block("simulation/history")),
      p.repeat(p.num(8192),{horizon(240),p.block("simulation/restart"),
          p.set("temporary snapshot",p.block("simulation/snapshot")),horizon(80),p.block("simulation/restart"),
          p.set("temporary history",p.block("simulation/history"))}),
      require(equal(p.block("data/length",{{"list",p.get("held history")}}),p.num(1))),
      p.block("procedures/return",{{"value",append(append(p.block("data/list"),p.get("held restart")),p.block("simulation/snapshot"))}})});
  p.start({p.repeat(p.num(4),{p.block("simulation/step")}),
      p.set("rows",p.block("procedures/map",{{"function",p.reference("bounded restart")},
          {"list",p.block("data/numbers",{{"to",p.num(3)}})}})),
      p.block("flow/for-each",{{"list",p.get("rows")}},{{"name","row"}},{{"body",{
          p.block("flow/for-each",{{"list",p.block("data/numbers",{{"to",p.num(2)}})}},{{"name","index"}},{{"body",{
              p.block("simulation/restore",{{"snapshot",p.block("data/item",{{"list",p.get("row")},{"index",p.get("index")}})}}),
              p.set("history",p.block("simulation/history")),
              require(equal(p.block("data/length",{{"list",p.get("history")}}),p.num(1))),
              require(equal(p.block("simulation/time"),p.ms(0))),
              require(equal(p.block("simulation/read",{{"state",at("history",1)}},{{"property","duration"}}),
                  p.block("simulation/horizon"))),
              p.block("simulation/step"),p.block("results/count"),
              p.block("results/publish",{{"score",p.block("simulation/horizon")}})}}})}}})});
  SearchRequest request{packs,scenario}; request.backend=PhysicsBackend::Cuda; request.parallelSampleCount=3;
  request.simulationHorizonMs=80; request.executable=std::make_shared<const VisualProgram>(p.graph);
  SearchRunControl control; control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
  std::vector<SearchLiveUpdate> observed; std::vector<std::string> modes;
  control.liveChanged=[&](const auto &value) { observed.push_back(value); };
  control.visualExecutionModeChanged=[&](const auto &value) { modes.push_back(value); };
  RunSearch(request,&control);
  Check(observed.size()==6 && std::find(modes.begin(),modes.end(),"CUDA block-program kernel")!=modes.end() &&
      std::none_of(modes.begin(),modes.end(),[](const auto &mode) { return mode.find("source fallback")!=std::string::npos; }),
      "Repeated CUDA restart leaked retained snapshots/history or required source fallback.");
  std::cout << "PASS bounded CUDA restart: 8192 double resets per lane with retained snapshots and history\n";
#else
  (void)packs; (void)scenario;
#endif
}

void MappedHistoryCancellation(const std::string &packs,const std::string &scenario,bool restart=false) {
#if FOREVERVALIDATOR_HAS_CUDA
  for (const bool restore : {false,true}) {
    Program p;
    std::vector<Id> body;
    if (restore) body.push_back(p.block("simulation/restore",{{"snapshot",p.get("saved")}}));
    if (restart) body.push_back(p.block("simulation/restart"));
    if (restore) body.push_back(p.block("simulation/step"));
    body.push_back(p.set("observed prefix",p.block("simulation/history")));
    p.define("read history forever","saved",{p.block("flow/forever",{}, {},{{"body",body}})});
    p.start({p.repeat(p.num(4),{p.block("simulation/step")}),p.set("origin",p.block("simulation/snapshot")),
        p.set("jobs",p.block("procedures/map",{{"function",p.reference("read history forever")},
            {"list",p.block("data/append",{{"list",p.block("data/list")},{"value",p.get("origin")}})}}))});
    SearchRequest request{packs,scenario}; request.backend=PhysicsBackend::Cuda;
    request.simulationHorizonMs=200; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
    std::optional<std::chrono::steady_clock::time_point> started;
    bool cancelled=false,fallback=false;
    control.visualExecutionModeChanged=[&](const auto &mode) {
      if (mode=="Preparing CUDA block program" && !started) started=std::chrono::steady_clock::now();
      fallback=fallback || mode.find("source fallback")!=std::string::npos;
    };
    control.stopRequested=[&] {
      cancelled=cancelled || (started && std::chrono::steady_clock::now()-*started>=std::chrono::milliseconds(200));
      return cancelled;
    };
    RunSearch(request,&control);
    Check(started && cancelled && !fallback && std::chrono::steady_clock::now()-*started<std::chrono::seconds(5),
        "A history-reading CUDA loop did not cancel promptly without source fallback.");
  }
  std::cout << "PASS bounded CUDA cancellation: " << (restart ? "restart and restore/restart/step/history loops" : "history reads and restore/step/history loops") << '\n';
#else
  (void)packs; (void)scenario; (void)restart;
#endif
}

void MappedHorizonResources(const std::string &packs,const std::string &scenario) {
#if FOREVERVALIDATOR_HAS_CUDA
  Program p;
  const auto horizon=[&](unsigned time) { return p.block("simulation/set-horizon",{{"time",p.ms(time)}}); };
  const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
  p.define("bounded horizon history","index",{horizon(240),p.set("held",p.block("simulation/snapshot")),
      p.repeat(p.num(65536),{horizon(80),horizon(240)}),horizon(80),
      p.block("procedures/return",{{"value",append(append(p.block("data/list"),p.get("held")),p.block("simulation/snapshot"))}})});
  p.start({p.set("jobs",p.block("procedures/map",{{"function",p.reference("bounded horizon history")},
      {"list",p.block("data/numbers",{{"to",p.num(3)}})}})),
      p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","row"}},{{"body",{
          p.block("flow/for-each",{{"list",p.get("row")}},{{"name","saved"}},{{"body",{
              p.block("simulation/restore",{{"snapshot",p.get("saved")}}),
              p.block("results/publish",{{"score",p.block("simulation/read",{{"state",p.block("data/item",{
                  {"list",p.block("simulation/history")},{"index",p.num(1)}})}},{{"property","duration"}})}})}}})}}})});
  SearchRequest request{packs,scenario}; request.backend=PhysicsBackend::Cuda; request.parallelSampleCount=3;
  request.simulationHorizonMs=80; request.executable=std::make_shared<const VisualProgram>(p.graph);
  SearchRunControl control; control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
  std::vector<SearchLiveUpdate> observed; bool kernel=false,fallback=false;
  control.liveChanged=[&](const auto &value) { observed.push_back(value); };
  control.visualExecutionModeChanged=[&](const auto &mode) {
    kernel=kernel || mode=="CUDA block-program kernel";
    fallback=fallback || mode.find("source fallback")!=std::string::npos;
  };
  RunSearch(request,&control);
  Check(kernel && !fallback && observed.size()==6,"A bounded same-time horizon loop exhausted its CUDA arena.");
  for (std::size_t i=0;i<observed.size();++i) {
    auto expected=observed.front().bestState; expected.durationMs=i%2 ? 80 : 240;
    Check(observed[i].bestScore==80 && EquivalentValue(VisualValue(expected),VisualValue(observed[i].bestState)),
        "Horizon log coalescing changed a retained snapshot or the original history frame.");
  }
  std::cout << "PASS bounded CUDA horizon history: 131072 setters per lane with a retained snapshot\n";
#else
  (void)packs; (void)scenario;
#endif
}

void MappedRestoreCancellation(const std::string &packs,const std::string &scenario,bool resize=false) {
#if FOREVERVALIDATOR_HAS_CUDA
  Program p;
  const std::vector<Id> body=resize ? std::vector<Id>{
      p.block("simulation/set-horizon",{{"time",p.ms(240)}}),p.block("simulation/set-horizon",{{"time",p.ms(40)}})}
      : std::vector<Id>{p.block("simulation/restore",{{"snapshot",p.get("saved")}})};
  p.define("restore forever","saved",{p.block("flow/forever",{}, {},{{"body",body}})});
  p.start({p.set("origin",p.block("simulation/snapshot")),p.repeat(p.num(4),{p.block("simulation/step")}),
      p.set("jobs",p.block("procedures/map",{{"function",p.reference("restore forever")},
          {"list",p.block("data/append",{{"list",p.block("data/list")},{"value",p.get("origin")}})}}))});
  SearchRequest request{packs,scenario}; request.backend=PhysicsBackend::Cuda;
  request.simulationHorizonMs=200; request.executable=std::make_shared<const VisualProgram>(p.graph);
  SearchRunControl control; control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
  std::optional<std::chrono::steady_clock::time_point> started;
  bool cancelled=false,fallback=false;
  control.visualExecutionModeChanged=[&](const auto &mode) {
    if (mode=="Preparing CUDA block program" && !started) started=std::chrono::steady_clock::now();
    fallback=fallback || mode.find("source fallback")!=std::string::npos;
  };
  control.stopRequested=[&] {
    cancelled=cancelled || (started && std::chrono::steady_clock::now()-*started>=std::chrono::milliseconds(200));
    return cancelled;
  };
  RunSearch(request,&control);
  Check(started && cancelled && !fallback && std::chrono::steady_clock::now()-*started<std::chrono::seconds(5),
      "A state-setter CUDA loop did not cancel promptly without source fallback.");
  std::cout << "PASS bounded CUDA cancellation: " << (resize ? "horizon changes" : "restore") << '\n';
#else
  (void)packs; (void)scenario; (void)resize;
#endif
}

void SourceTransitionParity(const std::string &packs,const std::string &scenario) {
  const auto *policy=std::getenv("FOREVERTAS_CUDA_SOURCE_CURSOR");
  Check(!policy || std::string(policy)!="0","Source transition test requires the resident cursor.");
  Program p;
  const auto steps=[&](unsigned count) {
    return p.repeat(p.num(count),{p.block("simulation/step"),p.block("results/count"),
        p.block("results/publish",{{"score",p.block("simulation/time")}})});
  };
  p.start({p.input(p.ms(0),p.num(1),"accelerate"),steps(70),
      p.set("held",p.block("simulation/snapshot")),p.input(p.ms(700),p.num(12345)),
      p.input(p.ms(700),p.num(1),"brake"),p.input(p.ms(700),p.num(23456)),steps(10),
      p.block("simulation/restore",{{"snapshot",p.get("held")}}),
      p.block("simulation/set-horizon",{{"time",p.ms(2500)}}),steps(70),
      p.block("simulation/restart"),steps(1),p.input(p.ms(10),p.num(-12345)),
      p.input(p.ms(10),p.num(1),"brake"),p.input(p.ms(10),p.num(-23456)),steps(1)});
  std::vector<SearchLiveUpdate> expected;
  for (auto backend : {PhysicsBackend::OptimizedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                       ,PhysicsBackend::Cuda
#endif
                       }) {
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=2;
    request.simulationHorizonMs=2000; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::OptimizedCpu;
    control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
    std::vector<SearchLiveUpdate> observed;
    std::vector<bool> resident;
    std::string mode;
    control.visualExecutionModeChanged=[&](const auto &value) { mode=value; };
    control.liveChanged=[&](const auto &value) {
      observed.push_back(value); resident.push_back(mode=="CUDA resident physics with source block control");
    };
    RunSearch(request,&control);
    Check(observed.size()==152,"Source transitions lost publications.");
    if (expected.empty()) expected=observed;
    for (std::size_t i=0;i<observed.size();++i) {
      Check(SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Source transitions changed inputs.");
      Check(EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)),
          "Source transitions changed physical state at publication "+std::to_string(i));
#if FOREVERVALIDATOR_HAS_CUDA
      if (backend==PhysicsBackend::Cuda)
        Check(resident[i],"Source transitions lost the resident cursor at publication "+std::to_string(i));
#endif
    }
    std::cout << "PASS source prediction, input edits, restore, horizon and restart: " << PhysicsBackendId(backend) << '\n';
  }
}

void AcceleratedSourcePhysicsParity(const std::string &packs,const std::string &scenario) {
  Program p;
  p.event("events/on-tick",{p.change("event ticks"),
      p.set("event time",p.block("simulation/read",{{"state",p.block("events/state")}},{{"property","time"}}))});
  p.define("observe steps","ticks",{p.repeat(p.get("ticks"),{
      p.block("simulation/step"),p.block("results/count"),
      p.block("results/publish",{{"score",p.binary("math/add",p.get("event time"),p.get("event ticks"))}})})});
  p.define("two steps","index",{p.repeat(p.num(2),{p.block("simulation/step")}),
      p.block("procedures/return",{{"value",p.block("simulation/snapshot")}})});
  const auto run=[&](double count) { return p.call("observe steps","ticks",{p.num(count)},false); };
  p.start({p.set("event ticks",p.num(0)),p.input(p.ms(0),p.num(1),"accelerate"),run(40),
      p.set("held",p.block("simulation/snapshot")),
      p.input(p.ms(400),p.num(32767)),p.block("simulation/set-horizon",{{"time",p.ms(1500)}}),run(10),
      p.block("simulation/restore",{{"snapshot",p.get("held")}}),run(10),
      p.input(p.ms(800),p.num(-10000)),run(50),
      p.input(p.ms(1110),p.num(1),"respawn"),p.input(p.ms(1120),p.num(0),"respawn"),run(2),
      p.set("jobs",p.block("procedures/map",{{"function",p.reference("two steps")},
          {"list",p.block("data/numbers",{{"to",p.num(2)}})}})),
      p.block("simulation/restore",{{"snapshot",p.block("data/item",{{"list",p.get("jobs")},{"index",p.num(1)}})}}),run(5),
      p.block("simulation/restart"),run(4)});
  std::vector<SearchLiveUpdate> expected;
  std::optional<VisualState> expectedStopped,expectedFeedback;
  std::optional<SearchResult> expectedLargeInputs;
  Program largeInputs;
  largeInputs.start({largeInputs.block("simulation/step"),
      largeInputs.input(largeInputs.ms(20),largeInputs.num(6789)),
      largeInputs.block("simulation/step"),largeInputs.block("simulation/step"),
      largeInputs.block("results/publish",{{"score",largeInputs.block("simulation/time")}})});
  std::vector<ParsedInputCommand> largeCommands;
  for (std::size_t i=0;i<17000;++i) {
    forevervalidator::experimental::PhysicsSandboxInputValue value;
    value.kind=forevervalidator::experimental::PhysicsSandboxInputValueKind::Analog;
    value.analog=static_cast<decltype(value.analog)>(i%2 ? 1000 : -1000);
    largeCommands.push_back({static_cast<std::int64_t>(i*10),SandboxInputAction::Steer,value,i+1});
  }
  std::vector<unsigned char> oldStaging(largeCommands.size()*sizeof(vm::Value)+256u*1024u);
  vm::Memory oldMemory; oldMemory.bytes=oldStaging.data(); oldMemory.capacity=static_cast<std::uint32_t>(oldStaging.size());
  oldMemory.allocate(vm::Kind::Inputs,static_cast<std::uint32_t>(largeCommands.size()));
  Check(oldMemory.error==vm::Error::Capacity,"Large input fixture no longer crosses the old staging boundary.");
  for (auto backend : {PhysicsBackend::Reference,PhysicsBackend::OptimizedCpu,PhysicsBackend::MultiThreadedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                       ,PhysicsBackend::Cuda
#endif
                       }) {
    for (bool compiled : {false,true}) {
      SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=2;
      request.simulationHorizonMs=2000; request.executable=std::make_shared<const VisualProgram>(p.graph);
      SearchRunControl control; control.compileVisualPrograms=compiled;
      control.sampleBestTimeline=false; control.sampleImprovementTimelines=false;
      std::vector<SearchLiveUpdate> observed;
      bool sawResident=false,lostResident=false;
      control.liveChanged=[&](const auto &value) { observed.push_back(value); };
      control.visualExecutionModeChanged=[&](const auto &value) {
        sawResident=sawResident || value=="CUDA resident physics with source block control";
        lostResident=lostResident || value=="CUDA physics with source block control: resident cursor unavailable";
      };
      RunSearch(request,&control);
      Check(observed.size()==121,"Root publications were lost.");
      if (expected.empty()) expected=observed;
      for (std::size_t i=0;i<observed.size();++i) {
        const auto label=std::string(PhysicsBackendId(backend))+" root publication "+std::to_string(i);
        Check(SameInputs(expected[i].bestInputs,observed[i].bestInputs),label+" changed inputs.");
        Check(EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)),label+" changed physical state.");
        Check(expected[i].bestScore==observed[i].bestScore && expected[i].iterations==observed[i].iterations,
            label+" changed publication/candidate ordering.");
      }
#if FOREVERVALIDATOR_HAS_CUDA
      const auto *cursorPolicy=std::getenv("FOREVERTAS_CUDA_SOURCE_CURSOR");
      const bool cursorEnabled=!cursorPolicy || std::string(cursorPolicy)!="0";
      if (backend==PhysicsBackend::Cuda)
        Check(sawResident==(compiled && cursorEnabled),"Root test did not select the intended CUDA execution path.");
#endif
      observed.clear(); control.iterationLimit=5;
      RunSearch(request,&control);
      Check(observed.size()==5,"Root candidate-limit stop changed publication ordering.");
      for (std::size_t i=0;i<observed.size();++i)
        Check(EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)),
            "Root candidate-limit stop changed a published state.");
      Program stopped;
      stopped.start({stopped.input(stopped.ms(0),stopped.num(1),"accelerate"),
          stopped.repeat(stopped.num(100),{stopped.block("simulation/step"),stopped.block("results/count")})});
      request.executable=std::make_shared<const VisualProgram>(stopped.graph);
      const auto stoppedResult=RunSearch(request,&control);
      Check(stoppedResult.bestState.timeMs==60,"Root candidate stop leaked a predicted future state.");
      if (!expectedStopped) expectedStopped=stoppedResult.bestState;
      Check(EquivalentValue(VisualValue(*expectedStopped),VisualValue(stoppedResult.bestState)),
          "Root candidate stop changed its final physical state.");
      Program feedback;
      feedback.start({feedback.input(feedback.ms(0),feedback.num(1),"accelerate"),
          feedback.repeat(feedback.num(100),{feedback.block("simulation/step"),
              feedback.input(feedback.block("simulation/time"),feedback.binary("math/subtract",feedback.num(16000),
                  feedback.binary("math/multiply",feedback.block("simulation/time"),feedback.num(32))))})});
      request.executable=std::make_shared<const VisualProgram>(feedback.graph);
      control.iterationLimit=0;
      lostResident=false;
      std::optional<std::chrono::steady_clock::time_point> feedbackStarted;
      control.progressChanged=[&](const SearchProgress &progress) {
        if (progress.stage==SearchProgressStage::ApplyingBaselineInputs && !feedbackStarted)
          feedbackStarted=std::chrono::steady_clock::now();
      };
      const auto feedbackResult=RunSearch(request,&control);
      if (std::getenv("FOREVERTAS_CUDA_VM_PROFILE") && feedbackStarted)
        std::cout << "ROOT_CONTROLLER_PROFILE backend=" << PhysicsBackendId(backend)
                  << " accelerated=" << compiled << " milliseconds="
                  << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-*feedbackStarted).count() << '\n';
      Check(feedbackResult.bestState.timeMs==1000,"Root feedback stopped early.");
#if FOREVERVALIDATOR_HAS_CUDA
      if (backend==PhysicsBackend::Cuda && compiled && cursorEnabled)
        Check(!lostResident,"Frequent input edits silently disabled the resident cursor.");
#endif
      if (!expectedFeedback) expectedFeedback=feedbackResult.bestState;
      Check(EquivalentValue(VisualValue(*expectedFeedback),VisualValue(feedbackResult.bestState)),
          "Frequent input edits changed root physical state.");
      if (compiled) {
        request.executable=std::make_shared<const VisualProgram>(largeInputs.graph);
        request.baseInputCommands=largeCommands;
        request.simulationHorizonMs=40;
        sawResident=false; lostResident=false;
        const auto largeResult=RunSearch(request,&control);
        Check(largeResult.bestState.timeMs==30 && largeResult.bestInputs.size()>=largeCommands.size(),
            "Large input staging lost events or stopped early.");
        if (!expectedLargeInputs) expectedLargeInputs=largeResult;
        Check(EquivalentValue(VisualValue(expectedLargeInputs->bestState),VisualValue(largeResult.bestState)) &&
              SameInputs(expectedLargeInputs->bestInputs,largeResult.bestInputs),
              "Large input staging or replacement changed source semantics.");
#if FOREVERVALIDATOR_HAS_CUDA
        if (backend==PhysicsBackend::Cuda && cursorEnabled)
          Check(sawResident && !lostResident,"Large input staging silently disabled the resident cursor.");
#endif
        std::cout << "PASS 17000-input staging and resident replacement: " << PhysicsBackendId(backend) << '\n';
      }
      std::cout << "PASS root physics, edits, horizons, retained restore, map handoff and callback ordering: "
                << PhysicsBackendId(backend) << (compiled ? " accelerated" : " source") << '\n';
    }
  }
}

void AcceleratedMathParity(const std::string &packs,const std::string &scenario) {
  Program p;
  const auto append=[&](Id list,Id value) { return p.block("data/append",{{"list",list},{"value",value}}); };
  const auto rotation=p.block("targets/rotation",{{"yaw",p.get("angle")},
      {"pitch",p.binary("math/divide",p.get("angle"),p.num(3))},
      {"roll",p.binary("math/multiply",p.get("angle"),p.num(2))}});
  auto values=append(append(append(append(p.block("data/list"),p.block("math/sin",{{"value",p.get("angle")}})),
      p.block("math/cos",{{"value",p.get("angle")}})),
      p.binary("math/rotation-distance",rotation,p.block("targets/rotation"))),p.block("simulation/time"));
  for (const auto *op : {"math/min","math/max"})
    values=append(values,p.binary(op,p.binary("math/multiply",p.num(-1),p.num(0)),p.num(0)));
  values=append(values,p.block("math/clamp",{{"value",p.binary("math/multiply",p.num(-1),p.num(0))},
      {"minimum",p.num(0)},{"maximum",p.num(1)}}));
  values=append(values,p.block("math/sin",{{"value",p.binary("math/multiply",p.num(-1),p.num(0))}}));
  values=append(values,p.block("math/sin",{{"value",p.num(0)}}));
  values=append(values,p.block("math/sin",{{"value",p.num(0)}}));
  p.define("scalar math","index",{
      p.set("angle",p.binary("math/divide",p.binary("math/multiply",
          p.binary("math/subtract",p.get("index"),p.num(65)),p.num(137)),p.num(7))),
      p.repeat(p.binary("math/modulo",p.get("index"),p.num(5)),{
          p.block("simulation/step"),
          p.set("angle",p.block("math/sin",{{"value",p.get("angle")}}))}),
      p.block("procedures/return",{{"value",values}})});
  p.start({p.set("jobs",p.block("procedures/map",{{"function",p.reference("scalar math")},
          {"list",p.block("data/numbers",{{"to",p.num(128)}})}})),
      p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","values"}},{{"body",{
          p.block("flow/for-each",{{"list",p.get("values")}},{{"name","value"}},{{"body",{
              p.block("results/count"),p.block("results/publish",{{"score",p.get("value")}})}}})}}})});
  std::vector<double> expected;
  const auto bits=[](double value) { std::uint64_t result; std::memcpy(&result,&value,sizeof(result)); return result; };
  for (auto backend : {PhysicsBackend::Reference,PhysicsBackend::MultiThreadedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                       ,PhysicsBackend::Cuda
#endif
                       }) {
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=4;
    request.simulationHorizonMs=100; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::Reference;
    control.sampleImprovementTimelines=false; control.sampleBestTimeline=false;
    std::vector<double> observed; std::string mode;
    control.liveChanged=[&](const auto &value) { observed.push_back(value.bestScore); };
    control.visualExecutionModeChanged=[&](const auto &value) { mode=value; };
    RunSearch(request,&control);
    Check(observed.size()==1280,"Scalar math results were lost.");
    if (expected.empty()) expected=observed;
    for (std::size_t i=0;i<observed.size();++i)
      Check(bits(expected[i])==bits(observed[i]),"Scalar math is not bit-identical on "+std::string(PhysicsBackendId(backend))+
          " at output "+std::to_string(i)+": "+std::to_string(bits(expected[i]))+" vs "+std::to_string(bits(observed[i])));
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend==PhysicsBackend::Cuda) Check(mode=="CUDA block-program kernel","Scalar math test fell back: "+mode);
#endif
    std::cout << "PASS bit-identical scalar trigonometry and rotations: " << PhysicsBackendId(backend) << '\n';
    Program infinite;
    infinite.define("math forever","index",{infinite.set("angle",infinite.get("index")),infinite.repeat(infinite.num(1e12),{
        infinite.set("angle",infinite.binary("math/add",infinite.get("angle"),infinite.num(1))),
        infinite.set("value",infinite.block("math/sin",{{"value",infinite.get("angle")}}))})});
    infinite.start({infinite.set("jobs",infinite.block("procedures/map",{
        {"function",infinite.reference("math forever")},
        {"list",infinite.block("data/numbers",{{"to",infinite.num(4)}})}}))});
    request.executable=std::make_shared<const VisualProgram>(infinite.graph);
    std::atomic_bool stop{false};
    std::optional<std::chrono::steady_clock::time_point> stoppedAt;
    control.liveChanged={};
    control.stopRequested=[&] { return stop.load(); };
    control.statisticsChanged=[&](const auto &) {
      if (!stop.exchange(true)) stoppedAt=std::chrono::steady_clock::now();
    };
    RunSearch(request,&control);
    Check(stoppedAt && std::chrono::steady_clock::now()-*stoppedAt<std::chrono::seconds(5),
        "Mapped exact math did not stop promptly on "+std::string(PhysicsBackendId(backend)));
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend==PhysicsBackend::Cuda) Check(mode=="Preparing CUDA block program","Math cancellation did not exercise the GPU kernel.");
#endif
    std::cout << "PASS exact-math cancellation: " << PhysicsBackendId(backend) << '\n';
  }
}

void AcceleratedHistoryAndCancellation(const std::string &packs,const std::string &scenario) {
  Program p;
  p.define("late feedback","index",{
      p.input(p.binary("math/multiply",p.binary("math/modulo",p.get("index"),p.num(3)),p.num(1000)),
          p.binary("math/multiply",p.get("index"),p.num(1000))),
      p.repeat(p.num(310),{p.block("simulation/step")}),
      p.set("held",p.block("simulation/snapshot")),
      p.repeat(p.num(2),{p.block("simulation/step")}),
      p.block("procedures/return",{{"value",p.get("held")}})});
  p.start({p.input(p.ms(0),p.num(1),"accelerate"),
      p.set("root",p.block("simulation/snapshot")),
      p.repeat(p.num(350),{p.block("simulation/step")}),
      p.repeat(p.num(2),{
          p.block("simulation/restore",{{"snapshot",p.get("root")}}),
          p.set("jobs",p.block("procedures/map",{{"function",p.reference("late feedback")},
              {"list",p.block("data/numbers",{{"to",p.num(9)}})}, {"workers",p.num(4)}})),
          p.block("flow/for-each",{{"list",p.get("jobs")}},{{"name","saved"}},{{"body",{
              p.block("simulation/restore",{{"snapshot",p.get("saved")}}),
              p.block("simulation/step"),p.block("results/publish",{{"score",p.block("simulation/car-speed")}})}}})})});
  std::vector<SearchLiveUpdate> expected;
  for (auto backend : {PhysicsBackend::Reference,PhysicsBackend::MultiThreadedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                      ,PhysicsBackend::Cuda
#endif
                      }) {
    SearchRequest request{packs,scenario}; request.backend=backend; request.parallelSampleCount=4;
    request.simulationHorizonMs=4000; request.executable=std::make_shared<const VisualProgram>(p.graph);
    SearchRunControl control; control.compileVisualPrograms=backend!=PhysicsBackend::Reference;
    control.sampleImprovementTimelines=false;
    std::vector<SearchLiveUpdate> observed;
    control.liveChanged=[&](const auto &value) { observed.push_back(value); };
    const auto result=RunSearch(request,&control);
    Check(observed.size()==18 && result.bestTimeline.size()==312,"Deferred checkpoint history lost a branch or tick.");
    if (expected.empty()) expected=observed;
    for (std::size_t i=0;i<observed.size();++i) {
      Check(EquivalentValue(VisualValue(expected[i].bestState),VisualValue(observed[i].bestState)),
          "Restored deferred snapshot changed physics on "+std::string(PhysicsBackendId(backend)));
      Check(SameInputs(expected[i].bestInputs,observed[i].bestInputs),"Deferred snapshot changed its inputs.");
    }

    Program infinite;
    infinite.define("loop forever","item",{infinite.block("flow/forever")});
    infinite.start({infinite.set("jobs",infinite.block("procedures/map",{{"function",infinite.reference("loop forever")},
        {"list",infinite.block("data/numbers",{{"to",infinite.num(4)}})}, {"workers",infinite.num(4)}}))});
    request.executable=std::make_shared<const VisualProgram>(infinite.graph);
    std::atomic_bool stop{false};
    std::optional<std::chrono::steady_clock::time_point> stoppedAt;
    std::string mode;
    control.liveChanged={}; control.sampleBestTimeline=false;
    control.stopRequested=[&] { return stop.load(); };
    control.visualExecutionModeChanged=[&](const auto &value) { mode=value; };
    control.statisticsChanged=[&](const auto &) {
      if (!stop.exchange(true)) stoppedAt=std::chrono::steady_clock::now();
    };
    RunSearch(request,&control);
    Check(stoppedAt && std::chrono::steady_clock::now()-*stoppedAt<std::chrono::seconds(5),
          "Empty mapped loop did not stop promptly on "+std::string(PhysicsBackendId(backend)));
#if FOREVERVALIDATOR_HAS_CUDA
    if (backend==PhysicsBackend::Cuda) Check(mode=="Preparing CUDA block program","Cancellation did not exercise the GPU kernel.");
#endif
    std::cout << "PASS deferred multi-batch snapshot continuation, full history and empty-loop cancellation: "
              << PhysicsBackendId(backend) << '\n';
  }
}

void RealSimulation(const std::string &packs,const std::string &scenario) {
  AcceleratedSimulationParity(packs,scenario);
  AcceleratedHistoryAndCancellation(packs,scenario);
  Program p;
  p.define("advance","ticks",{p.block("simulation/step",{{"ticks",p.get("ticks")}}),
      p.block("procedures/return",{{"value",p.block("simulation/state")}})});
  p.start({p.set("start",p.block("simulation/snapshot")),p.input(p.ms(0),p.num(1),"accelerate"),
      p.set("observed",p.call("advance","ticks",{p.num(8)})),
      p.block("simulation/restore",{{"snapshot",p.get("start")}}),p.input(p.ms(0),p.num(1),"accelerate"),
      p.block("simulation/step",{{"ticks",p.num(8)}}),p.block("results/publish",{{"score",p.block("simulation/time")}})});
  for (auto backend : {PhysicsBackend::Reference,PhysicsBackend::OptimizedCpu,
                      PhysicsBackend::MultiThreadedCpu
#if FOREVERVALIDATOR_HAS_CUDA
                      ,PhysicsBackend::Cuda
#endif
                      }) {
    SearchRequest request{packs,scenario};
    request.backend=backend;
    request.modifiers.clear();
    request.searchAlgorithm.id="not-a-native-search";
    request.evaluationTarget.id="not-a-native-evaluator";
    request.executable=std::make_shared<const VisualProgram>(p.graph);
    const auto result=RunSearch(request);
    Check(result.winnerSource==SearchWinnerSource::Program && result.bestState.timeMs==80 && result.bestScore==80,
          "Real simulation did not execute arbitrary blocks independently of native search components.");
    Check(result.bestTimeline.size()==9,"Real result timeline was not the program's selected branch.");
    Program mapped;
    mapped.define("advance","ticks",{mapped.block("simulation/step",{{"ticks",mapped.get("ticks")}}),
        mapped.block("procedures/return",{{"value",mapped.block("simulation/snapshot")}})});
    mapped.start({mapped.set("jobs",mapped.block("procedures/map",{{"function",mapped.reference("advance")},{"list",mapped.block("data/numbers",{{"to",mapped.num(3)}})}})),
        mapped.block("simulation/restore",{{"snapshot",mapped.block("data/item",{{"list",mapped.get("jobs")},{"index",mapped.num(2)}})}}),
        mapped.block("results/publish",{{"score",mapped.block("simulation/time")}})});
    request.executable=std::make_shared<const VisualProgram>(mapped.graph);
    request.parallelSampleCount=3;
    const auto selected=RunSearch(request);
    Check(selected.bestScore==20 && selected.bestState.timeMs==20 && selected.bestTimeline.size()==3,
          "Real mapped branch could not be returned, restored, and selected.");
    std::cout << "PASS real simulation " << PhysicsBackendId(backend) << '\n';
  }
  for (auto backend : {PhysicsBackend::Reference
#if FOREVERVALIDATOR_HAS_CUDA
                      ,PhysicsBackend::Cuda
#endif
                      }) {
    for (const auto &macro : VisualMacroCatalog()) {
      auto program=macro.program;
      if (macro.id=="bruteforce") MacroValue(program,"iterations","2");
      Id next=program.nodes.rbegin()->first;
      const auto add=[&](const std::string &type,std::map<std::string,std::string> fields,
                         std::map<std::string,Id> inputs) {
        const Id id=++next;
        program.nodes.emplace(id,VisualNode{id,type,std::move(fields),std::move(inputs),{}});
        return id;
      };
      std::vector<Id> prefix;
      for (const auto &[channel,amount] : std::vector<std::pair<std::string,std::string>>{{"steer","1000"},{"accelerate","1"},{"brake","0"}}) {
        const auto time=add("values/number",{{"value","0"}},{});
        const auto action=add("inputs/action-name",{{"value",channel}},{});
        const auto value=add("values/number",{{"value",amount}},{});
        prefix.push_back(add("simulation/set-input",{},{{"time",time},{"action",action},{"value",value}}));
      }
      auto &body=program.find(program.topLevel.front())->statements.at("body");
      body.insert(body.begin(),prefix.begin(),prefix.end());
      SearchRequest request{packs,scenario};
      request.backend=backend; request.simulationHorizonMs=80;
      request.searchAlgorithm.id="no-native-search"; request.evaluationTarget.id="no-native-evaluator"; request.modifiers.clear();
      request.executable=std::make_shared<const VisualProgram>(std::move(program));
      try {
        Check(RunSearch(request).winnerSource==SearchWinnerSource::Program,"Macro became native configuration.");
      } catch (const std::exception &error) {
        throw std::runtime_error(macro.id+" on "+std::string(PhysicsBackendId(backend))+": "+error.what());
      }
    }
    std::cout << "PASS all " << VisualMacroCatalog().size() << " expanded macroblocks on real " << PhysicsBackendId(backend) << " physics\n";
  }
}
} // namespace

int main(int argc,char **argv) {
  try {
    if (argc==4 && std::string(argv[1])=="--accelerated-parity") {
      AcceleratedSimulationParity(argv[2],argv[3]); return 0;
    }
    if (argc==4 && (std::string(argv[1])=="--accelerated-events" || std::string(argv[1])=="--accelerated-mixed")) {
      AcceleratedEventParity(argv[2],argv[3],std::string(argv[1])=="--accelerated-mixed"); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--accelerated-publication") {
      AcceleratedPublicationParity(argv[2],argv[3]); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--accelerated-initialization") {
      AcceleratedInitializationParity(argv[2],argv[3]); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--global-imports") {
      InitialGlobalImportParity(argv[2],argv[3]); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--large-imports") {
      LargeValueImportParity(argv[2],argv[3]); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--shared-imports") {
      SharedImportExportParity(argv[2],argv[3]); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--dense-prefix") {
      DensePrefixParity(argv[2],argv[3]); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--dynamic-globals") {
      DynamicGlobalImportParity(argv[2],argv[3]); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--divergence") {
      DivergentLaneParity(argv[2],argv[3]); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--output-growth") {
      OutputGrowthParity(argv[2],argv[3]); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--accelerated-snapshots") {
      AcceleratedSnapshotSharingParity(argv[2],argv[3]); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--accelerated-horizon") {
      MappedHorizonParity(argv[2],argv[3]);
      MappedHorizonParity(argv[2],argv[3],990);
      MappedHorizonParity(argv[2],argv[3],2990);
      MappedRestoreParity(argv[2],argv[3],0,true); MappedRestoreParity(argv[2],argv[3],990,true);
      MappedHorizonResources(argv[2],argv[3]);
      MappedRestoreCancellation(argv[2],argv[3],true); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--nested-map") {
      NestedMapParity(argv[2],argv[3]);
      NestedMapResources(argv[2],argv[3]);
      return 0;
    }
    if (argc==4 && std::string(argv[1])=="--mapped-history") {
      MappedHistoryImports(argv[2],argv[3]);
      for (const auto anchor : {0u,990u,2990u}) {
        MappedHistoryParity(argv[2],argv[3],anchor);
        MappedHorizonParity(argv[2],argv[3],anchor,true);
      }
      MappedHistoryResources(argv[2],argv[3]);
      MappedHistoryCancellation(argv[2],argv[3]);
      return 0;
    }
    if (argc==4 && (std::string(argv[1])=="--mapped-restart" || std::string(argv[1])=="--restart-kernel")) {
      const bool cudaOnly=std::string(argv[1])=="--restart-kernel";
#if !FOREVERVALIDATOR_HAS_CUDA
      Check(!cudaOnly,"CUDA restart checks require a CUDA-enabled build.");
#endif
      for (const auto anchor : {0u,990u,2990u}) for (bool history : {false,true})
        MappedRestartParity(argv[2],argv[3],anchor,history,cudaOnly);
      MappedRestartResources(argv[2],argv[3]);
      MappedHistoryCancellation(argv[2],argv[3],true);
      return 0;
    }
    if (argc==4 && std::string(argv[1])=="--history-imports") {
      MappedHistoryImports(argv[2],argv[3]); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--accelerated-restore") {
      MappedRestoreParity(argv[2],argv[3]); MappedRestoreParity(argv[2],argv[3],990);
      MappedRestoreCancellation(argv[2],argv[3]); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--accelerated-math") {
      AcceleratedMathParity(argv[2],argv[3]); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--accelerated-source") {
      AcceleratedSourcePhysicsParity(argv[2],argv[3]); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--source-transitions") {
      SourceTransitionParity(argv[2],argv[3]); return 0;
    }
    if (argc==4 && std::string(argv[1])=="--accelerated-history") {
      AcceleratedHistoryAndCancellation(argv[2],argv[3]); return 0;
    }
    ControlAndFunctions();
    SnapshotAndCollections();
    NovelSearch();
    RandomAndState();
    EveryReporterExecutes();
    DiagnosticsAndCancellation();
    EventScripts();
    CompiledEventScripts();
    CompiledReentrantEvents();
    CompactFrameScopes();
    InitialGlobalControlFlow();
    DynamicGlobalReachability();
    CompiledErrorParity();
    CompiledEventClock();
    PartialBatchFallback();
    ParallelBranches();
    InteractiveDebugger();
    HigherOrderAndRecovery();
    ExactInputPrimitives();
    GeometryAndEmptyRanges();
    ExpandedMacroExecution();
    CompiledMacros();
    CompiledRestore();
    HistoryReachability();
    CompiledNestedMaps();
    CompiledHistory();
    CompiledRestart();
    CompiledHorizon();
    CompiledValueBoundaries();
    ExecutorLifetimeAndCancellation();
    OperandResumeParity();
    UniformProgramProofs();
    SkyPresetConfiguration();
    MutationMacroSemantics();
    ExplicitTargetsAndPublication();
    std::cout << "PASS visual runtime: primitives, expanded macros, procedures, snapshots, custom search, debugger, cancellation\n";
    if (argc==3) RealSimulation(argv[1],argv[2]);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL visual runtime: " << error.what() << '\n';
    return 1;
  }
}
