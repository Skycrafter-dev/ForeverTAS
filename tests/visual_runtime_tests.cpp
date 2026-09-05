#include "blocks/block_value.h"
#include "blocks/visual_catalog.h"
#include "blocks/visual_compiler.h"
#include "blocks/visual_runtime.h"
#include "blocks/visual_debugger.h"
#include "blocks/visual_macros.h"
#include "searches/search_runner.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <atomic>
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
  VisualState state;
  VisualInputs events;
  std::uint32_t checkpointAtMs=0, finishAtMs=0;
  TestHost() { state.durationMs=6000; state.car.rotationW=1; }
  VisualState read() const override { return state; }
  VisualState advance() override {
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
  Check(result.published->snapshot->inputs.front().value.analog==0,"Selected inputs did not match the selected state.");
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
  MacroValue(existing,"maximum shift ms","0"); MacroValue(existing,"minimum steering","5000"); MacroValue(existing,"maximum steering","5000");
  MacroFlag(existing,"absolute steering",true); MacroFlag(existing,"toggle accelerate",false); MacroFlag(existing,"toggle brake",false);
  host=initial; ExecuteVisualProgram(existing,host,control);
  auto expected=initial.events;
  for (auto &event:expected) if (event.action==A::Steer) event.value.analog=5000;
  Check(SameInputs(host.events,expected),"Existing-event macro does not honor channel toggles, absolute mode or without-replacement selection.");
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

void RealSimulation(const std::string &packs,const std::string &scenario) {
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
    ControlAndFunctions();
    SnapshotAndCollections();
    NovelSearch();
    RandomAndState();
    EveryReporterExecutes();
    DiagnosticsAndCancellation();
    EventScripts();
    ParallelBranches();
    InteractiveDebugger();
    HigherOrderAndRecovery();
    ExactInputPrimitives();
    GeometryAndEmptyRanges();
    ExpandedMacroExecution();
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
