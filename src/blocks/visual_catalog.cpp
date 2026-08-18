#include "blocks/visual_catalog.h"

#include "blocks/block_catalog.h"
#include "searches/algorithm_registry.h"

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
          std::move(defaultValue), {}};
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

VisualFieldDefinition FromLegacyField(const OptionField &field) {
  VisualFieldDefinition result;
  result.key = field.key;
  result.label = field.label;
  result.defaultValue = field.defaultValue;
  switch (field.kind) {
  case OptionField::Kind::Number:
    result.kind = VisualFieldDefinition::Kind::Number;
    break;
  case OptionField::Kind::Boolean:
    result.kind = VisualFieldDefinition::Kind::Boolean;
    break;
  case OptionField::Kind::Enum:
    result.kind = VisualFieldDefinition::Kind::Enum;
    result.enumValues = field.enumValues;
    break;
  case OptionField::Kind::Line:
    result.kind = VisualFieldDefinition::Kind::Text;
    break;
  case OptionField::Kind::Mirrored:
    // Mirrored fields are implementation aliases in the old registry;
    // they should not become independent editor controls.
    result.key.clear();
    break;
  }
  return result;
}

bool EndsWith(const std::string &value, const std::string &suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::optional<std::string> MaximumPartnerKey(const std::string &minimumKey) {
  if (minimumKey.rfind("min", 0) == 0) {
    return "max" + minimumKey.substr(3);
  }
  const std::size_t marker = minimumKey.find("Min");
  if (marker == std::string::npos)
    return std::nullopt;
  std::string result = minimumKey;
  result.replace(marker, 3, "Max");
  return result;
}

std::string RangeInputKey(const std::string &minimumKey) {
  if (minimumKey == "minCount")
    return "countRange";
  if (minimumKey.rfind("min", 0) == 0)
    return "range" + minimumKey.substr(3);
  const std::size_t count = minimumKey.find("MinCount");
  if (count != std::string::npos) {
    std::string result = minimumKey;
    result.replace(count, 8, "CountRange");
    return result;
  }
  const std::size_t marker = minimumKey.find("Min");
  if (marker == std::string::npos)
    return minimumKey + "Range";
  std::string result = minimumKey;
  result.replace(marker, 3, "Range");
  return result;
}

std::string RangeLabel(std::string label) {
  if (label.rfind("Min ", 0) == 0)
    return label.substr(4);
  if (EndsWith(label, " min")) {
    label.resize(label.size() - 4);
    return label;
  }
  return label;
}

std::optional<VisualInputDefinition>
CompactRangeInput(const OptionField &minimum,
                  const OptionFieldList &fields) {
  if (minimum.kind != OptionField::Kind::Number)
    return std::nullopt;
  const auto maximumKey = MaximumPartnerKey(minimum.key);
  if (!maximumKey)
    return std::nullopt;
  const auto maximum =
      std::find_if(fields.begin(), fields.end(), [&maximumKey](const OptionField &field) {
        return field.key == *maximumKey && field.kind == OptionField::Kind::Number;
      });
  if (maximum == fields.end())
    return std::nullopt;
  const bool integer = EndsWith(minimum.key, "Count") &&
                       EndsWith(maximum->key, "Count");
  VisualInputDefinition result{
      RangeInputKey(minimum.key),
      RangeLabel(minimum.label),
      integer ? VisualValueType::IntegerRange : VisualValueType::NumberRange,
      integer ? "values/integer-range" : "values/number-range",
      minimum.defaultValue + "," + maximum->defaultValue,
      {}};
  result.nativeSettingKeys = {minimum.key, maximum->key};
  return result;
}

std::string CompactMutationLabel(const std::string &id,
                                 const std::string &fallback) {
  static const std::vector<std::pair<std::string, std::string>> labels{
      {"mutate/shift-events", "shift events"},
      {"mutate/nudge-steering", "nudge steering"},
      {"mutate/set-steering", "set steering"},
      {"mutate/flip-accelerate", "flip accelerate"},
      {"mutate/flip-brake", "flip brake"},
      {"mutate/insert-steering-at", "insert steering"},
      {"mutate/adjust-steering-by", "insert steering offset"},
      {"mutate/press-accelerate", "insert accelerate"},
      {"mutate/press-brake", "insert brake"},
      {"mutate/delete-steering", "delete steering"},
      {"mutate/delete-accelerate", "delete accelerate"},
      {"mutate/delete-brake", "delete brake"},
      {"mutate/reroll-steering", "reroll steering"},
      {"mutate/smooth-steering", "smooth steering"}};
  const auto found =
      std::find_if(labels.begin(), labels.end(), [&id](const auto &entry) {
        return entry.first == id;
      });
  return found == labels.end() ? fallback : found->second;
}

VisualInputDefinition FromLegacyScalarField(const OptionField &field) {
  if (field.kind == OptionField::Kind::Boolean) {
    return Input(field.key, field.label, VisualValueType::Boolean,
                 "values/boolean", field.defaultValue);
  }
  VisualValueType type = VisualValueType::Number;
  std::string literal = "values/number";
  std::string label = field.label;
  if (EndsWith(field.key, "Count")) {
    type = VisualValueType::Integer;
    literal = "values/integer";
  } else if (EndsWith(field.key, "Ms")) {
    type = VisualValueType::Milliseconds;
    literal = "values/milliseconds";
    const std::string unit = " (ms)";
    if (EndsWith(label, unit))
      label.resize(label.size() - unit.size());
  }
  return Input(field.key, std::move(label), type, std::move(literal),
               field.defaultValue);
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
      {"search", "Search", "#d97706"},
      {"flow", "Flow", "#e69024"},
      {"simulation", "Simulation", "#3278c8"},
      {"mutation", "Mutation", "#7a4bd1"},
      {"conditions", "Conditions", "#43a85b"},
      {"targets", "Targets", "#159e8c"},
      {"math", "Math", "#59a64a"},
      {"values", "Values", "#6c78c7"}};
  return categories;
}

