#include "blocks/visual_catalog.h"
#include "blocks/visual_compiler.h"
#include "blocks/visual_program.h"
#include "evaluators/point_target_evaluator.h"
#include "evaluators/visual_expression_evaluator.h"
#include "searches/algorithm_registry.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>

namespace {

using namespace forevertas;
using namespace forevertas::blocks;

bool Check(bool condition, const char *message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

class Builder final {
public:
  VisualNodeId add(const std::string &definitionId, double x = 0.0,
                   double y = 0.0) {
    const VisualNodeId id = next_++;
    VisualNode node;
    node.id = id;
    node.definitionId = definitionId;
    node.x = x;
    node.y = y;
    program.nodes.emplace(id, std::move(node));
    return id;
  }

  VisualNodeId literal(const std::string &definitionId,
                       const std::string &value) {
    const VisualNodeId id = add(definitionId);
    program.find(id)->fields["value"] = value;
    return id;
  }

  VisualNodeId rangeLiteral(const std::string &definitionId,
                            const std::string &minimum,
                            const std::string &maximum) {
    const VisualNodeId id = add(definitionId);
    program.find(id)->fields["minimum"] = minimum;
    program.find(id)->fields["maximum"] = maximum;
    return id;
  }

  void input(VisualNodeId owner, const std::string &key, VisualNodeId child) {
    program.find(owner)->inputs[key] = child;
  }

  void statement(VisualNodeId owner, const std::string &key,
                 VisualNodeId child) {
    program.find(owner)->statements[key].push_back(child);
  }

  void top(VisualNodeId id) { program.topLevel.push_back(id); }

  VisualProgram program;

private:
  VisualNodeId next_ = 1;
};

std::string DefaultBlockForType(VisualValueType type) {
  switch (type) {
  case VisualValueType::Scalar:
  case VisualValueType::Number:
    return "values/number";
  case VisualValueType::Integer:
    return "values/integer";
  case VisualValueType::NumberRange:
    return "values/number-range";
  case VisualValueType::IntegerRange:
    return "values/integer-range";
  case VisualValueType::Milliseconds:
    return "values/milliseconds";
  case VisualValueType::Meters:
    return "values/meters";
  case VisualValueType::MetersPerSecond:
    return "simulation/car-speed";
  case VisualValueType::Degrees:
    return "values/degrees";
  case VisualValueType::Percent:
    return "values/percent";
  case VisualValueType::Boolean:
    return "values/boolean";
  case VisualValueType::Vector3:
    return "targets/size";
  case VisualValueType::Position3:
    return "targets/point";
  case VisualValueType::Direction3:
    return "targets/direction";
  case VisualValueType::Rotation3:
    return "targets/rotation";
  case VisualValueType::Volume:
    return "targets/box";
  case VisualValueType::Polygon2:
    return "values/polygon";
  case VisualValueType::TimeRange:
    return "time/all";
  case VisualValueType::Score:
    return "objective/maximize";
  case VisualValueType::None:
    break;
  }
  return {};
}

VisualNodeId AddCatalogDefaultBlock(Builder &b, const std::string &definitionId,
                                    const std::string &overrideKey = {},
                                    VisualNodeId overrideNode = 0,
                                    std::size_t depth = 0u) {
  if (depth > 32u)
    return 0;
  const VisualBlockDefinition *const definition =
      FindVisualBlock(definitionId);
  if (definition == nullptr)
    return 0;
  const VisualNodeId id = b.add(definitionId);
  VisualNode *const node = b.program.find(id);
  for (const VisualFieldDefinition &field : definition->fields)
    node->fields[field.key] = field.defaultValue;
  for (const VisualInputDefinition &input : definition->inputs) {
    if (input.key == overrideKey && overrideNode != 0) {
      b.input(id, input.key, overrideNode);
      continue;
    }
    const std::string childDefinition = input.defaultBlockId.empty()
                                            ? DefaultBlockForType(input.type)
                                            : input.defaultBlockId;
    if (childDefinition.empty())
      continue;
    const VisualNodeId child =
        AddCatalogDefaultBlock(b, childDefinition, {}, 0, depth + 1u);
    if (child == 0)
      continue;
    if (!input.defaultValue.empty()) {
      VisualNode *const childNode = b.program.find(child);
      const VisualBlockDefinition *const childDefinitionInfo =
          FindVisualBlock(childDefinition);
      if (childDefinitionInfo != nullptr) {
        const auto valueField = std::find_if(
            childDefinitionInfo->fields.begin(), childDefinitionInfo->fields.end(),
            [](const VisualFieldDefinition &field) {
              return field.key == "value";
            });
        if (valueField != childDefinitionInfo->fields.end()) {
          childNode->fields["value"] = input.defaultValue;
        } else {
          const std::size_t comma = input.defaultValue.find(',');
          if (comma != std::string::npos &&
              childNode->fields.count("minimum") != 0u &&
              childNode->fields.count("maximum") != 0u) {
            childNode->fields["minimum"] = input.defaultValue.substr(0, comma);
            childNode->fields["maximum"] = input.defaultValue.substr(comma + 1u);
          }
        }
      }
    }
    b.input(id, input.key, child);
  }
  return id;
}

VisualNodeId AddSearchPolicy(Builder &b, VisualNodeId start,
                             bool autoPromote = false) {
  const VisualNodeId search = b.add("search/basic-brute-force");
  b.statement(start, "body", search);
  const VisualNodeId promote = b.add("values/boolean");
  b.program.find(promote)->fields["value"] = autoPromote ? "true" : "false";
  b.input(search, "autoPromoteBest", promote);
  return search;
}

VisualNodeId AddSimulationStep(Builder &b, VisualNodeId search,
                               const std::string &horizonMs = "6000",
                               VisualNodeId where = 0) {
  const VisualNodeId simulate = b.add("flow/simulate");
  b.input(simulate, "until",
          b.literal("values/milliseconds", horizonMs));
  b.input(simulate, "where",
          where != 0 ? where : b.literal("values/boolean", "true"));
  b.statement(search, "body", simulate);
  return simulate;
}

void SetIterationPipeline(Builder &b, VisualNodeId search, VisualNodeId mutation,
                          VisualNodeId simulate, VisualNodeId choose) {
  b.program.find(search)->statements["body"] = {mutation, simulate, choose};
}

VisualProgram SimulationPredicateProgram(
    const std::function<VisualNodeId(Builder &)> &buildPredicate) {
  Builder b;
  const VisualNodeId where = buildPredicate(b);
  const VisualNodeId start = b.add("flow/when-start");
  b.top(start);
  const VisualNodeId search = AddSearchPolicy(b, start);

  const VisualNodeId choose = b.add("flow/set-objective");
  const VisualNodeId maximize = b.add("objective/maximize");
  b.statement(search, "body", choose);
  b.input(choose, "score", maximize);
  b.input(maximize, "value", b.add("simulation/car-speed"));
  b.input(maximize, "range", b.add("time/all"));

  const VisualNodeId window = b.add("flow/mutation-window");
  const VisualNodeId range = b.add("time/range");
  b.statement(search, "body", window);
  b.input(range, "from", b.literal("values/milliseconds", "100"));
  b.input(range, "to", b.literal("values/milliseconds", "990"));
  b.input(window, "range", range);
  b.input(window, "seed", b.literal("values/integer", "42"));
  b.statement(window, "body", b.add("mutate/reroll-steering"));

  const VisualNodeId simulate = AddSimulationStep(b, search, "1000", where);
  SetIterationPipeline(b, search, window, simulate, choose);
  return std::move(b.program);
}

CompileResult CompileCatalogProgram(Builder &b, VisualNodeId score,
                                    VisualNodeId mutationCommand = 0,
                                    VisualNodeId simulationWhere = 0) {
  const VisualNodeId start = b.add("flow/when-start");
  b.top(start);
  const VisualNodeId search = AddSearchPolicy(b, start);

  const VisualNodeId mutation = b.add("flow/mutation-window");
  const VisualNodeId range = b.add("time/range");
  b.input(range, "from", b.literal("values/milliseconds", "100"));
  b.input(range, "to", b.literal("values/milliseconds", "990"));
  b.input(mutation, "range", range);
  b.input(mutation, "seed", b.literal("values/integer", "42"));
  if (mutationCommand == 0)
    mutationCommand = AddCatalogDefaultBlock(b, "mutate/reroll-steering");
  b.statement(mutation, "body", mutationCommand);

  const VisualNodeId simulate =
      AddSimulationStep(b, search, "1000", simulationWhere);
  const VisualNodeId choose = b.add("flow/set-objective");
  b.input(choose, "score", score);
  SetIterationPipeline(b, search, mutation, simulate, choose);
  return CompileVisualProgram(
      b.program,
      {DefaultSearchAlgorithmConfiguration(), DefaultModifierConfigurations(),
       DefaultEvaluationTargetConfiguration()});
}

VisualNodeId PredicateForCatalogValue(Builder &b,
                                      const VisualBlockDefinition &definition,
                                      VisualNodeId value) {
  const auto compareNonNegative = [&](VisualNodeId scalar) {
    const VisualNodeId predicate = b.add("conditions/greater-equal");
    b.input(predicate, "a", scalar);
    b.input(predicate, "b", b.literal("values/number", "0"));
    return predicate;
  };

  if (definition.outputType == VisualValueType::Boolean)
    return value;
  if (VisualTypeCompatible(definition.outputType, VisualValueType::Scalar))
    return compareNonNegative(value);
  if (definition.outputType == VisualValueType::Position3) {
    const VisualNodeId distance = b.add("math/distance");
    b.input(distance, "a", value);
    b.input(distance, "b", AddCatalogDefaultBlock(b, "targets/point"));
    return compareNonNegative(distance);
  }
  if (definition.outputType == VisualValueType::Vector3 ||
      definition.outputType == VisualValueType::Direction3) {
    const VisualNodeId magnitude = b.add("math/magnitude");
    b.input(magnitude, "value", value);
    return compareNonNegative(magnitude);
  }
  if (definition.outputType == VisualValueType::Rotation3) {
    const VisualNodeId distance = b.add("math/rotation-distance");
    b.input(distance, "a", value);
    b.input(distance, "b", AddCatalogDefaultBlock(b, "targets/rotation"));
    return compareNonNegative(distance);
  }
  if (definition.outputType == VisualValueType::Volume) {
    const VisualNodeId inside = b.add("conditions/inside");
    b.input(inside, "position", b.add("simulation/car-position"));
    b.input(inside, "volume", value);
    return inside;
  }
  if (definition.outputType == VisualValueType::Polygon2) {
    const VisualNodeId prism =
        AddCatalogDefaultBlock(b, "targets/prism", "polygon", value);
    const VisualNodeId inside = b.add("conditions/inside");
    b.input(inside, "position", b.add("simulation/car-position"));
    b.input(inside, "volume", prism);
    return inside;
  }
  if (definition.outputType == VisualValueType::Percent) {
    const VisualNodeId ratio = b.add("math/percent-ratio");
    b.input(ratio, "value", value);
    return compareNonNegative(ratio);
  }
  return 0;
}

VisualNodeId ScoreForCatalogValue(Builder &b,
                                  const VisualBlockDefinition &definition,
                                  VisualNodeId value) {
  if (definition.outputType == VisualValueType::Score)
    return value;
  if (VisualTypeCompatible(definition.outputType, VisualValueType::Scalar)) {
    const VisualNodeId score = b.add("objective/maximize");
    b.input(score, "value", value);
    b.input(score, "range", b.add("time/all"));
    return score;
  }
  if (definition.outputType == VisualValueType::Boolean) {
    const VisualNodeId score = b.add("objective/first-time");
    b.input(score, "condition", value);
    b.input(score, "range", b.add("time/all"));
    return score;
  }
  if (definition.outputType == VisualValueType::Position3) {
    const VisualNodeId distance = b.add("math/distance");
    b.input(distance, "a", value);
    b.input(distance, "b", AddCatalogDefaultBlock(b, "targets/point"));
    const VisualNodeId score = b.add("objective/minimize");
    b.input(score, "value", distance);
    b.input(score, "range", b.add("time/all"));
    return score;
  }
  if (definition.outputType == VisualValueType::Vector3 ||
      definition.outputType == VisualValueType::Direction3) {
    const VisualNodeId magnitude = b.add("math/magnitude");
    b.input(magnitude, "value", value);
    const VisualNodeId score = b.add("objective/maximize");
    b.input(score, "value", magnitude);
    b.input(score, "range", b.add("time/all"));
    return score;
  }
  if (definition.outputType == VisualValueType::Rotation3) {
    const VisualNodeId distance = b.add("math/rotation-distance");
    b.input(distance, "a", value);
    b.input(distance, "b", AddCatalogDefaultBlock(b, "targets/rotation"));
    const VisualNodeId score = b.add("objective/minimize");
    b.input(score, "value", distance);
    b.input(score, "range", b.add("time/all"));
    return score;
  }
  if (definition.outputType == VisualValueType::Volume) {
    const VisualNodeId inside = b.add("conditions/inside");
    b.input(inside, "position", b.add("simulation/car-position"));
    b.input(inside, "volume", value);
    const VisualNodeId score = b.add("objective/first-time");
    b.input(score, "condition", inside);
    b.input(score, "range", b.add("time/all"));
    return score;
  }
  if (definition.outputType == VisualValueType::Polygon2) {
    const VisualNodeId prism =
        AddCatalogDefaultBlock(b, "targets/prism", "polygon", value);
    const VisualNodeId inside = b.add("conditions/inside");
    b.input(inside, "position", b.add("simulation/car-position"));
    b.input(inside, "volume", prism);
    const VisualNodeId score = b.add("objective/first-time");
    b.input(score, "condition", inside);
    b.input(score, "range", b.add("time/all"));
    return score;
  }
  if (definition.outputType == VisualValueType::TimeRange) {
    const VisualNodeId score = b.add("objective/maximize");
    b.input(score, "value", b.add("simulation/car-speed"));
    b.input(score, "range", value);
    return score;
  }
  if (definition.outputType == VisualValueType::Percent) {
    const VisualNodeId ratio = b.add("math/percent-ratio");
    b.input(ratio, "value", value);
    const VisualNodeId score = b.add("objective/maximize");
    b.input(score, "value", ratio);
    b.input(score, "range", b.add("time/all"));
    return score;
  }
  return 0;
}

bool TestCatalogIsPrimitive() {
  bool okay = true;
  okay &= Check(FindVisualBlock("math/distance") != nullptr,
                "distance primitive missing");
  okay &= Check(FindVisualBlock("simulation/car-position") != nullptr,
                "car-position primitive missing");
  okay &= Check(FindVisualBlock("targets/point") != nullptr,
                "point primitive missing");
  okay &= Check(FindVisualBlock("objective/minimize") != nullptr,
                "minimize primitive missing");
  okay &= Check(FindVisualBlock("search/basic-brute-force") != nullptr,
                "basic bruteforce search policy missing");
  okay &= Check(FindVisualBlock("flow/simulate") != nullptr,
                "explicit simulation step missing");
  okay &= Check(FindVisualBlock("values/boolean") != nullptr,
                "boolean literal primitive missing");
  okay &= Check(FindVisualBlock("simulation/checkpoint-count") != nullptr,
                "checkpoint reporter primitive missing");
  okay &= Check(FindVisualBlock("conditions/and") != nullptr &&
                    FindVisualBlock("conditions/not") != nullptr &&
                    FindVisualBlock("conditions/less") != nullptr,
                "composable predicate primitives missing");
  okay &= Check(FindVisualBlock("math/kmh") != nullptr &&
                    FindVisualBlock("math/abs") != nullptr &&
                    FindVisualBlock("math/clamp") != nullptr,
                "basic math/conversion primitives missing");
  okay &= Check(FindVisualBlock("evaluate/distance-to-point") == nullptr,
                "monolithic point evaluator leaked into v3 catalog");
  const VisualBlockDefinition *const smooth =
      FindVisualBlock("mutate/smooth-steering");
  okay &= Check(smooth != nullptr, "smooth mutation primitive missing");
  if (smooth != nullptr) {
    okay &= Check(smooth->fields.empty(),
                  "numeric mutation parameters leaked into inline fields");
    const auto count = std::find_if(
        smooth->inputs.begin(), smooth->inputs.end(),
        [](const VisualInputDefinition &input) {
          return input.key == "deformationCount";
        });
    const auto radius = std::find_if(
        smooth->inputs.begin(), smooth->inputs.end(),
        [](const VisualInputDefinition &input) { return input.key == "radiusMs"; });
    okay &= Check(count != smooth->inputs.end() &&
                      count->type == VisualValueType::Integer &&
                      count->defaultBlockId == "values/integer",
                  "mutation count is not an integer reporter socket");
    okay &= Check(radius != smooth->inputs.end() &&
                      radius->type == VisualValueType::Milliseconds &&
                      radius->defaultBlockId == "values/milliseconds",
                  "mutation duration is not a millisecond reporter socket");
  }
  return okay;
}

bool TestEveryToolboxBlockHasExecutableDefaults() {
  bool okay = true;
  for (const VisualBlockDefinition &definition : VisualBlockCatalog()) {
    if (!definition.toolboxVisible)
      continue;

    if (definition.statementFamily == "mutation-command") {
      Builder b;
      const VisualNodeId mutation = AddCatalogDefaultBlock(b, definition.id);
      const VisualNodeId score = AddCatalogDefaultBlock(b, "objective/maximize");
      const CompileResult compiled = CompileCatalogProgram(b, score, mutation);
      if (!compiled.ok) {
        std::cerr << "toolbox mutation '" << definition.id
                  << "' failed with its catalog defaults:\n";
        for (const auto &error : compiled.errors)
          std::cerr << "  " << error << '\n';
        okay = false;
      }
      continue;
    }

    if (definition.shape != VisualBlockShape::Reporter &&
        definition.shape != VisualBlockShape::Predicate)
      continue;

    Builder b;
    const VisualNodeId value = AddCatalogDefaultBlock(b, definition.id);
    if (value == 0) {
      std::cerr << "could not instantiate toolbox block '" << definition.id
                << "' from catalog defaults\n";
      okay = false;
      continue;
    }

    if (definition.outputType == VisualValueType::NumberRange ||
        definition.outputType == VisualValueType::IntegerRange) {
      const VisualBlockDefinition *consumer = nullptr;
      const VisualInputDefinition *consumerInput = nullptr;
      for (const VisualBlockDefinition &candidate : VisualBlockCatalog()) {
        if (!candidate.toolboxVisible ||
            candidate.statementFamily != "mutation-command")
          continue;
        const auto input = std::find_if(
            candidate.inputs.begin(), candidate.inputs.end(),
            [&definition](const VisualInputDefinition &candidateInput) {
              return VisualTypeCompatible(definition.outputType,
                                          candidateInput.type);
            });
        if (input != candidate.inputs.end()) {
          consumer = &candidate;
          consumerInput = &*input;
          break;
        }
      }
      if (consumer == nullptr || consumerInput == nullptr) {
        std::cerr << "toolbox range block '" << definition.id
                  << "' has no executable mutation consumer\n";
        okay = false;
        continue;
      }
      const VisualNodeId mutation = AddCatalogDefaultBlock(
          b, consumer->id, consumerInput->key, value);
      const VisualNodeId score = AddCatalogDefaultBlock(b, "objective/maximize");
      const CompileResult compiled = CompileCatalogProgram(b, score, mutation);
      if (!compiled.ok) {
        std::cerr << "toolbox range block '" << definition.id
                  << "' failed through mutation consumer '" << consumer->id
                  << "':\n";
        for (const auto &error : compiled.errors)
          std::cerr << "  " << error << '\n';
        okay = false;
      }
      continue;
    }

    const VisualNodeId score = ScoreForCatalogValue(b, definition, value);
    if (score == 0) {
      std::cerr << "toolbox value block '" << definition.id << "' of type '"
                << VisualValueTypeName(definition.outputType)
                << "' has no executable catalog test context\n";
      okay = false;
      continue;
    }
    const CompileResult compiled = CompileCatalogProgram(b, score);
    if (!compiled.ok) {
      std::cerr << "toolbox value block '" << definition.id
                << "' failed with its catalog defaults:\n";
      for (const auto &error : compiled.errors)
        std::cerr << "  " << error << '\n';
      okay = false;
    }
  }
  return okay;
}

bool TestEveryPredicateComposableToolboxBlockCompilesInSimulationWhere() {
  bool okay = true;
  for (const VisualBlockDefinition &definition : VisualBlockCatalog()) {
    if (!definition.toolboxVisible ||
        (definition.shape != VisualBlockShape::Reporter &&
         definition.shape != VisualBlockShape::Predicate))
      continue;

    Builder b;
    const VisualNodeId value = AddCatalogDefaultBlock(b, definition.id);
    if (value == 0) {
      std::cerr << "could not instantiate simulation-predicate block '"
                << definition.id << "' from catalog defaults\n";
      okay = false;
      continue;
    }
    const VisualNodeId predicate = PredicateForCatalogValue(b, definition, value);
    if (predicate == 0)
      continue;

    const VisualNodeId score = AddCatalogDefaultBlock(b, "objective/maximize");
    const CompileResult compiled = CompileCatalogProgram(b, score, 0, predicate);
    if (!compiled.ok) {
      std::cerr << "toolbox block '" << definition.id
                << "' failed inside simulate where:\n";
      for (const auto &error : compiled.errors)
        std::cerr << "  " << error << '\n';
      okay = false;
    }
  }
  return okay;
}

VisualProgram SmoothMutationProgram() {
  Builder b;
  const VisualNodeId start = b.add("flow/when-start");
  b.top(start);
  const VisualNodeId search = AddSearchPolicy(b, start, true);

  const VisualNodeId choose = b.add("flow/set-objective");
  b.statement(search, "body", choose);
  const VisualNodeId maximize = b.add("objective/maximize");
  b.input(choose, "score", maximize);
  b.input(maximize, "value", b.add("simulation/car-speed"));
  const VisualNodeId objectiveRange = b.add("time/range");
  b.input(objectiveRange, "from", b.literal("values/milliseconds", "1000"));
  b.input(objectiveRange, "to", b.literal("values/milliseconds", "6000"));
  b.input(maximize, "range", objectiveRange);

  const VisualNodeId window = b.add("flow/mutation-window");
  b.statement(search, "body", window);
  const VisualNodeId range = b.add("time/range");
  b.input(range, "from", b.literal("values/milliseconds", "1200"));
  b.input(range, "to", b.literal("values/milliseconds", "5200"));
  b.input(window, "range", range);
  b.input(window, "seed", b.literal("values/integer", "123456"));

  const VisualNodeId smooth = b.add("mutate/smooth-steering");
  b.statement(window, "body", smooth);
  b.input(smooth, "deformationCount", b.literal("values/integer", "3"));
  b.input(smooth, "radiusMs", b.literal("values/milliseconds", "300"));
  b.input(smooth, "amplitudeRange",
          b.rangeLiteral("values/number-range", "-0.35", "0.4"));
  const VisualNodeId simulate = AddSimulationStep(b, search);
  SetIterationPipeline(b, search, window, simulate, choose);
  return std::move(b.program);
}

bool TestMutationReporterSocketsLowerToNativeSettings() {
  const VisualProgram program = SmoothMutationProgram();
  const VisualProgramValidation validation = ValidateVisualProgram(program);
  bool okay = Check(validation.ok, "compositional mutation did not type-check");
  if (!validation.ok)
    return false;
  const CompileResult compiled = CompileVisualProgram(
      program,
      {DefaultSearchAlgorithmConfiguration(), DefaultModifierConfigurations(),
       DefaultEvaluationTargetConfiguration()});
  okay &= Check(compiled.ok, "compositional mutation did not compile");
  if (!compiled.ok)
    return false;
  okay &= Check(compiled.configuration.modifiers.size() == 1,
                "compositional mutation changed modifier count");
  okay &= Check(compiled.configuration.searchAlgorithm.id ==
                        kBasicBruteForceSearchId &&
                    compiled.configuration.searchAlgorithm.settings.at(
                        "autoPromoteBest") == "true",
                "search policy did not lower to native settings");
  okay &= Check(compiled.simulationHorizonMs &&
                    *compiled.simulationHorizonMs == "6000",
                "simulate step did not lower its horizon");
  if (compiled.configuration.modifiers.size() != 1)
    return false;
  const OptionConfiguration &modifier = compiled.configuration.modifiers.front();
  okay &= Check(modifier.id == kSmoothSteeringModifierId,
                "smooth mutation lowered to the wrong native modifier");
  const OptionSettings &settings = modifier.settings;
  okay &= Check(settings.at("minTimeMs") == "1200" &&
                      settings.at("maxTimeMs") == "5200" &&
                      settings.at("seed") == "123456",
                  "mutation window reporter values changed during lowering");
  okay &= Check(settings.at("deformationCount") == "3" &&
                      settings.at("radiusMs") == "300" &&
                      settings.at("amplitudeMin") == "-0.35" &&
                      settings.at("amplitudeMax") == "0.4",
                  "mutation parameter reporter values changed during lowering");
  return okay;
}

bool TestMutationWindowGroupingSurvivesNativeLowering() {
  VisualProgram program = SmoothMutationProgram();
  VisualNode *window = nullptr;
  for (auto &[id, node] : program.nodes) {
    static_cast<void>(id);
    if (node.definitionId == "flow/mutation-window") window = &node;
  }
  if (!Check(window != nullptr, "mutation window missing in grouping test"))
    return false;
  VisualNode reroll;
  reroll.id = 1000;
  reroll.definitionId = "mutate/reroll-steering";
  program.nodes.emplace(reroll.id, std::move(reroll));
  window->statements["body"].push_back(1000);

  const CompileResult compiled = CompileVisualProgram(
      program,
      {DefaultSearchAlgorithmConfiguration(), DefaultModifierConfigurations(),
       DefaultEvaluationTargetConfiguration()});
  bool okay = Check(compiled.ok,
                    "multi-family mutation window did not compile");
  if (!compiled.ok) return false;
  okay &= Check(compiled.configuration.modifiers.size() == 2,
                "multi-family mutation window did not lower to two modifiers");
  okay &= Check(compiled.configuration.modifierWindowGroups.size() == 2 &&
                    compiled.configuration.modifierWindowGroups[0] == 0 &&
                    compiled.configuration.modifierWindowGroups[1] == 0,
                "lowered modifiers lost their visual mutation-window identity");
  return okay;
}

VisualProgram PointObjectiveProgram() {
  Builder b;
  const VisualNodeId start = b.add("flow/when-start", 80.0, 60.0);
  b.top(start);
  const VisualNodeId search = AddSearchPolicy(b, start);
  const VisualNodeId choose = b.add("flow/set-objective");
  b.statement(search, "body", choose);
  const VisualNodeId minimize = b.add("objective/minimize");
  b.input(choose, "score", minimize);
  const VisualNodeId distance = b.add("math/distance");
  b.input(minimize, "value", distance);
  const VisualNodeId car = b.add("simulation/car-position");
  const VisualNodeId point = b.add("targets/point");
  b.input(distance, "a", car);
  b.input(distance, "b", point);
  b.input(point, "x", b.literal("values/meters", "12.5"));
  b.input(point, "y", b.literal("values/meters", "3"));
  b.input(point, "z", b.literal("values/meters", "-8.25"));
  const VisualNodeId range = b.add("time/range");
  b.input(range, "from", b.literal("values/milliseconds", "3000"));
  b.input(range, "to", b.literal("values/milliseconds", "5500"));
  b.input(minimize, "range", range);

  const VisualNodeId window = b.add("flow/mutation-window");
  b.statement(search, "body", window);
  const VisualNodeId mutationRange = b.add("time/range");
  b.input(mutationRange, "from", b.literal("values/milliseconds", "1000"));
  b.input(mutationRange, "to", b.literal("values/milliseconds", "5990"));
  b.input(window, "range", mutationRange);
  b.input(window, "seed", b.literal("values/integer", "1179926867"));
  b.statement(window, "body", b.add("mutate/reroll-steering"));
  const VisualNodeId simulate = AddSimulationStep(b, search);
  SetIterationPipeline(b, search, window, simulate, choose);
  return std::move(b.program);
}

bool TestPointObjectiveLowersToNativeEvaluator() {
  const VisualProgram program = PointObjectiveProgram();
  const VisualProgramValidation validation = ValidateVisualProgram(program);
  bool okay = Check(validation.ok, "point objective did not type-check");
  if (!validation.ok) {
    for (const auto &error : validation.errors)
      std::cerr << error << '\n';
    return false;
  }
  SearchComponentConfiguration base{DefaultSearchAlgorithmConfiguration(),
                                    DefaultModifierConfigurations(),
                                    DefaultEvaluationTargetConfiguration()};
  const CompileResult compiled = CompileVisualProgram(program, base);
  okay &= Check(compiled.ok, "point objective did not compile");
  if (!compiled.ok) {
    for (const auto &error : compiled.errors)
      std::cerr << error << '\n';
    return false;
  }
  okay &= Check(compiled.configuration.evaluationTarget.id ==
                    kPointTargetEvaluationId,
                "point objective did not use native point-target evaluator");
  const auto &settings = compiled.configuration.evaluationTarget.settings;
  okay &= Check(settings.at("minTimeMs") == "3000", "wrong point min time");
  okay &= Check(settings.at("maxTimeMs") == "5500", "wrong point max time");
  okay &= Check(settings.at("x") == "12.5", "wrong point x");
  okay &= Check(settings.at("y") == "3", "wrong point y");
  okay &= Check(settings.at("z") == "-8.25", "wrong point z");
  return okay;
}

VisualProgram GenericObjectiveProgram() {
  Builder b;
  const VisualNodeId start = b.add("flow/when-start");
  b.top(start);
  const VisualNodeId search = AddSearchPolicy(b, start);

  const VisualNodeId choose = b.add("flow/set-objective");
  b.statement(search, "body", choose);
  const VisualNodeId gate = b.add("objective/only-when");
  b.input(choose, "score", gate);
  const VisualNodeId maximize = b.add("objective/maximize");
  b.input(gate, "score", maximize);
  const VisualNodeId add = b.add("math/add");
  b.input(add, "a", b.add("simulation/car-speed"));
  b.input(add, "b", b.literal("values/number", "5"));
  b.input(maximize, "value", add);
  const VisualNodeId objectiveRange = b.add("time/range");
  b.input(objectiveRange, "from", b.literal("values/milliseconds", "1000"));
  b.input(objectiveRange, "to", b.literal("values/milliseconds", "6000"));
  b.input(maximize, "range", objectiveRange);

  const VisualNodeId condition = b.add("conditions/greater-equal");
  const VisualNodeId kmh = b.add("math/kmh");
  b.input(kmh, "value", b.add("simulation/car-speed"));
  b.input(condition, "a", kmh);
  b.input(condition, "b", b.literal("values/number", "36"));
  b.input(gate, "condition", condition);

  const VisualNodeId window = b.add("flow/mutation-window");
  b.statement(search, "body", window);
  const VisualNodeId mutationRange = b.add("time/range");
  b.input(mutationRange, "from", b.literal("values/milliseconds", "1000"));
  b.input(mutationRange, "to", b.literal("values/milliseconds", "5990"));
  b.input(window, "range", mutationRange);
  b.input(window, "seed", b.literal("values/integer", "42"));
  b.statement(window, "body", b.add("mutate/reroll-steering"));
  const VisualNodeId simulationSpeed = b.add("conditions/greater-equal");
  const VisualNodeId simulationKmh = b.add("math/kmh");
  b.input(simulationKmh, "value", b.add("simulation/car-speed"));
  b.input(simulationSpeed, "a", simulationKmh);
  b.input(simulationSpeed, "b", b.literal("values/number", "36"));
  const VisualNodeId simulationNotSliding = b.add("conditions/not");
  b.input(simulationNotSliding, "value", b.add("simulation/sliding"));
  const VisualNodeId simulationWhere = b.add("conditions/and");
  b.input(simulationWhere, "a", simulationSpeed);
  b.input(simulationWhere, "b", simulationNotSliding);
  const VisualNodeId simulate =
      AddSimulationStep(b, search, "7000", simulationWhere);
  SetIterationPipeline(b, search, window, simulate, choose);
  return std::move(b.program);
}

bool TestGenericObjectiveFallsBackToCpuExpressionEvaluator() {
  const VisualProgram program = GenericObjectiveProgram();
  const VisualProgramValidation validation = ValidateVisualProgram(program);
  bool okay = Check(validation.ok, "generic objective did not type-check");
  if (!validation.ok) return false;

  const CompileResult compiled = CompileVisualProgram(
      program,
      {DefaultSearchAlgorithmConfiguration(), DefaultModifierConfigurations(),
       DefaultEvaluationTargetConfiguration()});
  okay &= Check(compiled.ok, "generic objective did not compile");
  if (!compiled.ok) {
    for (const auto &error : compiled.errors) std::cerr << error << '\n';
    return false;
  }
  const OptionConfiguration &target = compiled.configuration.evaluationTarget;
  okay &= Check(target.id == kVisualExpressionEvaluationId,
                "generic objective did not select expression fallback");
  okay &= Check(target.settings.at("expression") == "add carspeed num 5",
                "generic score expression encoded incorrectly");
  okay &= Check(target.settings.at("condition") == "ge kmh carspeed num 36",
                "generic condition expression encoded incorrectly");
  okay &= Check(compiled.simulationHorizonMs &&
                    *compiled.simulationHorizonMs == "7000",
                "generic program lost its explicit simulation horizon");
  okay &= Check(compiled.conditionProgram.has_value(),
                "simulate where did not compile to a condition program");
  if (compiled.conditionProgram) {
    forevervalidator::experimental::PhysicsSandboxStateView previous;
    forevervalidator::experimental::PhysicsSandboxStateView current;
    ConditionExecutionContext context;
    current.car.linearSpeed = {9.0f, 0.0f, 0.0f};
    current.car.sliding = false;
    okay &= Check(!compiled.conditionProgram->Evaluate(previous, current, context),
                  "simulate where accepted speed below its threshold");
    current.car.linearSpeed = {12.0f, 0.0f, 0.0f};
    current.car.sliding = true;
    okay &= Check(!compiled.conditionProgram->Evaluate(previous, current, context),
                  "simulate where ignored its not-sliding predicate");
    current.car.sliding = false;
    okay &= Check(compiled.conditionProgram->Evaluate(previous, current, context),
                  "simulate where rejected a matching tick");
  }
  okay &= Check(!ValidateSearchComponents(compiled.configuration, 10u, 7000u),
                "generic runtime configuration failed normal search validation");

  const EvaluationTargetRegistration *const registration =
      FindEvaluationTarget(kVisualExpressionEvaluationId);
  okay &= Check(registration != nullptr,
                "internal expression evaluator is not registered for runtime");
  okay &= Check(std::none_of(EvaluationTargetRegistry().begin(),
                             EvaluationTargetRegistry().end(),
                             [](const EvaluationTargetRegistration &candidate) {
                               return candidate.id ==
                                      kVisualExpressionEvaluationId;
                             }),
                "internal expression evaluator leaked into the public registry");
  if (registration == nullptr) return false;
  okay &= Check(!registration->validateSettings(target.settings, 10u),
                "compiled generic expression settings are invalid");
  auto evaluator = registration->create(target.settings, 10u);
  const EvaluationPlan plan = evaluator->Plan(7000, 1000, 10u);
  okay &= Check(plan.startTimeMs == 1000 && plan.endTimeMs == 6000,
                "generic expression evaluation window is wrong");
  auto session = evaluator->CreateSession();
  forevervalidator::experimental::PhysicsSandboxStateView state;
  state.timeMs = 2000;
  state.car.linearSpeed = {6.0f, 0.0f, 0.0f};
  okay &= Check(!session->Observe(std::nullopt, state),
                "generic objective ignored its only-when gate");
  state.car.linearSpeed = {12.0f, 0.0f, 0.0f};
  const auto sample = session->Observe(std::nullopt, state);
  okay &= Check(sample && std::abs(sample->score - 17.0) < 1e-9,
                "generic objective evaluated the expression incorrectly");
  return okay;
}

VisualProgram GenericFirstTimeProgram() {
  Builder b;
  const VisualNodeId start = b.add("flow/when-start");
  b.top(start);
  const VisualNodeId search = AddSearchPolicy(b, start);

  const VisualNodeId choose = b.add("flow/set-objective");
  b.statement(search, "body", choose);
  const VisualNodeId first = b.add("objective/first-time");
  b.input(choose, "score", first);

  const VisualNodeId condition = b.add("conditions/and");
  const VisualNodeId checkpointGate = b.add("conditions/greater-equal");
  b.input(checkpointGate, "a", b.add("simulation/checkpoint-count"));
  b.input(checkpointGate, "b", b.literal("values/integer", "3"));
  const VisualNodeId notSliding = b.add("conditions/not");
  b.input(notSliding, "value", b.add("simulation/sliding"));
  b.input(condition, "a", checkpointGate);
  b.input(condition, "b", notSliding);
  b.input(first, "condition", condition);

  const VisualNodeId objectiveRange = b.add("time/range");
  b.input(objectiveRange, "from", b.literal("values/milliseconds", "1000"));
  b.input(objectiveRange, "to", b.literal("values/milliseconds", "5000"));
  b.input(first, "range", objectiveRange);

  const VisualNodeId window = b.add("flow/mutation-window");
  b.statement(search, "body", window);
  const VisualNodeId mutationRange = b.add("time/range");
  b.input(mutationRange, "from", b.literal("values/milliseconds", "1000"));
  b.input(mutationRange, "to", b.literal("values/milliseconds", "4990"));
  b.input(window, "range", mutationRange);
  b.input(window, "seed", b.literal("values/integer", "7"));
  b.statement(window, "body", b.add("mutate/reroll-steering"));
  const VisualNodeId simulate = AddSimulationStep(b, search);
  SetIterationPipeline(b, search, window, simulate, choose);
  return std::move(b.program);
}

bool TestGenericFirstTimePredicateFallsBackToCpuExpressionEvaluator() {
  const VisualProgram program = GenericFirstTimeProgram();
  const VisualProgramValidation validation = ValidateVisualProgram(program);
  bool okay = Check(validation.ok, "generic first-time predicate did not type-check");
  if (!validation.ok) return false;

  const CompileResult compiled = CompileVisualProgram(
      program,
      {DefaultSearchAlgorithmConfiguration(), DefaultModifierConfigurations(),
       DefaultEvaluationTargetConfiguration()});
  okay &= Check(compiled.ok, "generic first-time predicate did not compile");
  if (!compiled.ok) {
    for (const auto &error : compiled.errors) std::cerr << error << '\n';
    return false;
  }
  const OptionConfiguration &target = compiled.configuration.evaluationTarget;
  okay &= Check(target.id == kVisualExpressionEvaluationId,
                "generic first-time predicate did not use expression runtime");
  okay &= Check(target.settings.at("mode") == "first-time" &&
                    target.settings.at("direction") == "minimize" &&
                    target.settings.at("minTimeMs") == "1000" &&
                    target.settings.at("maxTimeMs") == "5000",
                "generic first-time runtime settings are wrong");
  okay &= Check(target.settings.at("condition") ==
                    "and ge checkpoints num 3 not sliding",
                "generic first-time predicate encoded incorrectly");

  const EvaluationTargetRegistration *const registration =
      FindEvaluationTarget(kVisualExpressionEvaluationId);
  if (!Check(registration != nullptr,
             "generic first-time runtime registration missing")) {
    return false;
  }
  auto evaluator = registration->create(target.settings, 10u);
  const EvaluationPlan plan = evaluator->Plan(6000, 1000, 10u);
  okay &= Check(plan.startTimeMs == 1000 && plan.endTimeMs == 5000,
                "generic first-time evaluation range is wrong");
  auto session = evaluator->CreateSession();
  forevervalidator::experimental::PhysicsSandboxStateView state;
  state.timeMs = 2000;
  state.checkpointsCollected = 2;
  okay &= Check(!session->Observe(std::nullopt, state),
                "first-time predicate accepted too few checkpoints");
  state.timeMs = 2500;
  state.checkpointsCollected = 3;
  state.car.sliding = true;
  okay &= Check(!session->Observe(std::nullopt, state),
                "first-time predicate ignored boolean composition");
  state.timeMs = 3000;
  state.car.sliding = false;
  const auto sample = session->Observe(std::nullopt, state);
  okay &= Check(sample && sample->score == 3000.0 && sample->timeMs == 3000.0,
                "first-time predicate returned the wrong matching time");
  state.timeMs = 3010;
  okay &= Check(!session->Observe(std::nullopt, state),
                "first-time predicate reported more than its first match");
  const EvaluationSample earlier{2900.0, 2900.0, {}};
  okay &= Check(sample && evaluator->IsBetter(earlier, *sample),
                "first-time predicate did not rank earlier matches first");
  return okay;
}

bool TestWindowedInsideBoxUsesGenericPredicateRuntime() {
  Builder b;
  const VisualNodeId start = b.add("flow/when-start");
  b.top(start);
  const VisualNodeId search = AddSearchPolicy(b, start);
  const VisualNodeId choose = b.add("flow/set-objective");
  b.statement(search, "body", choose);
  const VisualNodeId first = b.add("objective/first-time");
  b.input(choose, "score", first);
  const VisualNodeId inside = b.add("conditions/inside");
  b.input(first, "condition", inside);
  b.input(inside, "position", b.add("simulation/car-position"));
  const VisualNodeId box = b.add("targets/box");
  b.input(inside, "volume", box);
  const VisualNodeId center = b.add("targets/point");
  b.input(center, "x", b.literal("values/meters", "10"));
  b.input(center, "y", b.literal("values/meters", "0"));
  b.input(center, "z", b.literal("values/meters", "0"));
  b.input(box, "center", center);
  const VisualNodeId size = b.add("targets/size");
  b.input(size, "x", b.literal("values/meters", "4"));
  b.input(size, "y", b.literal("values/meters", "4"));
  b.input(size, "z", b.literal("values/meters", "4"));
  b.input(box, "size", size);
  const VisualNodeId range = b.add("time/range");
  b.input(range, "from", b.literal("values/milliseconds", "1000"));
  b.input(range, "to", b.literal("values/milliseconds", "5000"));
  b.input(first, "range", range);

  const VisualNodeId window = b.add("flow/mutation-window");
  b.statement(search, "body", window);
  const VisualNodeId mutationRange = b.add("time/range");
  b.input(mutationRange, "from", b.literal("values/milliseconds", "1000"));
  b.input(mutationRange, "to", b.literal("values/milliseconds", "4990"));
  b.input(window, "range", mutationRange);
  b.input(window, "seed", b.literal("values/integer", "9"));
  b.statement(window, "body", b.add("mutate/reroll-steering"));
  const VisualNodeId simulate = AddSimulationStep(b, search);
  SetIterationPipeline(b, search, window, simulate, choose);

  const VisualProgramValidation validation = ValidateVisualProgram(b.program);
  bool okay = Check(validation.ok, "windowed inside-box predicate did not type-check");
  if (!validation.ok) return false;
  const CompileResult compiled = CompileVisualProgram(
      b.program,
      {DefaultSearchAlgorithmConfiguration(), DefaultModifierConfigurations(),
       DefaultEvaluationTargetConfiguration()});
  okay &= Check(compiled.ok, "windowed inside-box predicate did not compile");
  if (!compiled.ok) {
    for (const auto &error : compiled.errors) std::cerr << error << '\n';
    return false;
  }
  const OptionConfiguration &target = compiled.configuration.evaluationTarget;
  okay &= Check(target.id == kVisualExpressionEvaluationId &&
                    target.settings.at("mode") == "first-time",
                "windowed inside-box predicate did not use generic runtime");
  okay &= Check(target.settings.at("condition") ==
                    "insidebox carpos vec num 10 num 0 num 0 "
                    "vec num 4 num 4 num 4",
                "inside-box predicate encoded incorrectly");

  const auto *const registration = FindEvaluationTarget(kVisualExpressionEvaluationId);
  if (!Check(registration != nullptr, "generic inside-box runtime missing"))
    return false;
  auto session = registration->create(target.settings, 10u)->CreateSession();
  forevervalidator::experimental::PhysicsSandboxStateView state;
  state.timeMs = 2000;
  state.car.position = {7.0f, 0.0f, 0.0f};
  okay &= Check(!session->Observe(std::nullopt, state),
                "inside-box predicate accepted an outside position");
  state.timeMs = 2500;
  state.car.position = {9.0f, 0.0f, 0.0f};
  const auto sample = session->Observe(std::nullopt, state);
  okay &= Check(sample && sample->score == 2500.0,
                "inside-box predicate did not report the first inside tick");
  return okay;
}

bool TestWindowedInsidePrismUsesGenericPredicateRuntime() {
  Builder b;
  const VisualNodeId start = b.add("flow/when-start");
  b.top(start);
  const VisualNodeId search = AddSearchPolicy(b, start);
  const VisualNodeId choose = b.add("flow/set-objective");
  const VisualNodeId first = b.add("objective/first-time");
  b.input(choose, "score", first);
  const VisualNodeId inside = b.add("conditions/inside");
  b.input(first, "condition", inside);
  b.input(inside, "position", b.add("simulation/car-position"));

  const VisualNodeId prism = b.add("targets/prism");
  b.program.find(prism)->fields["plane"] = "xz";
  b.input(inside, "volume", prism);
  const VisualNodeId origin = b.add("targets/point");
  b.input(origin, "x", b.literal("values/meters", "0"));
  b.input(origin, "y", b.literal("values/meters", "0"));
  b.input(origin, "z", b.literal("values/meters", "0"));
  b.input(prism, "origin", origin);
  b.input(prism, "depth", b.literal("values/meters", "5"));
  b.input(prism, "polygon",
          b.literal("values/polygon", "-2,-2;2,-2;2,2;-2,2"));

  const VisualNodeId range = b.add("time/range");
  b.input(range, "from", b.literal("values/milliseconds", "1000"));
  b.input(range, "to", b.literal("values/milliseconds", "5000"));
  b.input(first, "range", range);

  const VisualNodeId window = b.add("flow/mutation-window");
  const VisualNodeId mutationRange = b.add("time/range");
  b.input(mutationRange, "from", b.literal("values/milliseconds", "1000"));
  b.input(mutationRange, "to", b.literal("values/milliseconds", "4990"));
  b.input(window, "range", mutationRange);
  b.input(window, "seed", b.literal("values/integer", "11"));
  b.statement(window, "body", b.add("mutate/reroll-steering"));
  const VisualNodeId simulate = AddSimulationStep(b, search);
  SetIterationPipeline(b, search, window, simulate, choose);

  const VisualProgramValidation validation = ValidateVisualProgram(b.program);
  bool okay =
      Check(validation.ok, "windowed inside-prism predicate did not type-check");
  if (!validation.ok) return false;
  const CompileResult compiled = CompileVisualProgram(
      b.program,
      {DefaultSearchAlgorithmConfiguration(), DefaultModifierConfigurations(),
       DefaultEvaluationTargetConfiguration()});
  okay &= Check(compiled.ok, "windowed inside-prism predicate did not compile");
  if (!compiled.ok) {
    for (const auto &error : compiled.errors) std::cerr << error << '\n';
    return false;
  }
  const OptionConfiguration &target = compiled.configuration.evaluationTarget;
  okay &= Check(target.id == kVisualExpressionEvaluationId &&
                    target.settings.at("mode") == "first-time",
                "windowed inside-prism predicate did not use generic runtime");
  okay &= Check(target.settings.at("condition") ==
                    "insideprism carpos vec num 0 num 0 num 0 num 5 xz 4 "
                    "-2 -2 2 -2 2 2 -2 2",
                "inside-prism predicate encoded incorrectly");

  const auto *const registration = FindEvaluationTarget(kVisualExpressionEvaluationId);
  if (!Check(registration != nullptr, "generic inside-prism runtime missing"))
    return false;
  auto evaluator = registration->create(target.settings, 10u);
  auto session = evaluator->CreateSession();
  forevervalidator::experimental::PhysicsSandboxStateView state;
  state.timeMs = 2000;
  state.car.position = {3.0f, 1.0f, 0.0f};
  okay &= Check(!session->Observe(std::nullopt, state),
                "inside-prism predicate accepted an outside position");
  state.timeMs = 2500;
  state.car.position = {1.0f, 2.0f, -1.0f};
  const auto sample = session->Observe(std::nullopt, state);
  okay &= Check(sample && sample->score == 2500.0,
                "inside-prism predicate did not report the first inside tick");

  std::string cudaError;
  const auto cuda =
      BuildCudaVisualExpressionEvaluator(target.settings, 10u, &cudaError);
  okay &= Check(cuda.has_value(),
                "inside-prism expression did not compile to CUDA");
  if (!cuda) {
    if (!cudaError.empty()) std::cerr << cudaError << '\n';
    return false;
  }
  using Opcode = forevervalidator::experimental::
      PhysicsSandboxCudaExpressionOpcode;
  okay &= Check(cuda->prisms.size() == 1u &&
                    cuda->prismVertices.size() == 4u &&
                    !cuda->condition.empty() &&
                    cuda->condition.back().opcode == Opcode::InsidePrism,
                "inside-prism CUDA geometry/bytecode is incomplete");
  return okay;
}

bool TestTypeMismatchRejected() {
  VisualProgram program = PointObjectiveProgram();
  VisualNode *point = nullptr;
  for (auto &[id, node] : program.nodes) {
    static_cast<void>(id);
    if (node.definitionId == "targets/point")
      point = &node;
  }
  if (!Check(point != nullptr, "point missing in type test"))
    return false;
  const VisualNodeId x = point->inputs.at("x");
  program.find(x)->definitionId = "simulation/car-position";
  const VisualProgramValidation validation = ValidateVisualProgram(program);
  return Check(!validation.ok, "meters socket accepted a position reporter");
}

bool TestCycleRejected() {
  Builder b;
  const VisualNodeId add = b.add("math/add");
  b.top(add);
  b.input(add, "a", add);
  b.input(add, "b", b.literal("values/number", "1"));
  const VisualProgramValidation validation = ValidateVisualProgram(b.program);
  return Check(!validation.ok, "cycle was accepted");
}

bool TestDuplicateConnectionRejected() {
  Builder b;
  const VisualNodeId add = b.add("math/add");
  const VisualNodeId literal = b.literal("values/number", "1.5");
  b.top(add);
  b.input(add, "a", literal);
  b.input(add, "b", literal);
  const VisualProgramValidation validation = ValidateVisualProgram(b.program);
  return Check(!validation.ok,
               "one reporter was accepted in two sockets of the same parent");
}

bool TestInvalidFieldRejected() {
  Builder b;
  const VisualNodeId literal = b.literal("values/integer", "1.5");
  b.top(literal);
  const VisualProgramValidation validation = ValidateVisualProgram(b.program);
  return Check(!validation.ok, "fractional integer field was accepted");
}

bool TestStatementFamilyMismatchRejected() {
  Builder b;
  const VisualNodeId start = b.add("flow/when-start");
  b.top(start);
  b.statement(start, "body", b.add("mutate/reroll-steering"));
  const VisualProgramValidation validation = ValidateVisualProgram(b.program);
  return Check(!validation.ok,
               "root command stack accepted a mutation-only command");
}

bool TestIterationOrderRejected() {
  VisualProgram program = PointObjectiveProgram();
  VisualNode *search = nullptr;
  VisualNodeId mutation = 0;
  VisualNodeId simulate = 0;
  VisualNodeId choose = 0;
  for (auto &[id, node] : program.nodes) {
    if (node.definitionId == "search/basic-brute-force") search = &node;
    if (node.definitionId == "flow/mutation-window") mutation = id;
    if (node.definitionId == "flow/simulate") simulate = id;
    if (node.definitionId == "flow/set-objective") choose = id;
  }
  if (!Check(search != nullptr && mutation != 0 && simulate != 0 && choose != 0,
             "iteration-order fixture is incomplete")) {
    return false;
  }
  search->statements["body"] = {choose, mutation, simulate};
  const CompileResult compiled = CompileVisualProgram(
      program,
      {DefaultSearchAlgorithmConfiguration(), DefaultModifierConfigurations(),
       DefaultEvaluationTargetConfiguration()});
  return Check(!compiled.ok,
               "compiler accepted iteration steps in a dishonest visual order");
}

bool TestSimulationPredicateSupportsComposedGeometryAndRotation() {
  const VisualProgram program = SimulationPredicateProgram([](Builder &b) {
    const auto point = [&](const std::string &x, const std::string &y,
                           const std::string &z) {
      const VisualNodeId result = b.add("targets/point");
      b.input(result, "x", b.literal("values/meters", x));
      b.input(result, "y", b.literal("values/meters", y));
      b.input(result, "z", b.literal("values/meters", z));
      return result;
    };

    const VisualNodeId origin = point("0", "0", "0");
    const VisualNodeId dynamicX = b.add("math/distance");
    b.input(dynamicX, "a", b.add("simulation/car-position"));
    b.input(dynamicX, "b", origin);
    const VisualNodeId center = b.add("targets/point");
    b.input(center, "x", dynamicX);
    b.input(center, "y", b.literal("values/meters", "0"));
    b.input(center, "z", b.literal("values/meters", "0"));
    const VisualNodeId size = b.add("targets/size");
    b.input(size, "x", b.literal("values/meters", "4"));
    b.input(size, "y", b.literal("values/meters", "4"));
    b.input(size, "z", b.literal("values/meters", "4"));
    const VisualNodeId box = b.add("targets/box");
    b.input(box, "center", center);
    b.input(box, "size", size);
    const VisualNodeId inside = b.add("conditions/inside");
    b.input(inside, "position", b.add("simulation/car-position"));
    b.input(inside, "volume", box);

    const VisualNodeId normalized = b.add("math/normalize");
    b.input(normalized, "value", b.add("simulation/car-velocity"));
    const VisualNodeId direction = b.add("targets/direction");
    b.input(direction, "x", b.literal("values/number", "1"));
    b.input(direction, "y", b.literal("values/number", "0"));
    b.input(direction, "z", b.literal("values/number", "0"));
    const VisualNodeId dot = b.add("math/dot");
    b.input(dot, "a", normalized);
    b.input(dot, "b", direction);
    const VisualNodeId facing = b.add("conditions/greater-equal");
    b.input(facing, "a", dot);
    b.input(facing, "b", b.literal("values/number", "0.9"));

    const VisualNodeId targetRotation = b.add("targets/rotation");
    b.input(targetRotation, "yaw", b.literal("values/degrees", "0"));
    b.input(targetRotation, "pitch", b.literal("values/degrees", "0"));
    b.input(targetRotation, "roll", b.literal("values/degrees", "0"));
    const VisualNodeId rotationDistance = b.add("math/rotation-distance");
    b.input(rotationDistance, "a", b.add("simulation/car-rotation"));
    b.input(rotationDistance, "b", targetRotation);
    const VisualNodeId rotationClose = b.add("conditions/less");
    b.input(rotationClose, "a", rotationDistance);
    b.input(rotationClose, "b", b.literal("values/number", "0.1"));

    const VisualNodeId geometryAndFacing = b.add("conditions/and");
    b.input(geometryAndFacing, "a", inside);
    b.input(geometryAndFacing, "b", facing);
    const VisualNodeId result = b.add("conditions/and");
    b.input(result, "a", geometryAndFacing);
    b.input(result, "b", rotationClose);
    return result;
  });

  const VisualProgramValidation validation = ValidateVisualProgram(program);
  bool okay = Check(validation.ok,
                    "advanced simulate-where geometry did not type-check");
  if (!validation.ok) {
    for (const auto &error : validation.errors) std::cerr << error << '\n';
    return false;
  }
  const CompileResult compiled = CompileVisualProgram(
      program,
      {DefaultSearchAlgorithmConfiguration(), DefaultModifierConfigurations(),
       DefaultEvaluationTargetConfiguration()});
  okay &= Check(compiled.ok,
                "advanced simulate-where geometry did not compile");
  if (!compiled.ok) {
    for (const auto &error : compiled.errors) std::cerr << error << '\n';
    return false;
  }
  if (!Check(compiled.conditionProgram.has_value(),
             "advanced simulate-where lost its condition program"))
    return false;

  using Op = forevervalidator::experimental::PhysicsSandboxCudaConditionOpcode;
  const auto &instructions = compiled.conditionProgram->cuda.instructions;
  const auto has = [&](Op opcode) {
    return std::any_of(instructions.begin(), instructions.end(),
                       [opcode](const auto &instruction) {
                         return instruction.opcode == opcode;
                       });
  };
  okay &= Check(has(Op::ComposeVector) && has(Op::Direction) &&
                    has(Op::Normalize) && has(Op::Dot) && has(Op::InsideBox) &&
                    has(Op::RotationSource) && has(Op::Rotation) &&
                    has(Op::RotationDistance),
                "advanced simulate-where omitted typed condition opcodes");

  forevervalidator::experimental::PhysicsSandboxStateView previous;
  forevervalidator::experimental::PhysicsSandboxStateView current;
  ConditionExecutionContext context;
  current.car.position = {1.0f, 0.0f, 0.0f};
  current.car.linearSpeed = {10.0f, 0.0f, 0.0f};
  current.car.rotationW = 1.0f;
  okay &= Check(compiled.conditionProgram->Evaluate(previous, current, context),
                "advanced simulate-where rejected a matching state");
  current.car.linearSpeed = {-10.0f, 0.0f, 0.0f};
  okay &= Check(!compiled.conditionProgram->Evaluate(previous, current, context),
                "normalize/dot predicate accepted the opposite direction");
  current.car.linearSpeed = {10.0f, 0.0f, 0.0f};
  current.car.rotationY = 0.70710677f;
  current.car.rotationW = 0.70710677f;
  okay &= Check(!compiled.conditionProgram->Evaluate(previous, current, context),
                "rotation-distance predicate ignored car rotation");
  return okay;
}

bool TestSimulationPredicateSupportsPrismsAndScalarLanguage() {
  const VisualProgram program = SimulationPredicateProgram([](Builder &b) {
    const VisualNodeId origin = b.add("targets/point");
    b.input(origin, "x", b.literal("values/meters", "0"));
    b.input(origin, "y", b.literal("values/meters", "0"));
    b.input(origin, "z", b.literal("values/meters", "0"));
    const VisualNodeId polygon =
        b.literal("values/polygon", "-2,-2;2,-2;2,2;-2,2");
    const VisualNodeId prism = b.add("targets/prism");
    b.input(prism, "origin", origin);
    b.input(prism, "depth", b.literal("values/meters", "5"));
    b.input(prism, "polygon", polygon);
    b.program.find(prism)->fields["plane"] = "xz";
    const VisualNodeId inside = b.add("conditions/inside");
    b.input(inside, "position", b.add("simulation/car-position"));
    b.input(inside, "volume", prism);

    const VisualNodeId blend = b.add("math/weighted-blend");
    b.input(blend, "a", b.add("simulation/stunt-points"));
    b.input(blend, "b", b.add("simulation/time"));
    b.input(blend, "weight", b.literal("values/percent", "50"));
    const VisualNodeId ratio = b.add("math/percent-ratio");
    b.input(ratio, "value", b.literal("values/percent", "50"));
    const VisualNodeId multiply = b.add("math/multiply");
    b.input(multiply, "a", blend);
    b.input(multiply, "b", ratio);
    const VisualNodeId subtract = b.add("math/subtract");
    b.input(subtract, "a", multiply);
    b.input(subtract, "b", b.literal("values/number", "10"));
    const VisualNodeId absolute = b.add("math/abs");
    b.input(absolute, "value", subtract);
    const VisualNodeId clamp = b.add("math/clamp");
    b.input(clamp, "value", absolute);
    b.input(clamp, "minimum", b.literal("values/number", "0"));
    b.input(clamp, "maximum", b.literal("values/number", "10"));
    const VisualNodeId minimum = b.add("math/min");
    b.input(minimum, "a", clamp);
    b.input(minimum, "b", b.add("simulation/checkpoint-count"));
    const VisualNodeId maximum = b.add("math/max");
    b.input(maximum, "a", minimum);
    b.input(maximum, "b", b.literal("values/number", "1"));
    const VisualNodeId scalarOkay = b.add("conditions/greater-equal");
    b.input(scalarOkay, "a", maximum);
    b.input(scalarOkay, "b", b.literal("values/number", "2"));

    const VisualNodeId localMagnitude = b.add("math/magnitude");
    b.input(localMagnitude, "value", b.add("simulation/car-local-velocity"));
    const VisualNodeId moving = b.add("conditions/greater");
    b.input(moving, "a", localMagnitude);
    b.input(moving, "b", b.literal("values/number", "0"));
    const VisualNodeId finishPositive = b.add("conditions/greater");
    b.input(finishPositive, "a", b.add("simulation/finish-time"));
    b.input(finishPositive, "b", b.literal("values/milliseconds", "0"));
    const VisualNodeId completed = b.add("conditions/and");
    b.input(completed, "a", b.add("simulation/race-completed"));
    b.input(completed, "b", finishPositive);
    const VisualNodeId notFreewheeling = b.add("conditions/not");
    b.input(notFreewheeling, "value", b.add("simulation/freewheeling"));
    const VisualNodeId completionOrGrip = b.add("conditions/or");
    b.input(completionOrGrip, "a", completed);
    b.input(completionOrGrip, "b", notFreewheeling);

    const VisualNodeId first = b.add("conditions/and");
    b.input(first, "a", inside);
    b.input(first, "b", scalarOkay);
    const VisualNodeId second = b.add("conditions/and");
    b.input(second, "a", moving);
    b.input(second, "b", completionOrGrip);
    const VisualNodeId result = b.add("conditions/and");
    b.input(result, "a", first);
    b.input(result, "b", second);
    return result;
  });

  const VisualProgramValidation validation = ValidateVisualProgram(program);
  bool okay = Check(validation.ok,
                    "prism/scalar simulate-where did not type-check");
  if (!validation.ok) {
    for (const auto &error : validation.errors) std::cerr << error << '\n';
    return false;
  }
  const CompileResult compiled = CompileVisualProgram(
      program,
      {DefaultSearchAlgorithmConfiguration(), DefaultModifierConfigurations(),
       DefaultEvaluationTargetConfiguration()});
  okay &= Check(compiled.ok, "prism/scalar simulate-where did not compile");
  if (!compiled.ok) {
    for (const auto &error : compiled.errors) std::cerr << error << '\n';
    return false;
  }
  if (!Check(compiled.conditionProgram.has_value(),
             "prism/scalar simulate-where lost its condition program"))
    return false;
  const ConditionProgram &condition = *compiled.conditionProgram;
  okay &= Check(condition.cuda.prisms.size() == 1u &&
                    condition.cuda.prismVertices.size() == 4u,
                "simulate-where prism geometry was not lowered");
  using Op = forevervalidator::experimental::PhysicsSandboxCudaConditionOpcode;
  const auto has = [&](Op opcode) {
    return std::any_of(condition.cuda.instructions.begin(),
                       condition.cuda.instructions.end(),
                       [opcode](const auto &instruction) {
                         return instruction.opcode == opcode;
                       });
  };
  okay &= Check(has(Op::InsidePrism) && has(Op::WeightedBlend) &&
                    has(Op::PercentRatio) && has(Op::Absolute) &&
                    has(Op::Clamp) && has(Op::Minimum) && has(Op::Maximum) &&
                    has(Op::Magnitude) && has(Op::LogicalOr) &&
                    has(Op::LogicalNot),
                "prism/scalar simulate-where omitted composed opcodes");

  forevervalidator::experimental::PhysicsSandboxStateView previous;
  forevervalidator::experimental::PhysicsSandboxStateView current;
  ConditionExecutionContext context;
  current.car.position = {0.0f, 1.0f, 0.0f};
  current.car.localSpeed = {1.0f, 0.0f, 0.0f};
  current.stuntsScore = 10u;
  current.timeMs = 20u;
  current.checkpointsCollected = 4u;
  current.raceCompleted = true;
  current.finishTimeMs = 100u;
  current.finishTime = forevervalidator::FinishTimeEstimate{
      99999999u, 100000000u, 100000000u};
  okay &= Check(condition.Evaluate(previous, current, context),
                "prism/scalar simulate-where rejected a matching state");
  current.car.position.y = 6.0f;
  okay &= Check(!condition.Evaluate(previous, current, context),
                "simulate-where prism accepted a point beyond its depth");
  current.car.position.y = 1.0f;
  current.car.localSpeed = {};
  okay &= Check(!condition.Evaluate(previous, current, context),
                "simulate-where magnitude predicate ignored zero local speed");
  return okay;
}

bool TestNestedLegacyConditionRemainsComposable() {
  const VisualProgram program = SimulationPredicateProgram([](Builder &b) {
    const VisualNodeId legacy = b.add("conditions/legacy-script");
    b.program.find(legacy)->fields["source"] = "car.speed >= 1";
    const VisualNodeId result = b.add("conditions/and");
    b.input(result, "a", legacy);
    b.input(result, "b", b.literal("values/boolean", "true"));
    return result;
  });
  const CompileResult compiled = CompileVisualProgram(
      program,
      {DefaultSearchAlgorithmConfiguration(), DefaultModifierConfigurations(),
       DefaultEvaluationTargetConfiguration()});
  bool okay = Check(compiled.ok,
                    "nested migrated legacy condition did not compile");
  if (!compiled.ok) {
    for (const auto &error : compiled.errors) std::cerr << error << '\n';
    return false;
  }
  if (!Check(compiled.conditionProgram.has_value(),
             "nested legacy condition lost its runtime program"))
    return false;
  forevervalidator::experimental::PhysicsSandboxStateView previous;
  forevervalidator::experimental::PhysicsSandboxStateView current;
  ConditionExecutionContext context;
  current.car.linearSpeed = {2.0f, 0.0f, 0.0f};
  okay &= Check(compiled.conditionProgram->Evaluate(previous, current, context),
                "nested legacy condition rejected matching state");
  current.car.linearSpeed = {};
  okay &= Check(!compiled.conditionProgram->Evaluate(previous, current, context),
                "nested legacy condition ignored migrated script");
  return okay;
}

bool TestVisualExpressionCompilesToCudaBytecode() {
  OptionSettings settings = DefaultVisualExpressionOptionSettings();
  settings["direction"] = "minimize";
  settings["expression"] = "add kmh carspeed num 5";
  settings["condition"] =
      "and gt checkpoints num 2 not sliding";
  std::string error;
  const auto evaluator =
      BuildCudaVisualExpressionEvaluator(settings, 10u, &error);
  bool okay = Check(evaluator.has_value(),
                    "valid visual expression did not compile to CUDA");
  if (!evaluator) {
    if (!error.empty()) std::cerr << error << '\n';
    return false;
  }
  using Opcode = forevervalidator::experimental::
      PhysicsSandboxCudaExpressionOpcode;
  using Source = forevervalidator::experimental::
      PhysicsSandboxCudaExpressionSource;
  okay &= Check(!evaluator->maximize && !evaluator->firstTime,
                "CUDA expression lost objective direction/mode");
  okay &= Check(!evaluator->score.empty() &&
                    evaluator->score.back().opcode == Opcode::Add,
                "CUDA score program is not postfix composition");
  okay &= Check(evaluator->score.front().opcode == Opcode::Source &&
                    evaluator->score.front().source == Source::CarSpeed,
                "CUDA score program lost car-speed source");
  okay &= Check(!evaluator->condition.empty() &&
                    evaluator->condition.back().opcode == Opcode::LogicalAnd,
                "CUDA condition program lost Boolean composition");
  return okay;
}

bool TestVisualExpressionCudaFirstTimeAndLimits() {
  OptionSettings settings = DefaultVisualExpressionOptionSettings();
  settings["mode"] = "first-time";
  settings["condition"] = "gt checkpoints num 3";
  std::string error;
  const auto firstTime =
      BuildCudaVisualExpressionEvaluator(settings, 10u, &error);
  bool okay = Check(firstTime.has_value() && firstTime->firstTime,
                    "CUDA first-time expression lost mode");

  settings = DefaultVisualExpressionOptionSettings();
  settings["expression"] = "finishms";
  error.clear();
  okay &= Check(!BuildCudaVisualExpressionEvaluator(settings, 10u, &error) &&
                    error.find("precise finish time") != std::string::npos,
                "generic CUDA expression accepted imprecise finish-time source");

  settings = DefaultVisualExpressionOptionSettings();
  std::string deep = "num 0";
  for (int index = 0; index < 40; ++index) {
    deep = "add num 1 " + deep;
  }
  settings["expression"] = deep;
  error.clear();
  okay &= Check(!BuildCudaVisualExpressionEvaluator(settings, 10u, &error) &&
                    error.find("32 temporary") != std::string::npos,
                "CUDA expression accepted a program exceeding device stack");
  return okay;
}

} // namespace

int main() {
  bool okay = true;
  okay &= TestCatalogIsPrimitive();
  okay &= TestEveryToolboxBlockHasExecutableDefaults();
  okay &= TestEveryPredicateComposableToolboxBlockCompilesInSimulationWhere();
  okay &= TestPointObjectiveLowersToNativeEvaluator();
  okay &= TestGenericObjectiveFallsBackToCpuExpressionEvaluator();
  okay &= TestGenericFirstTimePredicateFallsBackToCpuExpressionEvaluator();
  okay &= TestWindowedInsideBoxUsesGenericPredicateRuntime();
  okay &= TestWindowedInsidePrismUsesGenericPredicateRuntime();
  okay &= TestMutationReporterSocketsLowerToNativeSettings();
  okay &= TestMutationWindowGroupingSurvivesNativeLowering();
  okay &= TestTypeMismatchRejected();
  okay &= TestCycleRejected();
  okay &= TestDuplicateConnectionRejected();
  okay &= TestInvalidFieldRejected();
  okay &= TestStatementFamilyMismatchRejected();
  okay &= TestIterationOrderRejected();
  okay &= TestSimulationPredicateSupportsComposedGeometryAndRotation();
  okay &= TestSimulationPredicateSupportsPrismsAndScalarLanguage();
  okay &= TestNestedLegacyConditionRemainsComposable();
  okay &= TestVisualExpressionCompilesToCudaBytecode();
  okay &= TestVisualExpressionCudaFirstTimeAndLimits();
  return okay ? 0 : 1;
}
