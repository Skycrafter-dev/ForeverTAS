#include "blocks/visual_catalog.h"

#include "blocks/visual_runtime.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <utility>

namespace forevertas::blocks {
namespace {

VisualInputDefinition Input(std::string key, std::string label,
                            VisualValueType type, std::string defaultBlock = {},
                            std::string defaultValue = {}) {
  return {std::move(key), std::move(label), type, std::move(defaultBlock),
          std::move(defaultValue)};
}

VisualBlockDefinition Reporter(std::string id, std::string category,
                               std::string label, VisualValueType output,
                               std::vector<VisualInputDefinition> inputs = {}) {
  VisualBlockDefinition result;
  result.id = std::move(id);
  result.blocklyType = VisualBlocklyType(result.id);
  result.categoryId = std::move(category);
  result.label = std::move(label);
  result.shape = VisualBlockShape::Reporter;
  result.outputType = output;
  result.inputs = std::move(inputs);
  return result;
}

VisualBlockDefinition
Predicate(std::string id, std::string category, std::string label,
          std::vector<VisualInputDefinition> inputs = {}) {
  VisualBlockDefinition result =
      Reporter(std::move(id), std::move(category), std::move(label),
               VisualValueType::Boolean, std::move(inputs));
  result.shape = VisualBlockShape::Predicate;
  return result;
}

VisualBlockDefinition Command(std::string id, std::string category,
                              std::string label,
                              std::vector<VisualInputDefinition> inputs = {},
                              std::string statementFamily = {}) {
  VisualBlockDefinition result;
  result.id = std::move(id);
  result.blocklyType = VisualBlocklyType(result.id);
  result.categoryId = std::move(category);
  result.label = std::move(label);
  result.shape = VisualBlockShape::Command;
  result.statementFamily = std::move(statementFamily);
  result.inputs = std::move(inputs);
  return result;
}

VisualBlockDefinition Literal(std::string id, std::string category,
                              std::string label, VisualValueType output,
                              VisualFieldDefinition::Kind kind,
                              std::string defaultValue) {
  auto result =
      Reporter(std::move(id), std::move(category), std::move(label), output);
  result.fields.push_back({"value", "", kind, std::move(defaultValue), {}});
  return result;
}

} // namespace

std::string VisualBlocklyType(const std::string &definitionId) {
  std::string result = "ft_";
  result.reserve(definitionId.size() + 3);
  for (const char value : definitionId) {
    if (std::isalnum(static_cast<unsigned char>(value))) {
      result.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
    } else {
      result.push_back('_');
    }
  }
  return result;
}

const std::vector<VisualCategory> &VisualCategories() {
  static const std::vector<VisualCategory> categories{
      {"flow", "Flow", "#e69024"},
      {"data", "Variables & lists", "#d98928"},
      {"procedures", "My blocks", "#c65291"},
      {"inputs", "Inputs", "#7a4bd1"},
      {"simulation", "Simulation", "#3278c8"},
      {"conditions", "Conditions", "#43a85b"},
      {"targets", "Targets", "#159e8c"},
      {"math", "Math", "#59a64a"},
      {"values", "Values", "#6c78c7"}};
  return categories;
}

const std::vector<VisualBlockDefinition> &VisualBlockCatalog() {
  static const std::vector<VisualBlockDefinition> definitions = [] {
    std::vector<VisualBlockDefinition> result;

    VisualBlockDefinition start;
    start.id = "flow/when-start";
    start.blocklyType = VisualBlocklyType(start.id);
    start.categoryId = "flow";
    start.label = "when run starts";
    start.shape = VisualBlockShape::Hat;
    start.statements.push_back({"body", "", "root-command"});
    result.push_back(std::move(start));

    using T = VisualValueType;
    using F = VisualFieldDefinition;
    const auto number = [](std::string key, std::string label, std::string value = "0") {
      return Input(std::move(key), std::move(label), T::Scalar, "values/number", std::move(value));
    };
    const auto named = [](VisualBlockDefinition block, std::string name = "value") {
      block.fields.push_back({"name", "", F::Kind::Text, std::move(name), {}});
      return block;
    };
    for (const auto &[id, label] : std::vector<std::pair<std::string, std::string>>{
             {"on-tick", "after each simulation tick"}, {"on-checkpoint", "when checkpoint collected"},
             {"on-finish", "when race finishes"}, {"on-message", "when I receive"},
             {"when", "when condition becomes true"}}) {
      auto event = Command("events/"+id, "flow", label);
      event.shape = VisualBlockShape::Hat;
      event.statements.push_back({"body", "", "command"});
      if (id == "on-message") event = named(std::move(event), "message");
      if (id == "when") event.inputs.push_back(Input("condition", "", T::Boolean, "values/boolean", "false"));
      result.push_back(std::move(event));
    }
    result.push_back(Command("events/send", "flow", "broadcast and wait",
        {Input("message", "message", T::Text, "values/text", "message"),
         Input("value", "with", T::Any, "values/number", "0")}));
    result.push_back(Reporter("events/value", "flow", "event value", T::Any));
    result.push_back(Reporter("events/state", "simulation", "event state", T::State));
    result.push_back(Command("debug/pause", "flow", "pause here"));
    auto each = named(Command("flow/for-each", "flow", "for each",
        {Input("list", "in", T::List, "data/list")}), "item");
    each.shape = VisualBlockShape::Control;
    each.statements.push_back({"body", "", "command"});
    result.push_back(std::move(each));
    result.push_back(Reporter("data/numbers", "data", "numbers", T::List,
        {number("from", "from", "1"), number("to", "to", "10"), number("step", "step", "1")}));
    result.push_back(named(Reporter("procedures/reference", "procedures", "block", T::Procedure), "my block"));
    result.push_back(Reporter("procedures/map", "procedures", "map independent branches", T::List,
        {Input("function", "using", T::Procedure, "procedures/reference"), Input("list", "over", T::List, "data/list"),
         Input("workers", "workers", T::Integer, "runtime/workers")}));
    result.push_back(Reporter("runtime/workers", "procedures", "backend worker count", T::Integer));
    for (const auto &id : {"apply", "do"}) {
      auto inputs = std::vector<VisualInputDefinition>{Input("function", "block", T::Procedure, "procedures/reference"),
          Input("arguments", "arguments", T::List, "data/list")};
      result.push_back(std::string(id)=="apply"
          ? Reporter("procedures/apply", "procedures", "call", T::Any, inputs)
          : Command("procedures/do", "procedures", "run", inputs));
    }
    auto attempt = named(Command("flow/try", "flow", "try"), "error");
    attempt.shape = VisualBlockShape::Control;
    attempt.statements = {{"body", "", "command"}, {"error", "on error", "command"}};
    result.push_back(std::move(attempt));
    for (const auto &[id, label] : std::vector<std::pair<std::string, std::string>>{
             {"repeat", "repeat"}, {"while", "while"}, {"until", "repeat until"},
             {"forever", "forever"}, {"if", "if"}}) {
      auto block = Command("flow/" + id, "flow", label);
      block.shape = VisualBlockShape::Control;
      if (id == "repeat") block.inputs.push_back(number("count", "times", "10"));
      else if (id != "forever") block.inputs.push_back(
          Input("condition", "", T::Boolean, "values/boolean", "false"));
      block.statements.push_back({"body", "", "command"});
      if (id == "if") block.statements.push_back({"else", "else", "command"});
      result.push_back(std::move(block));
    }
    for (const auto &id : {"break", "continue", "stop"})
      result.push_back(Command(std::string("flow/") + id, "flow", id));

    result.push_back(named(Command("data/set", "data", "set",
        {Input("value", "to", T::Any, "values/number", "0")})));
    result.push_back(named(Command("data/local", "data", "local",
        {Input("value", "=", T::Any, "values/number", "0")})));
    result.push_back(named(Command("data/change", "data", "change", {number("value", "by", "1")})));
    result.push_back(named(Reporter("data/get", "data", "", T::Any)));
    result.push_back(Predicate("data/has-value", "data", "has value",
        {Input("value", "", T::Any)}));
    result.push_back(Reporter("data/list", "data", "empty list", T::List));
    result.push_back(Reporter("data/append", "data", "add to list", T::List,
        {Input("list", "", T::List, "data/list"), Input("value", "item", T::Any, "values/number", "0")}));
    result.push_back(Reporter("data/length", "data", "length", T::Integer,
        {Input("list", "of", T::List, "data/list")}));
    result.push_back(Reporter("data/item", "data", "item", T::Any,
        {number("index", "", "1"), Input("list", "of", T::List, "data/list")}));
    result.push_back(Predicate("data/contains", "data", "list contains",
        {Input("list", "", T::List, "data/list"), Input("value", "item", T::Any, "values/number", "0")}));
    for (const auto &id : {"replace-item", "delete-item"}) {
      auto block = Reporter(std::string("data/") + id, "data", id, T::List,
          {Input("list", "in", T::List, "data/list"), number("index", "item", "1")});
      if (std::string(id) == "replace-item") block.inputs.push_back(Input("value", "with", T::Any, "values/number", "0"));
      result.push_back(std::move(block));
    }
    result.push_back(Literal("values/text", "values", "text", T::Text, F::Kind::Text, ""));
    result.push_back(Reporter("values/none", "values", "no value", T::Any));
    auto define = named(Command("procedures/define", "procedures", "define"), "my block");
    define.shape = VisualBlockShape::Hat;
    define.fields.push_back({"parameters", "parameters", F::Kind::Text, "", {}});
    define.statements.push_back({"body", "", "command"});
    result.push_back(std::move(define));
    for (const auto &id : {"call", "value"}) {
      auto call = named(std::string(id) == "call"
          ? Command("procedures/call", "procedures", "run")
          : Reporter("procedures/value", "procedures", "value of", T::Any), "my block");
      call.fields.push_back({"parameters", "", F::Kind::Text, "", {}});
      result.push_back(std::move(call));
    }
    result.push_back(Command("procedures/return", "procedures", "return",
        {Input("value", "", T::Any, "values/number", "0")}));

    result.push_back(Command("simulation/step", "simulation", "advance one tick"));
    result.push_back(Command("simulation/restart", "simulation", "restart with current inputs"));
    result.push_back(Command("simulation/set-horizon", "simulation", "set horizon",
        {Input("time", "ms", T::Milliseconds, "values/milliseconds", "6000")}));
    result.push_back(Reporter("simulation/snapshot", "simulation", "save simulation", T::Snapshot));
    result.push_back(Command("simulation/restore", "simulation", "restore",
        {Input("snapshot", "", T::Snapshot)}));
    result.push_back(Reporter("simulation/state", "simulation", "current state", T::State));
    result.push_back(Reporter("simulation/horizon", "simulation", "simulation horizon", T::Milliseconds));
    result.push_back(Reporter("simulation/tick-duration", "simulation", "tick duration (ms)", T::Milliseconds));
    result.push_back(Reporter("simulation/previous-state", "simulation", "previous state", T::State));
    result.push_back(Reporter("simulation/history", "simulation", "sampled states", T::List));
    result.push_back(Reporter("simulation/snapshot-state", "simulation", "state of", T::State,
        {Input("snapshot", "", T::Snapshot)}));
    auto read = Reporter("simulation/read", "simulation", "read", T::Any,
        {Input("state", "of", T::State, "simulation/state")});
    read.fields.push_back({"property", "", F::Kind::Enum, "time", VisualStateProperties()});
    result.push_back(std::move(read));
    result.push_back(Reporter("simulation/inputs", "inputs", "current inputs", T::Inputs));
    result.push_back(Reporter("simulation/snapshot-inputs", "inputs", "inputs of", T::Inputs,
        {Input("snapshot", "", T::Snapshot)}));
    result.push_back(Command("simulation/replace-inputs", "inputs", "use inputs",
        {Input("inputs", "", T::Inputs, "simulation/inputs")}));
    result.push_back(Reporter("inputs/empty", "inputs", "empty inputs", T::Inputs));
    result.push_back(Reporter("inputs/count", "inputs", "input count", T::Integer,
        {Input("inputs", "", T::Inputs, "simulation/inputs")}));
    for (const auto &property : {"time", "value", "action"}) {
      result.push_back(Reporter(std::string("inputs/") + property, "inputs",
          std::string("input ") + property, std::string(property) == "action" ? T::Text : T::Scalar,
          {Input("inputs", "in", T::Inputs, "simulation/inputs"), number("index", "item", "1")}));
    }
    result.push_back(Reporter("inputs/remove", "inputs", "remove input", T::Inputs,
        {Input("inputs", "from", T::Inputs, "simulation/inputs"), number("index", "item", "1")}));
    result.push_back(Reporter("inputs/sort", "inputs", "sort inputs by time", T::Inputs,
        {Input("inputs", "", T::Inputs, "simulation/inputs")}));
    result.push_back(Reporter("inputs/with-time", "inputs", "move one input", T::Inputs,
        {Input("inputs", "in", T::Inputs, "simulation/inputs"), number("index", "item", "1"),
         Input("time", "to time", T::Milliseconds, "simulation/time")}));
    result.push_back(Reporter("inputs/with-value", "inputs", "change one input value", T::Inputs,
        {Input("inputs", "in", T::Inputs, "simulation/inputs"), number("index", "item", "1"), number("value", "to")}));
    result.push_back(Reporter("inputs/value-at", "inputs", "held input value", T::Scalar,
        {Input("inputs", "in", T::Inputs, "simulation/inputs"), Input("action", "action", T::Text, "inputs/action-name"),
         Input("time", "at", T::Milliseconds, "simulation/time")}));
    for (const auto &id : {"inputs/set", "simulation/set-input"}) {
      auto block = std::string(id) == "inputs/set"
          ? Reporter(id, "inputs", "set input", T::Inputs,
              {Input("inputs", "in", T::Inputs, "simulation/inputs")})
          : Command(id, "inputs", "set input");
      block.inputs.push_back(Input("time", "at", T::Milliseconds, "simulation/time"));
      block.inputs.push_back(Input("action", "action", T::Text, "inputs/action-name"));
      block.inputs.push_back(number("value", "to", "0"));
      result.push_back(std::move(block));
    }
    auto action = Reporter("inputs/action-name", "inputs", "", T::Text);
    action.fields.push_back({"value", "", F::Kind::Enum, "steer",
        {{"steer", "steering (-65536…65536)"}, {"accelerate", "accelerate (0/1)"},
         {"brake", "brake (0/1)"}, {"left", "left (0/1)"}, {"right", "right (0/1)"},
         {"respawn", "respawn (0/1)"}, {"gas", "gas (-65536…65536)"}}});
    result.push_back(std::move(action));

    result.push_back(Command("results/publish", "flow", "keep this result", {number("score", "score")}));
    result.push_back(Command("results/publish-snapshot", "flow", "keep saved result",
        {Input("snapshot", "snapshot", T::Snapshot, "simulation/snapshot"), number("score", "score")}));
    result.push_back(Reporter("results/best-score", "flow", "kept score", T::Scalar));
    result.push_back(Predicate("results/has-result", "flow", "has kept result"));
    result.push_back(Reporter("results/snapshot", "flow", "kept result", T::Snapshot));
    result.push_back(Command("results/clear", "flow", "forget kept result"));
    result.push_back(Reporter("results/iterations", "flow", "candidate count", T::Integer));
    result.push_back(Command("results/count", "flow", "count candidate"));
    result.push_back(Command("math/seed", "math", "set random seed", {number("value", "", "1")}));
    result.push_back(Reporter("math/random", "math", "random", T::Scalar,
        {number("a", "from"), number("b", "to", "1")}));
    result.push_back(Reporter("math/random-integer", "math", "random integer", T::Integer,
        {number("a", "from"), number("b", "to", "10")}));
    for (const auto &id : {"floor", "ceil", "round", "sqrt", "sin", "cos"})
      result.push_back(Reporter(std::string("math/") + id, "math", id, T::Scalar, {number("value", "")}));
    result.push_back(Reporter("math/modulo", "math", "mod", T::Scalar,
        {number("a", ""), number("b", "", "1")}));
    auto component = Reporter("math/component", "math", "component", T::Scalar,
        {Input("value", "of", T::Vector3, "targets/direction")});
    component.fields.push_back({"axis", "", F::Kind::Enum, "x", {{"x", "x"}, {"y", "y"}, {"z", "z"}}});
    result.push_back(std::move(component));
    for (const auto &id : {"vector-add", "vector-subtract"})
      result.push_back(Reporter(std::string("math/") + id, "math", id, T::Vector3,
          {Input("a", "", T::Vector3, "targets/direction"), Input("b", "", T::Vector3, "targets/direction")}));
    result.push_back(Reporter("math/vector-scale", "math", "scale vector", T::Vector3,
        {Input("value", "", T::Vector3, "targets/direction"), number("factor", "by", "1")}));
    for (const auto &id : {"number-range", "integer-range"})
      result.push_back(Reporter(std::string("math/") + id, "math", "range",
          std::string(id) == "number-range" ? T::NumberRange : T::IntegerRange,
          {number("minimum", "from"), number("maximum", "to", "1")}));

    result.push_back(Reporter("simulation/car-position", "simulation",
                              "car position", VisualValueType::Position3));
    result.push_back(Reporter("simulation/car-velocity", "simulation",
                              "car velocity", VisualValueType::Vector3));
    result.push_back(Reporter("simulation/car-local-velocity", "simulation",
                              "car local velocity", VisualValueType::Vector3));
    result.push_back(Reporter("simulation/car-speed", "simulation", "car speed",
                              VisualValueType::MetersPerSecond));
    result.push_back(Reporter("simulation/car-rotation", "simulation",
                              "car rotation", VisualValueType::Rotation3));
    result.push_back(Reporter("simulation/stunt-points", "simulation",
                              "stunt points", VisualValueType::Number));
    result.push_back(Reporter("simulation/finish-time", "simulation",
                              "finish time", VisualValueType::Milliseconds));
    result.push_back(Reporter("simulation/time", "simulation", "simulation time",
                              VisualValueType::Milliseconds));
    result.push_back(Reporter("simulation/checkpoint-count", "simulation",
                              "checkpoint count", VisualValueType::Integer));
    result.push_back(Predicate("simulation/race-completed", "simulation",
                               "race completed"));
    result.push_back(Predicate("simulation/sliding", "simulation", "sliding"));
    result.push_back(Predicate("simulation/freewheeling", "simulation",
                               "freewheeling"));

    VisualBlockDefinition point = Reporter(
        "targets/point", "targets", "point", VisualValueType::Position3,
        {Input("x", "x", VisualValueType::Meters, "values/meters", "0"),
         Input("y", "y", VisualValueType::Meters, "values/meters", "0"),
         Input("z", "z", VisualValueType::Meters, "values/meters", "0")});
    point.viewerPicker = "point";
    result.push_back(std::move(point));
    result.push_back(Reporter(
        "targets/direction", "targets", "direction",
        VisualValueType::Direction3,
        {Input("x", "x", VisualValueType::Number, "values/number", "1"),
         Input("y", "y", VisualValueType::Number, "values/number", "0"),
         Input("z", "z", VisualValueType::Number, "values/number", "0")}));
    VisualBlockDefinition rotation = Reporter(
        "targets/rotation", "targets", "rotation", VisualValueType::Rotation3,
        {Input("yaw", "yaw", VisualValueType::Degrees, "values/degrees", "0"),
         Input("pitch", "pitch", VisualValueType::Degrees, "values/degrees",
               "0"),
         Input("roll", "roll", VisualValueType::Degrees, "values/degrees",
               "0")});
    rotation.viewerPicker = "rotation";
    result.push_back(std::move(rotation));
    result.push_back(Reporter(
        "targets/size", "targets", "size", VisualValueType::Vector3,
        {Input("x", "x", VisualValueType::Meters, "values/meters", "10"),
         Input("y", "y", VisualValueType::Meters, "values/meters", "10"),
         Input("z", "z", VisualValueType::Meters, "values/meters", "10")}));
    VisualBlockDefinition box = Reporter(
        "targets/box", "targets", "box", VisualValueType::Volume,
        {Input("center", "center", VisualValueType::Position3, "targets/point"),
         Input("size", "size", VisualValueType::Vector3, "targets/size")});
    box.viewerPicker = "box";
    result.push_back(std::move(box));
    VisualBlockDefinition prism = Reporter(
        "targets/prism", "targets", "prism", VisualValueType::Volume,
        {Input("origin", "origin", VisualValueType::Position3, "targets/point"),
         Input("depth", "depth", VisualValueType::Meters, "values/meters", "5"),
         Input("polygon", "polygon", VisualValueType::Polygon2,
               "targets/polygon-from-points")});
    prism.fields.push_back({"plane",
                            "plane",
                            VisualFieldDefinition::Kind::Enum,
                            "xz",
                            {{"xy", "XY"}, {"xz", "XZ"}, {"yz", "YZ"}}});
    prism.viewerPicker = "prism";
    result.push_back(std::move(prism));

    result.push_back(Reporter("math/distance", "math", "distance",
                              VisualValueType::Meters,
                              {Input("a", "", VisualValueType::Position3),
                               Input("b", "to", VisualValueType::Position3)}));
    result.push_back(Reporter("math/magnitude", "math", "magnitude",
                              VisualValueType::Scalar,
                              {Input("value", "", VisualValueType::Vector3)}));
    result.push_back(Reporter("math/normalize", "math", "normalize",
                              VisualValueType::Direction3,
                              {Input("value", "", VisualValueType::Vector3)}));
    result.push_back(Reporter("math/dot", "math", "dot",
                              VisualValueType::Scalar,
                              {Input("a", "", VisualValueType::Vector3),
                               Input("b", "with", VisualValueType::Vector3)}));
    result.push_back(Reporter("math/rotation-distance", "math",
                              "rotation distance", VisualValueType::Number,
                              {Input("a", "", VisualValueType::Rotation3),
                               Input("b", "to", VisualValueType::Rotation3)}));
    result.push_back(
        Reporter("math/weighted-blend", "math", "weighted blend",
                 VisualValueType::Scalar,
                 {Input("a", "first", VisualValueType::Scalar),
                  Input("b", "second", VisualValueType::Scalar),
                  Input("weight", "weight", VisualValueType::Percent,
                        "values/percent", "50")}));
    result.push_back(Reporter("math/percent-ratio", "math", "as ratio",
                              VisualValueType::Number,
                              {Input("value", "", VisualValueType::Percent,
                                     "values/percent", "50")}));
    result.push_back(Reporter(
        "math/kmh", "math", "km/h", VisualValueType::Number,
        {Input("value", "", VisualValueType::MetersPerSecond)}));
    result.push_back(Reporter(
        "math/abs", "math", "abs", VisualValueType::Scalar,
        {Input("value", "", VisualValueType::Scalar, "values/number", "0")}));
    result.push_back(Reporter(
        "math/clamp", "math", "clamp", VisualValueType::Scalar,
        {Input("value", "", VisualValueType::Scalar, "values/number", "0"),
         Input("minimum", "min", VisualValueType::Scalar, "values/number", "0"),
         Input("maximum", "max", VisualValueType::Scalar, "values/number", "1")}));
    for (const auto &[id, label] :
         std::vector<std::pair<std::string, std::string>>{{"add", "+"},
                                                          {"subtract", "−"},
                                                          {"multiply", "×"},
                                                          {"divide", "÷"},
                                                          {"min", "min"},
                                                          {"max", "max"}}) {
      result.push_back(Reporter(
          "math/" + id, "math", label, VisualValueType::Scalar,
          {Input("a", "", VisualValueType::Scalar, "values/number", "0"),
           Input("b", "", VisualValueType::Scalar, "values/number",
                 id == "multiply" || id == "divide" ? "1" : "0")}));
    }

    for (const auto &[id, label] :
         std::vector<std::pair<std::string, std::string>>{
             {"less", "<"}, {"less-equal", "≤"}, {"equal", "="},
             {"greater-equal", "≥"}, {"greater", ">"}}) {
      result.push_back(Predicate(
          "conditions/" + id, "conditions", label,
          {Input("a", "", VisualValueType::Scalar),
           Input("b", "", VisualValueType::Scalar, "values/number", "0")}));
    }
    result.push_back(Predicate(
        "conditions/and", "conditions", "and",
        {Input("a", "", VisualValueType::Boolean, "values/boolean", "true"),
         Input("b", "", VisualValueType::Boolean, "values/boolean", "true")}));
    result.push_back(Predicate(
        "conditions/or", "conditions", "or",
        {Input("a", "", VisualValueType::Boolean, "values/boolean", "false"),
         Input("b", "", VisualValueType::Boolean, "values/boolean", "false")}));
    result.push_back(Predicate(
        "conditions/not", "conditions", "not",
        {Input("value", "", VisualValueType::Boolean, "values/boolean",
               "false")}));
    result.push_back(
        Predicate("conditions/inside", "conditions", "inside",
                  {Input("position", "", VisualValueType::Position3),
                   Input("volume", "", VisualValueType::Volume)}));

    result.push_back(Reporter(
        "time/range", "values", "", VisualValueType::TimeRange,
        {Input("from", "from", VisualValueType::Milliseconds,
               "values/milliseconds", "1000"),
         Input("to", "to", VisualValueType::Milliseconds, "values/milliseconds",
               "6000")}));
    result.push_back(Reporter("time/at", "values", "at",
                              VisualValueType::TimeRange,
                              {Input("time", "", VisualValueType::Milliseconds,
                                     "values/milliseconds", "6000")}));
    result.push_back(Reporter("time/all", "values", "whole simulation",
                              VisualValueType::TimeRange));
    result.push_back(Literal("values/number", "values", "number",
                             VisualValueType::Number,
                             VisualFieldDefinition::Kind::Number, "0"));
    result.push_back(Literal("values/boolean", "values", "boolean",
                             VisualValueType::Boolean,
                             VisualFieldDefinition::Kind::Boolean, "false"));
    result.push_back(Literal("values/integer", "values", "integer",
                             VisualValueType::Integer,
                             VisualFieldDefinition::Kind::Integer, "1"));
    VisualBlockDefinition numberRange =
        Reporter("values/number-range", "values", "range",
                 VisualValueType::NumberRange);
    numberRange.fields = {
        {"minimum", "", VisualFieldDefinition::Kind::Number, "0", {}},
        {"maximum", "", VisualFieldDefinition::Kind::Number, "1", {}}};
    result.push_back(std::move(numberRange));
    VisualBlockDefinition integerRange =
        Reporter("values/integer-range", "values", "integer range",
                 VisualValueType::IntegerRange);
    integerRange.fields = {
        {"minimum", "", VisualFieldDefinition::Kind::Integer, "0", {}},
        {"maximum", "", VisualFieldDefinition::Kind::Integer, "1", {}}};
    result.push_back(std::move(integerRange));
    result.push_back(Literal("values/meters", "values", "meters",
                             VisualValueType::Meters,
                             VisualFieldDefinition::Kind::Number, "0"));
    result.push_back(Literal("values/milliseconds", "values", "ms",
                             VisualValueType::Milliseconds,
                             VisualFieldDefinition::Kind::Integer, "1000"));
    result.push_back(Literal("values/degrees", "values", "degrees",
                             VisualValueType::Degrees,
                             VisualFieldDefinition::Kind::Number, "0"));
    result.push_back(Literal("values/percent", "values", "percent",
                             VisualValueType::Percent,
                             VisualFieldDefinition::Kind::Number, "50"));
    result.push_back(Reporter("targets/polygon-from-points", "targets", "polygon from points",
        VisualValueType::Polygon2, {Input("points", "", VisualValueType::List, "data/list")}));
    for (const auto &end : {std::string("minimum"), std::string("maximum")})
      result.push_back(Reporter("math/range-"+end, "math", "range "+end,
          VisualValueType::Scalar, {Input("range", "", VisualValueType::Any, "math/number-range")}));
    for (auto &block : result) {
      if (block.id == "conditions/equal")
        for (auto &input : block.inputs) input.type = VisualValueType::Any;
      if (block.shape == VisualBlockShape::Command || block.shape == VisualBlockShape::Control)
        block.statementFamily = "command";
      for (auto &statement : block.statements) statement.family = "command";
    }
    return result;
  }();
  return definitions;
}

const VisualBlockDefinition *FindVisualBlock(const std::string &id) {
  const auto &catalog = VisualBlockCatalog();
  const auto found =
      std::find_if(catalog.begin(), catalog.end(),
                   [&id](const VisualBlockDefinition &definition) {
                     return definition.id == id;
                   });
  return found == catalog.end() ? nullptr : &*found;
}

const VisualBlockDefinition *
FindVisualBlockByBlocklyType(const std::string &type) {
  const auto &catalog = VisualBlockCatalog();
  const auto found =
      std::find_if(catalog.begin(), catalog.end(),
                   [&type](const VisualBlockDefinition &definition) {
                     return definition.blocklyType == type;
                   });
  return found == catalog.end() ? nullptr : &*found;
}

std::vector<std::string> VisualProcedureParameters(const VisualNode &node) {
  const auto field = node.fields.find("parameters");
  if (field == node.fields.end() || field->second.empty()) return {};
  std::vector<std::string> result;
  std::size_t begin = 0;
  do {
    const auto end = field->second.find(',', begin);
    std::string name = field->second.substr(begin, end - begin);
    const auto first = name.find_first_not_of(" \t\r\n");
    const auto last = name.find_last_not_of(" \t\r\n");
    result.push_back(first == std::string::npos ? "" : name.substr(first, last - first + 1));
    if (end == std::string::npos) break;
    begin = end + 1;
  } while (begin <= field->second.size());
  return result;
}

std::vector<VisualInputDefinition> VisualInputsForNode(const VisualNode &node) {
  if (node.definitionId == "procedures/call" || node.definitionId == "procedures/value") {
    std::vector<VisualInputDefinition> inputs;
    for (const auto &name : VisualProcedureParameters(node))
      inputs.push_back(Input("arg" + std::to_string(inputs.size()), name,
                             VisualValueType::Any, "values/number", "0"));
    return inputs;
  }
  const auto *definition = FindVisualBlock(node.definitionId);
  return definition ? definition->inputs : std::vector<VisualInputDefinition>{};
}

} // namespace forevertas::blocks