const std::vector<VisualBlockDefinition> &VisualBlockCatalog() {
  static const std::vector<VisualBlockDefinition> definitions = [] {
    std::vector<VisualBlockDefinition> result;

    for (const SearchAlgorithmRegistration &registration :
         SearchAlgorithmRegistry()) {
      VisualBlockDefinition definition =
          Command("search/" + registration.id, "search",
                  registration.id == kBasicBruteForceSearchId
                      ? "bruteforce"
                      : registration.displayName,
                  {}, "root-command");
      definition.shape = VisualBlockShape::Control;
      definition.inputsInline = false;
      definition.statements.push_back(
          {"body", "each iteration", "iteration-command"});
      for (const OptionField &field : registration.fields) {
        if (field.kind == OptionField::Kind::Number ||
            field.kind == OptionField::Kind::Boolean) {
          VisualInputDefinition input = FromLegacyScalarField(field);
          if (registration.id == kBasicBruteForceSearchId &&
              field.key == "autoPromoteBest") {
            input.label = "promote best";
          }
          definition.inputs.push_back(std::move(input));
        } else {
          VisualFieldDefinition converted = FromLegacyField(field);
          if (!converted.key.empty())
            definition.fields.push_back(std::move(converted));
        }
      }
      result.push_back(std::move(definition));
    }

    VisualBlockDefinition start;
    start.id = "flow/when-start";
    start.blocklyType = VisualBlocklyType(start.id);
    start.categoryId = "flow";
    start.label = "when search starts";
    start.shape = VisualBlockShape::Hat;
    start.statements.push_back({"body", "", "root-command"});
    result.push_back(std::move(start));

    result.push_back(Command("flow/set-objective", "flow",
                             "keep best by",
                             {Input("score", "", VisualValueType::Score)},
                             "iteration-command"));

    VisualBlockDefinition simulate = Command(
        "flow/simulate", "simulation", "simulate",
        {Input("until", "until", VisualValueType::Milliseconds,
               "values/milliseconds", "6000"),
         Input("where", "where", VisualValueType::Boolean,
               "values/boolean", "true")},
        "iteration-command");
    simulate.inputsInline = false;
    result.push_back(std::move(simulate));

    VisualBlockDefinition window =
        Command("flow/mutation-window", "mutation", "mutate",
                {Input("range", "during", VisualValueType::TimeRange, "time/range",
                       "1000,5990"),
                 Input("seed", "seed", VisualValueType::Integer,
                       "values/integer", "100000000")},
                "iteration-command");
    window.shape = VisualBlockShape::Control;
    window.inputsInline = false;
    window.statements.push_back({"body", "", "mutation-command"});
    result.push_back(std::move(window));

    VisualBlockDefinition minimize = Reporter(
        "objective/minimize", "flow", "minimize", VisualValueType::Score,
        {Input("value", "", VisualValueType::Scalar),
         Input("range", "during", VisualValueType::TimeRange, "time/range",
               "1000,6000")});
    minimize.inputsInline = false;
    result.push_back(std::move(minimize));
    VisualBlockDefinition maximize = Reporter(
        "objective/maximize", "flow", "maximize", VisualValueType::Score,
        {Input("value", "", VisualValueType::Scalar),
         Input("range", "during", VisualValueType::TimeRange, "time/range",
               "1000,6000")});
    maximize.inputsInline = false;
    result.push_back(std::move(maximize));
    VisualBlockDefinition onlyWhen = Reporter(
        "objective/only-when", "flow", "only when", VisualValueType::Score,
        {Input("score", "", VisualValueType::Score),
         Input("condition", "condition", VisualValueType::Boolean)});
    onlyWhen.inputsInline = false;
    result.push_back(std::move(onlyWhen));
    VisualBlockDefinition firstTime = Reporter(
        "objective/first-time", "flow", "first time", VisualValueType::Score,
        {Input("condition", "", VisualValueType::Boolean),
         Input("range", "during", VisualValueType::TimeRange, "time/all")});
    firstTime.inputsInline = false;
    result.push_back(std::move(firstTime));

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
               "values/polygon", "-5,-5;5,-5;0,5")});
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

    // Transitional representation for old condition-script settings. It is
    // deliberately absent from the toolbox; users should compose predicates
    // from normal blocks, while existing saved scripts remain lossless until
    // replaced.
    VisualBlockDefinition legacyCondition =
        Predicate("conditions/legacy-script", "conditions", "legacy condition");
    legacyCondition.toolboxVisible = false;
    legacyCondition.fields.push_back({"source",
                                      "",
                                      VisualFieldDefinition::Kind::Text,
                                      "",
                                      {}});
    result.push_back(std::move(legacyCondition));

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
    result.push_back(Literal(
        "values/polygon", "values", "polygon", VisualValueType::Polygon2,
        VisualFieldDefinition::Kind::Text, "-5,-5;5,-5;0,5"));

    // Mutation atoms remain physical edit primitives, but their numeric
    // parameters are reporter sockets rather than settings-card fields. The
    // legacy registry is still the source of labels/defaults and the native
    // lowering contract; the visual editor only changes how values compose.
    for (const BlockDefinition &legacy : BlockCatalog()) {
      if (legacy.optionKind != "mutation" || legacy.id == "mutate/window") {
        continue;
      }
      VisualBlockDefinition definition =
          Command(legacy.id, "mutation",
                  CompactMutationLabel(legacy.id, legacy.label), {},
                  "mutation-command");
      std::set<std::string> consumedFields;
      for (const OptionField &field : legacy.fields) {
        if (consumedFields.count(field.key) != 0u)
          continue;
        if (const auto range = CompactRangeInput(field, legacy.fields)) {
          definition.inputs.push_back(*range);
          consumedFields.insert(range->nativeSettingKeys.begin(),
                                range->nativeSettingKeys.end());
          continue;
        }
        if (field.kind == OptionField::Kind::Number ||
            field.kind == OptionField::Kind::Boolean) {
          definition.inputs.push_back(FromLegacyScalarField(field));
        } else {
          VisualFieldDefinition converted = FromLegacyField(field);
          if (!converted.key.empty()) {
            definition.fields.push_back(std::move(converted));
          }
        }
      }
      result.push_back(std::move(definition));
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

} // namespace forevertas::blocks
