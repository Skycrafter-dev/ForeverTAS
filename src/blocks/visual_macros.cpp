#include "blocks/visual_macros.h"
#include "blocks/block_value.h"
#include "blocks/visual_catalog.h"

#include <stdexcept>
#include <set>
#include <utility>

namespace forevertas::blocks {
namespace {
using Id = VisualNodeId;
using Stack = std::vector<Id>;

// Every helper below builds source nodes. None is called by the interpreter.
// Expressions are freshly built at each use: graph nodes never have two parents.
class Source {
public:
  VisualProgram program;
  Stack body;

  Id block(const std::string &type, std::map<std::string, Id> inputs = {},
           std::map<std::string, std::string> fields = {},
           std::map<std::string, Stack> statements = {}) {
    const auto *definition = FindVisualBlock(type);
    if (!definition) throw std::logic_error("Macro uses unknown primitive: " + type);
    VisualNode node;
    node.id = program.nodes.size() + 1;
    node.definitionId = type;
    for (const auto &field : definition->fields) node.fields[field.key] = field.defaultValue;
    for (const auto &[key, value] : fields) node.fields[key] = value;
    node.inputs = std::move(inputs);
    node.statements = std::move(statements);
    const Id id = node.id;
    program.nodes.emplace(id, std::move(node));
    return id;
  }
  Id n(double value) { return block("values/number", {}, {{"value", FormatNumberValue(value)}}); }
  Id flag(bool value) { return block("values/boolean", {}, {{"value", value ? "true" : "false"}}); }
  Id g(const std::string &name) { return block("data/get", {}, {{"name", name}}); }
  Id set(const std::string &name, Id value) { return block("data/set", {{"value", value}}, {{"name", name}}); }
  Id local(const std::string &name, Id value) { return block("data/local", {{"value", value}}, {{"name", name}}); }
  Id op(const std::string &id, Id a, Id b) { return block(id, {{"a", a}, {"b", b}}); }
  Id unary(const std::string &id, Id value) { return block(id, {{"value", value}}); }
  Id eq(Id a, Id b) { return op("conditions/equal", a, b); }
  Id both(Id a, Id b) { return op("conditions/and", a, b); }
  Id either(Id a, Id b) { return op("conditions/or", a, b); }
  Id no(Id value) { return unary("conditions/not", value); }
  Id add(Id a, Id b) { return op("math/add", a, b); }
  Id sub(Id a, Id b) { return op("math/subtract", a, b); }
  Id mul(Id a, Id b) { return op("math/multiply", a, b); }
  Id div(Id a, Id b) { return op("math/divide", a, b); }
  Id random(Id a, Id b) { return op("math/random-integer", a, b); }
  Id now() { return block("simulation/time"); }
  Id tick() { return block("simulation/tick-duration"); }
  Id horizon() { return block("simulation/horizon"); }
  Id action(const std::string &name) { return block("inputs/action-name", {}, {{"value", name}}); }
  Id list(Stack items) {
    Id result = block("data/list");
    for (Id item : items) result = block("data/append", {{"list", result}, {"value", item}});
    return result;
  }
  Id item(Id list, Id index) { return block("data/item", {{"list", list}, {"index", index}}); }
  Id length(const std::string &name) { return block("data/length", {{"list", g(name)}}); }
  Id append(const std::string &name, Id value) {
    return set(name, block("data/append", {{"list", g(name)}, {"value", value}}));
  }
  Id numbers(Id from, Id to, Id step) { return block("data/numbers", {{"from", from}, {"to", to}, {"step", step}}); }
  Id count(const std::string &name = "inputs") { return block("inputs/count", {{"inputs", g(name)}}); }
  Id indices(const std::string &name = "inputs", bool reverse = false) {
    return reverse ? numbers(count(name), n(1), n(-1)) : numbers(n(1), count(name), n(1));
  }
  Id each(const std::string &name, Id items, Stack body) {
    return block("flow/for-each", {{"list", items}}, {{"name", name}}, {{"body", std::move(body)}});
  }
  Id repeat(Id count, Stack body) { return block("flow/repeat", {{"count", count}}, {}, {{"body", std::move(body)}}); }
  Id branch(Id condition, Stack yes, Stack otherwise = {}) {
    return block("flow/if", {{"condition", condition}}, {}, {{"body", std::move(yes)}, {"else", std::move(otherwise)}});
  }
  Id event(const std::string &property, const std::string &input = "inputs", const std::string &index = "index") {
    return block("inputs/" + property, {{"inputs", g(input)}, {"index", g(index)}});
  }
  Id read(const std::string &property, Id state = 0) {
    return block("simulation/read", {{"state", state ? state : block("simulation/state")}}, {{"property", property}});
  }
  Id inWindow(const std::string &time = "event time") {
    return both(op("conditions/greater-equal", g(time), g("first ms")),
                op("conditions/less-equal", g(time), g("last ms")));
  }
  Id clamp(Id value, Id minimum, Id maximum) {
    return block("math/clamp", {{"value", value}, {"minimum", minimum}, {"maximum", maximum}});
  }
  Id held(const std::string &inputs, const std::string &time, Id channel) {
    return block("inputs/value-at", {{"inputs", g(inputs)}, {"time", g(time)}, {"action", channel}});
  }
  Id put(Id time, Id channel, Id value) {
    return set("inputs", block("inputs/set", {{"inputs", g("inputs")}, {"time", time}, {"action", channel}, {"value", value}}));
  }
  Id remove(const std::string &index = "index") {
    return set("inputs", block("inputs/remove", {{"inputs", g("inputs")}, {"index", g(index)}}));
  }
  Id changeValue(Id value) {
    return set("inputs", block("inputs/with-value", {{"inputs", g("inputs")}, {"index", g("index")}, {"value", value}}));
  }
  void window() {
    body.push_back(local("first ms", now()));
    body.push_back(local("last ms", sub(horizon(), tick())));
    body.push_back(local("inputs", block("simulation/inputs")));
  }
  Id randomTime() {
    return mul(random(unary("math/ceil", div(g("first ms"), tick())),
                      unary("math/floor", div(g("last ms"), tick()))), tick());
  }
  Id apply() {
    return block("simulation/replace-inputs", {{"inputs", block("inputs/sort", {{"inputs", g("inputs")}})}});
  }
  Id point(double x, double y, double z) {
    return block("targets/point", {{"x", n(x)}, {"y", n(y)}, {"z", n(z)}});
  }
  Id choose(Id condition, Id score) {
    return branch(condition, {set("best score", score), set("best snapshot", block("simulation/snapshot"))});
  }
  VisualProgram finish() {
    // A for-each name is a binding, not an implicit write to a caller's global
    // variable. Declare it in the surrounding sequence so expansion works in
    // entry scripts and inside recursive user procedures alike.
    std::set<std::string> iterators;
    for (const auto &[id, node] : program.nodes) {
      (void)id;
      if (node.definitionId == "flow/for-each") iterators.insert(node.fields.at("name"));
    }
    Stack declarations;
    for (const auto &name : iterators) declarations.push_back(local(name, block("values/none")));
    body.insert(body.begin(), declarations.begin(), declarations.end());
    const Id start = block("flow/when-start", {}, {}, {{"body", std::move(body)}});
    program.topLevel.push_back(start);
    return std::move(program);
  }
};

// Match the legacy last-event-wins rule visibly, without a native normalizer.
Stack Coalesce(Source &s) {
  return {
    s.local("seen keys", s.list({})),
    s.each("index", s.indices("inputs", true), {
      s.local("key", s.list({s.event("time"), s.event("action")})),
      s.branch(s.block("data/contains", {{"list", s.g("seen keys")}, {"value", s.g("key")}}),
          {s.remove()}, {s.append("seen keys", s.g("key"))})}),
    s.set("inputs", s.block("inputs/sort", {{"inputs", s.g("inputs")}}))};
}

Stack Eligible(Source &s, Id channelCondition) {
  return {
    s.local("eligible", s.list({})),
    s.each("index", s.indices(), {
      s.local("event time", s.event("time")),
      s.branch(s.both(s.inWindow(), channelCondition), {s.append("eligible", s.g("index"))})})};
}

VisualProgram ExistingEvents() {
  Source s;
  s.window();
  for (const auto &[name, value] : std::vector<std::pair<std::string, double>>{
           {"minimum edits", 1}, {"maximum edits", 3}, {"maximum shift ms", 100},
           {"minimum steering", -9830}, {"maximum steering", 9830}})
    s.body.push_back(s.local(name, s.n(value)));
  s.body.push_back(s.local("absolute steering", s.flag(false)));
  s.body.push_back(s.local("toggle accelerate", s.flag(true)));
  s.body.push_back(s.local("toggle brake", s.flag(true)));
  auto eligible = Eligible(s, s.either(s.eq(s.event("action"), s.action("steer")),
      s.either(s.both(s.eq(s.event("action"), s.action("accelerate")), s.g("toggle accelerate")),
               s.both(s.eq(s.event("action"), s.action("brake")), s.g("toggle brake")))));
  s.body.insert(s.body.end(), eligible.begin(), eligible.end());
  // Removing the chosen list item makes selection without replacement explicit.
  s.body.push_back(s.repeat(s.op("math/min", s.random(s.g("minimum edits"), s.g("maximum edits")), s.length("eligible")), {
    s.local("choice", s.random(s.n(1), s.length("eligible"))),
    s.local("index", s.item(s.g("eligible"), s.g("choice"))),
    s.set("eligible", s.block("data/delete-item", {{"list", s.g("eligible")}, {"index", s.g("choice")}})),
    s.local("shift ticks", s.unary("math/floor", s.div(s.g("maximum shift ms"), s.tick()))),
    s.set("inputs", s.block("inputs/with-time", {{"inputs", s.g("inputs")}, {"index", s.g("index")},
      {"time", s.clamp(s.add(s.event("time"), s.mul(s.random(s.sub(s.n(0), s.g("shift ticks")), s.g("shift ticks")), s.tick())), s.g("first ms"), s.g("last ms"))}})),
    s.branch(s.eq(s.event("action"), s.action("steer")), {
      s.local("steering draw", s.random(s.g("minimum steering"), s.g("maximum steering"))),
      s.branch(s.g("absolute steering"), {s.changeValue(s.g("steering draw"))},
          {s.changeValue(s.clamp(s.add(s.event("value"), s.g("steering draw")), s.n(-65536), s.n(65536)))})
    }, {s.changeValue(s.sub(s.n(1), s.event("value")))})}));
  auto normalize = Coalesce(s);
  s.body.insert(s.body.end(), normalize.begin(), normalize.end());
  s.body.push_back(s.apply());
  return s.finish();
}

VisualProgram Deletion() {
  Source s;
  s.window();
  for (const auto &channel : {"steering", "accelerate", "brake"}) {
    s.body.push_back(s.local(std::string("delete ") + channel, s.flag(true)));
    s.body.push_back(s.local(std::string("maximum ") + channel + " deletions", s.n(1)));
  }
  s.body.push_back(s.local("channels", s.list({s.action("steer"), s.action("accelerate"), s.action("brake")})));
  s.body.push_back(s.local("enabled channels", s.list({s.g("delete steering"), s.g("delete accelerate"), s.g("delete brake")})));
  s.body.push_back(s.local("deletion limits", s.list({s.g("maximum steering deletions"), s.g("maximum accelerate deletions"), s.g("maximum brake deletions")})));
  auto attempt = Eligible(s, s.eq(s.event("action"), s.g("channel")));
  attempt.push_back(s.branch(s.eq(s.length("eligible"), s.n(0)), {s.block("flow/break")}));
  attempt.push_back(s.local("index", s.item(s.g("eligible"), s.random(s.n(1), s.length("eligible")))));
  attempt.push_back(s.remove());
  s.body.push_back(s.each("channel index", s.numbers(s.n(1), s.n(3), s.n(1)), {
    s.local("channel", s.item(s.g("channels"), s.g("channel index"))),
    s.branch(s.item(s.g("enabled channels"), s.g("channel index")), {
      s.repeat(s.random(s.n(0), s.item(s.g("deletion limits"), s.g("channel index"))), std::move(attempt))})}));
  s.body.push_back(s.apply());
  return s.finish();
}

VisualProgram Insertion() {
  Source s;
  s.window();
  s.body.push_back(s.local("original inputs", s.g("inputs")));
  for (const auto &channel : {"steering", "accelerate", "brake"}) {
    s.body.push_back(s.local(std::string("insert ") + channel, s.flag(true)));
    for (const auto &bound : {"minimum", "maximum"})
      s.body.push_back(s.local(std::string(bound) + " " + channel + " insertions", s.n(1)));
    s.body.push_back(s.local(std::string("maximum ") + channel + " hold ms", s.n(100)));
  }
  s.body.push_back(s.local("steering is offset", s.flag(false)));
  s.body.push_back(s.local("minimum steering", s.n(-65536)));
  s.body.push_back(s.local("maximum steering", s.n(65536)));
  s.body.push_back(s.local("channels", s.list({s.action("steer"), s.action("accelerate"), s.action("brake")})));
  s.body.push_back(s.local("enabled channels", s.list({s.g("insert steering"), s.g("insert accelerate"), s.g("insert brake")})));
  s.body.push_back(s.local("minimum insertions", s.list({s.g("minimum steering insertions"), s.g("minimum accelerate insertions"), s.g("minimum brake insertions")})));
  s.body.push_back(s.local("maximum insertions", s.list({s.g("maximum steering insertions"), s.g("maximum accelerate insertions"), s.g("maximum brake insertions")})));
  s.body.push_back(s.local("maximum holds", s.list({s.g("maximum steering hold ms"), s.g("maximum accelerate hold ms"), s.g("maximum brake hold ms")})));
  Stack insertion{
    s.local("start ms", s.randomTime()),
    s.local("end ms", s.op("math/min", s.g("last ms"), s.add(s.g("start ms"), s.mul(
      s.random(s.n(0), s.unary("math/floor", s.div(s.item(s.g("maximum holds"), s.g("channel index")), s.tick()))), s.tick())))),
    s.local("previous value", s.held("inputs", "start ms", s.g("channel"))),
    s.local("new value", s.n(0)),
    s.branch(s.eq(s.g("channel"), s.action("steer")), {
      s.set("new value", s.random(s.g("minimum steering"), s.g("maximum steering"))),
      s.branch(s.g("steering is offset"), {s.set("new value", s.clamp(s.add(s.g("previous value"), s.g("new value")), s.n(-65536), s.n(65536)))})
    }, {s.set("new value", s.sub(s.n(1), s.g("previous value")))}),
    // A hold is two edits plus an explicit removal loop, never one opaque atom.
    s.each("index", s.indices("inputs", true), {
      s.branch(s.both(s.eq(s.event("action"), s.g("channel")),
        s.both(s.op("conditions/greater-equal", s.event("time"), s.g("start ms")),
               s.op("conditions/less-equal", s.event("time"), s.g("end ms")))), {s.remove()})}),
    s.put(s.g("start ms"), s.g("channel"), s.g("new value")),
    s.branch(s.op("conditions/greater", s.g("end ms"), s.g("start ms")), {
      s.put(s.g("end ms"), s.g("channel"), s.held("original inputs", "end ms", s.g("channel")))})};
  s.body.push_back(s.branch(s.op("conditions/less-equal", s.g("first ms"), s.g("last ms")), {
    s.each("channel index", s.numbers(s.n(1), s.n(3), s.n(1)), {
      s.local("channel", s.item(s.g("channels"), s.g("channel index"))),
      s.branch(s.item(s.g("enabled channels"), s.g("channel index")), {
        s.repeat(s.random(s.item(s.g("minimum insertions"), s.g("channel index")), s.item(s.g("maximum insertions"), s.g("channel index"))), std::move(insertion))})})}));
  s.body.push_back(s.apply());
  return s.finish();
}

VisualProgram Reroll() {
  Source s;
  s.window();
  s.body.push_back(s.each("index", s.indices(), {
    s.local("event time", s.event("time")),
    s.branch(s.both(s.inWindow(), s.eq(s.event("action"), s.action("steer"))), {
      s.local("draw", s.random(s.n(-65536), s.n(65536))),
      s.branch(s.eq(s.g("draw"), s.event("value")), {
        s.branch(s.eq(s.g("draw"), s.n(65536)), {s.set("draw", s.n(-65536))}, {s.set("draw", s.n(65536))})}),
      s.changeValue(s.g("draw"))})}));
  s.body.push_back(s.apply());
  return s.finish();
}

VisualProgram SmoothSteering() {
  Source s;
  s.window();
  s.body.push_back(s.local("original inputs", s.g("inputs")));
  s.body.push_back(s.local("deformations", s.n(1)));
  s.body.push_back(s.local("radius ms", s.n(100)));
  s.body.push_back(s.local("minimum amplitude", s.n(-9830)));
  s.body.push_back(s.local("maximum amplitude", s.n(9830)));
  s.body.push_back(s.branch(s.op("conditions/less-equal", s.g("first ms"), s.g("last ms")), {
    s.repeat(s.g("deformations"), {
      s.local("center ms", s.randomTime()),
      s.local("amplitude", s.random(s.g("minimum amplitude"), s.g("maximum amplitude"))),
      s.local("start ms", s.op("math/max", s.g("first ms"), s.sub(s.g("center ms"), s.g("radius ms")))),
      s.local("end ms", s.op("math/min", s.g("last ms"), s.add(s.g("center ms"), s.g("radius ms")))),
      s.each("sample ms", s.numbers(s.mul(s.unary("math/floor", s.div(s.g("start ms"), s.tick())), s.tick()), s.g("end ms"), s.tick()), {
        s.local("weight", s.n(1)),
        s.branch(s.op("conditions/greater", s.g("radius ms"), s.n(0)), {
          s.set("weight", s.mul(s.n(0.5), s.add(s.n(1), s.unary("math/cos",
            s.div(s.mul(s.n(180), s.unary("math/abs", s.sub(s.g("sample ms"), s.g("center ms")))), s.g("radius ms"))))))}),
        s.put(s.g("sample ms"), s.action("steer"), s.clamp(s.add(s.held("original inputs", "sample ms", s.action("steer")),
          s.unary("math/round", s.mul(s.g("amplitude"), s.g("weight")))), s.n(-65536), s.n(65536)))}),
      s.local("restore ms", s.mul(s.add(s.unary("math/floor", s.div(s.g("end ms"), s.tick())), s.n(1)), s.tick())),
      s.put(s.g("restore ms"), s.action("steer"), s.held("original inputs", "restore ms", s.action("steer")))})}));
  s.body.push_back(s.apply());
  return s.finish();
}

VisualProgram Condition(const std::string &kind) {
  Source s;
  if (kind == "time") {
    s.body = {s.local("first ms", s.n(1000)), s.local("last ms", s.horizon()),
      s.local("event time", s.now()), s.local("condition", s.inWindow())};
  } else if (kind == "checkpoints") {
    s.body = {s.local("required checkpoints", s.n(1)), s.local("required laps", s.n(0)),
      s.local("condition", s.both(s.op("conditions/greater-equal", s.read("checkpoints"), s.g("required checkpoints")),
        s.op("conditions/greater-equal", s.read("laps"), s.g("required laps"))))};
  } else if (kind == "contact") {
    s.body = {s.local("wheel index", s.n(1)), s.local("required surface", s.n(0)),
      s.local("condition", s.both(s.item(s.read("wheel-contact"), s.g("wheel index")),
        s.eq(s.item(s.read("wheel-surface"), s.g("wheel index")), s.g("required surface"))))};
  } else {
    s.body = {s.local("minimum speed (m/s)", s.n(0)), s.local("until ms", s.horizon()), s.local("passed", s.flag(true)),
      s.block("flow/while", {{"condition", s.op("conditions/less", s.now(), s.g("until ms"))}}, {}, {{"body", {
        s.local("condition", s.op("conditions/greater-equal", s.read("speed"), s.g("minimum speed (m/s)"))),
        s.branch(s.no(s.g("condition")), {s.set("passed", s.flag(false)), s.block("flow/break")}),
        s.block("simulation/step")}}})};
  }
  return s.finish();
}

VisualProgram Target(const std::string &kind) {
  Source s;
  s.body = {s.local("first ms", s.now()), s.local("last ms", s.horizon()),
            s.local("best score", s.block("values/none")), s.local("best snapshot", s.block("values/none"))};
  const bool entry = kind == "box" || kind == "prism";
  const bool first = entry || kind == "finish";
  const bool maximize = kind == "speed" || kind == "direction" || kind == "stunts";
  if (kind == "point" || kind == "pose") s.body.push_back(s.local("target point", s.point(0,0,0)));
  if (kind == "pose") {
    s.body.push_back(s.local("target rotation", s.block("targets/rotation", {{"yaw", s.n(0)}, {"pitch", s.n(0)}, {"roll", s.n(0)}})));
    s.body.push_back(s.local("position weight", s.n(1)));
    s.body.push_back(s.local("rotation weight (per radian)", s.n(1)));
  }
  if (kind == "direction") s.body.push_back(s.local("direction", s.point(1,0,0)));
  if (entry) {
    s.body.push_back(s.local("origin", s.point(0,0,0)));
    if (kind == "box") s.body.push_back(s.local("volume", s.block("targets/box", {
      {"center", s.g("origin")}, {"size", s.block("targets/size", {{"x", s.n(10)}, {"y", s.n(10)}, {"z", s.n(10)}})}})));
    else {
      s.body.push_back(s.local("points", s.list({s.point(-5,-5,0), s.point(5,-5,0), s.point(0,5,0)})));
      s.body.push_back(s.local("volume", s.block("targets/prism", {{"origin", s.g("origin")}, {"depth", s.n(5)},
        {"polygon", s.block("targets/polygon-from-points", {{"points", s.g("points")}})}}, {{"plane", "xz"}})));
    }
  }
  Id score;
  if (entry) score = s.now();
  else if (kind == "point" || kind == "pose") {
    score = s.op("math/distance", s.read("position"), s.g("target point"));
    if (kind == "pose") score = s.add(s.mul(score, s.g("position weight")),
      s.mul(s.op("math/rotation-distance", s.read("rotation"), s.g("target rotation")), s.g("rotation weight (per radian)")));
  } else if (kind == "direction") score = s.op("math/dot", s.read("velocity"), s.unary("math/normalize", s.g("direction")));
  else score = s.read(kind == "finish" ? "finish-upper-bound" : kind == "stunts" ? "stunt-points" : "speed");
  Id qualifies = entry ? s.block("conditions/inside", {{"position", s.read("position")}, {"volume", s.g("volume")}}) : s.flag(true);
  Stack evaluate{s.local("score", score)};
  evaluate.push_back(s.branch(s.unary("data/has-value", s.g("score")), {
    s.choose(s.either(s.no(s.unary("data/has-value", s.g("best score"))),
      s.op(maximize ? "conditions/greater" : "conditions/less", s.g("score"), s.g("best score"))), s.g("score"))}));
  if (first) evaluate.push_back(s.branch(s.unary("data/has-value", s.g("best snapshot")), {s.block("flow/break")}));
  s.body.push_back(s.block("flow/forever", {}, {}, {{"body", {
    s.local("event time", s.now()), s.branch(s.both(s.inWindow(), qualifies), std::move(evaluate)),
    s.branch(s.either(s.op("conditions/greater-equal", s.now(), s.g("last ms")),
                     s.op("conditions/greater-equal", s.now(), s.horizon())), {s.block("flow/break")}),
    s.block("simulation/step")}}}));
  s.body.push_back(s.branch(s.unary("data/has-value", s.g("best snapshot")), {
    s.block("results/publish-snapshot", {{"snapshot", s.g("best snapshot")}, {"score", s.g("best score")}})}));
  return s.finish();
}

VisualProgram BruteForce() {
  Source s;
  s.body = {s.local("iterations", s.n(100)), s.local("seed", s.n(1)),
    s.block("math/seed", {{"value", s.g("seed")}}),
    s.local("origin", s.block("simulation/snapshot")), s.local("baseline inputs", s.block("simulation/inputs")),
    s.local("promote best", s.flag(true)), s.block("results/clear"),
    s.repeat(s.g("iterations"), {
      s.block("results/count"), s.block("simulation/restore", {{"snapshot", s.g("origin")}}),
      s.block("simulation/replace-inputs", {{"inputs", s.g("baseline inputs")}}),
      s.block("simulation/set-input", {{"time", s.now()}, {"action", s.action("steer")}, {"value", s.random(s.n(-65536), s.n(65536))}}),
      s.block("flow/while", {{"condition", s.op("conditions/less", s.now(), s.horizon())}}, {},
        {{"body", {s.block("simulation/step")}}}),
      s.local("score", s.read("speed")),
      s.branch(s.either(s.no(s.block("results/has-result")), s.op("conditions/greater", s.g("score"), s.block("results/best-score"))), {
        s.block("results/publish", {{"score", s.g("score")}}),
        s.branch(s.g("promote best"), {s.set("baseline inputs", s.block("simulation/inputs"))})})})};
  return s.finish();
}
} // namespace

const std::vector<VisualMacro> &VisualMacroCatalog() {
  static const std::vector<VisualMacro> macros{
    {"existing-events", "Inputs", "Existing-event mutation", "Choose without replacement, shift one timestamp, edit one value, resolve collisions, then sort. Steering values are analog integers.", ExistingEvents()},
    {"input-deletion", "Inputs", "Input deletion", "Draw a deletion count for each enabled channel; explicitly find and delete one eligible event at a time.", Deletion()},
    {"input-insertion", "Inputs", "Input insertion and holds", "Choose each channel's count, start and hold time; remove intervening events, insert the new value and restore the original value.", Insertion()},
    {"random-steering", "Inputs", "Reroll existing steering", "Visit each steering event in the window and choose a different analog value.", Reroll()},
    {"smooth-steering", "Inputs", "Smooth steering deformation", "Build a cosine bump one tick at a time from the original input sequence, then restore the original steering after its end.", SmoothSteering()},
    {"time-condition", "Conditions", "Time-window condition", "Compare the current time against two editable bounds. The result is the variable condition.", Condition("time")},
    {"checkpoint-condition", "Conditions", "Checkpoint and lap condition", "Read checkpoint and lap counters and combine two explicit comparisons.", Condition("checkpoints")},
    {"contact-condition", "Conditions", "Wheel contact and surface", "Read a selected wheel's contact and surface; combine those tests visibly.", Condition("contact")},
    {"require-each-tick", "Conditions", "Require a condition each tick", "Evaluate the condition, stop on rejection, otherwise advance one tick. Edit the comparison or place extra actions inside the loop.", Condition("require")},
    {"speed-target", "Targets", "Maximum speed", "Step and compare speed at each eligible time, keep a snapshot when it improves, then publish that snapshot.", Target("speed")},
    {"direction-target", "Targets", "Directional speed", "Normalize an editable direction, take a velocity dot product and select its maximum over explicit ticks.", Target("direction")},
    {"point-target", "Targets", "Closest point", "Calculate distance to an editable point and compare candidate snapshots explicitly.", Target("point")},
    {"pose-target", "Targets", "Position and rotation", "Combine weighted position distance and quaternion angular distance (radians) with ordinary arithmetic.", Target("pose")},
    {"box-target", "Targets", "First box entry", "Construct a box, test each position, record the first qualifying snapshot and stop scanning.", Target("box")},
    {"prism-target", "Targets", "First polygon-prism entry", "Build a polygon from a visible list of points, construct a prism, then test and select its first entry.", Target("prism")},
    {"finish-target", "Targets", "Precise finish time", "Check for an available finish-time upper bound in nanoseconds; publish its source snapshot without inventing a zero for missing data.", Target("finish")},
    {"stunt-target", "Targets", "Maximum stunt points", "Ignore unavailable stunt scores and explicitly retain the snapshot with the highest score.", Target("stunts")},
    {"bruteforce", "Search", "Bruteforce from primitives", "Repeat restore, edit, step, compare and publish. Replace the one steering edit with any input macroblock and edit the scoring logic.", BruteForce()}
  };
  return macros;
}
} // namespace forevertas::blocks
