#include "app/block_editor_bridge.h"

#include "app/search_controller.h"
#include "app/visual_program_json.h"
#include "blocks/block_lowering.h"
#include "blocks/visual_catalog.h"
#include "blocks/visual_compiler.h"
#include "searches/algorithm_registry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSettings>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace forevertas::app {
namespace {

constexpr auto kStoredWorkspaceKey = "blockEditor/v3Workspace";
constexpr auto kStoredProgramKey = "blockEditor/v3Program";
constexpr auto kCorruptProgramBackupKey = "blockEditor/v3ProgramCorruptBackup";

QString ToQString(const std::string &value) {
  return QString::fromStdString(value);
}

QString JsonText(const QJsonObject &object) {
  return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

void CopyTargetValue(QJsonObject *object, const QVariantMap &target,
                     const char *key) {
  const QString name = QString::fromLatin1(key);
  const auto value = target.constFind(name);
  if (value != target.constEnd()) object->insert(name, value->toString());
}

QJsonArray ChecksForOutput(blocks::VisualValueType type) {
  QJsonArray checks;
  using T = blocks::VisualValueType;
  const auto add = [&checks](const char *value) {
    checks.push_back(QString::fromLatin1(value));
  };
  switch (type) {
  case T::None:
    break;
  case T::Scalar:
    add("scalar");
    break;
  case T::Number:
    add("number");
    add("scalar");
    break;
  case T::Integer:
    add("integer");
    add("number");
    add("scalar");
    break;
  case T::NumberRange:
    add("number_range");
    break;
  case T::IntegerRange:
    add("integer_range");
    break;
  case T::Milliseconds:
    add("milliseconds");
    add("scalar");
    break;
  case T::Meters:
    add("meters");
    add("scalar");
    break;
  case T::MetersPerSecond:
    add("meters_per_second");
    add("scalar");
    break;
  case T::Degrees:
    add("degrees");
    add("scalar");
    break;
  case T::Percent:
    add("percent");
    break;
  case T::Boolean:
    add("boolean");
    break;
  case T::Vector3:
    add("vector3");
    break;
  case T::Position3:
    add("position3");
    add("vector3");
    break;
  case T::Direction3:
    add("direction3");
    add("vector3");
    break;
  case T::Rotation3:
    add("rotation3");
    break;
  case T::Volume:
    add("volume");
    break;
  case T::Polygon2:
    add("polygon2");
    break;
  case T::TimeRange:
    add("time_range");
    break;
  case T::Score:
    add("score");
    break;
  }
  return checks;
}

QJsonArray ChecksForInput(blocks::VisualValueType type) {
  using T = blocks::VisualValueType;
  switch (type) {
  case T::None:
    return {};
  case T::Scalar:
    return {QStringLiteral("scalar")};
  case T::Number:
    return {QStringLiteral("number"), QStringLiteral("integer")};
  case T::Integer:
    return {QStringLiteral("integer")};
  case T::NumberRange:
    return {QStringLiteral("number_range")};
  case T::IntegerRange:
    return {QStringLiteral("integer_range")};
  case T::Milliseconds:
    return {QStringLiteral("milliseconds")};
  case T::Meters:
    return {QStringLiteral("meters")};
  case T::MetersPerSecond:
    return {QStringLiteral("meters_per_second")};
  case T::Degrees:
    return {QStringLiteral("degrees")};
  case T::Percent:
    return {QStringLiteral("percent")};
  case T::Boolean:
    return {QStringLiteral("boolean")};
  case T::Vector3:
    return {QStringLiteral("vector3")};
  case T::Position3:
    return {QStringLiteral("position3")};
  case T::Direction3:
    return {QStringLiteral("direction3")};
  case T::Rotation3:
    return {QStringLiteral("rotation3")};
  case T::Volume:
    return {QStringLiteral("volume")};
  case T::Polygon2:
    return {QStringLiteral("polygon2")};
  case T::TimeRange:
    return {QStringLiteral("time_range")};
  case T::Score:
    return {QStringLiteral("score")};
  }
  return {};
}

QString ShapeName(blocks::VisualBlockShape shape) {
  using S = blocks::VisualBlockShape;
  switch (shape) {
  case S::Hat:
    return QStringLiteral("hat");
  case S::Command:
    return QStringLiteral("command");
  case S::Control:
    return QStringLiteral("control");
  case S::Reporter:
    return QStringLiteral("reporter");
  case S::Predicate:
    return QStringLiteral("predicate");
  }
  return {};
}

QString FieldKindName(blocks::VisualFieldDefinition::Kind kind) {
  using K = blocks::VisualFieldDefinition::Kind;
  switch (kind) {
  case K::Number:
    return QStringLiteral("number");
  case K::Integer:
    return QStringLiteral("integer");
  case K::Boolean:
    return QStringLiteral("boolean");
  case K::Enum:
    return QStringLiteral("enum");
  case K::Text:
    return QStringLiteral("text");
  }
  return {};
}

QJsonArray StatementChecks(const std::string &family) {
  return family.empty() ? QJsonArray{} : QJsonArray{ToQString(family)};
}

QJsonObject BlocklyBlock(const std::string &definitionId,
                         const QJsonObject &fields = {},
                         const QJsonObject &inputs = {}, double x = 0.0,
                         double y = 0.0) {
  QJsonObject block{{QStringLiteral("type"),
                     ToQString(blocks::VisualBlocklyType(definitionId))}};
  if (!fields.isEmpty())
    block.insert(QStringLiteral("fields"), fields);
  if (!inputs.isEmpty())
    block.insert(QStringLiteral("inputs"), inputs);
  if (x != 0.0 || y != 0.0) {
    block.insert(QStringLiteral("x"), x);
    block.insert(QStringLiteral("y"), y);
  }
  return block;
}

QJsonObject BlockInput(QJsonObject block, bool shadow = false) {
  return {{shadow ? QStringLiteral("shadow") : QStringLiteral("block"),
           std::move(block)}};
}

QJsonObject Literal(const char *definitionId, const std::string &value,
                    bool shadow = true) {
  QString fieldValue = ToQString(value);
  if (std::string_view(definitionId) == "values/boolean") {
    fieldValue = value == "true" ? QStringLiteral("TRUE")
                                  : QStringLiteral("FALSE");
  }
  return BlockInput(
      BlocklyBlock(definitionId, {{QStringLiteral("value"), fieldValue}}),
      shadow);
}

QJsonObject NumericRangeLiteral(const char *definitionId,
                                const std::string &minimum,
                                const std::string &maximum,
                                bool shadow = true) {
  return BlockInput(
      BlocklyBlock(definitionId,
                   {{QStringLiteral("minimum"), ToQString(minimum)},
                    {QStringLiteral("maximum"), ToQString(maximum)}}),
      shadow);
}

QJsonObject ConfiguredVisualBlock(const std::string &definitionId,
                                  const OptionSettings &settings) {
  const blocks::VisualBlockDefinition *const definition =
      blocks::FindVisualBlock(definitionId);
  if (definition == nullptr)
    return {};

  QJsonObject fields;
  QJsonObject inputs;
  for (const blocks::VisualInputDefinition &input : definition->inputs) {
    if (input.defaultBlockId.empty())
      continue;
    if (input.nativeSettingKeys.size() == 2u) {
      const auto minimum = settings.find(input.nativeSettingKeys[0]);
      const auto maximum = settings.find(input.nativeSettingKeys[1]);
      if (minimum != settings.end() && maximum != settings.end()) {
        inputs.insert(
            ToQString(input.key),
            NumericRangeLiteral(input.defaultBlockId.c_str(), minimum->second,
                                maximum->second));
      }
      continue;
    }
    const auto value = settings.find(input.key);
    if (value != settings.end())
      inputs.insert(ToQString(input.key),
                    Literal(input.defaultBlockId.c_str(), value->second));
  }
  for (const blocks::VisualFieldDefinition &field : definition->fields) {
    const auto value = settings.find(field.key);
    if (value != settings.end())
      fields.insert(ToQString(field.key), ToQString(value->second));
  }
  return BlocklyBlock(definitionId, fields, inputs);
}

QJsonObject RangeInput(const std::string &from, const std::string &to) {
  QJsonObject inputs;
  inputs.insert(QStringLiteral("from"), Literal("values/milliseconds", from));
  inputs.insert(QStringLiteral("to"), Literal("values/milliseconds", to));
  return BlockInput(BlocklyBlock("time/range", {}, inputs));
}

QJsonObject AtInput(const std::string &time) {
  QJsonObject inputs;
  inputs.insert(QStringLiteral("time"), Literal("values/milliseconds", time));
  return BlockInput(BlocklyBlock("time/at", {}, inputs));
}

QJsonObject AllTimeInput() { return BlockInput(BlocklyBlock("time/all")); }

std::string Setting(const OptionSettings &settings, const char *key,
                    const char *fallback) {
  const auto found = settings.find(key);
  return found == settings.end() ? std::string(fallback) : found->second;
}

QJsonObject PointInput(const OptionSettings &settings, const char *xKey = "x",
                       const char *yKey = "y", const char *zKey = "z") {
  QJsonObject inputs;
  inputs.insert(QStringLiteral("x"),
                Literal("values/meters", Setting(settings, xKey, "0")));
  inputs.insert(QStringLiteral("y"),
                Literal("values/meters", Setting(settings, yKey, "0")));
  inputs.insert(QStringLiteral("z"),
                Literal("values/meters", Setting(settings, zKey, "0")));
  return BlockInput(BlocklyBlock("targets/point", {}, inputs));
}

QJsonObject DirectionInput(const OptionSettings &settings) {
  QJsonObject inputs;
  inputs.insert(QStringLiteral("x"),
                Literal("values/number", Setting(settings, "directionX", "1")));
  inputs.insert(QStringLiteral("y"),
                Literal("values/number", Setting(settings, "directionY", "0")));
  inputs.insert(QStringLiteral("z"),
                Literal("values/number", Setting(settings, "directionZ", "0")));
  return BlockInput(BlocklyBlock("targets/direction", {}, inputs));
}

QJsonObject RotationInput(const OptionSettings &settings) {
  QJsonObject inputs;
  inputs.insert(
      QStringLiteral("yaw"),
      Literal("values/degrees", Setting(settings, "yawDegrees", "0")));
  inputs.insert(
      QStringLiteral("pitch"),
      Literal("values/degrees", Setting(settings, "pitchDegrees", "0")));
  inputs.insert(
      QStringLiteral("roll"),
      Literal("values/degrees", Setting(settings, "rollDegrees", "0")));
  return BlockInput(BlocklyBlock("targets/rotation", {}, inputs));
}

QJsonObject SizeInput(const OptionSettings &settings) {
  QJsonObject inputs;
  inputs.insert(QStringLiteral("x"),
                Literal("values/meters", Setting(settings, "sizeX", "10")));
  inputs.insert(QStringLiteral("y"),
                Literal("values/meters", Setting(settings, "sizeY", "10")));
  inputs.insert(QStringLiteral("z"),
                Literal("values/meters", Setting(settings, "sizeZ", "10")));
  return BlockInput(BlocklyBlock("targets/size", {}, inputs));
}

QJsonObject BoxInput(const OptionSettings &settings) {
  QJsonObject inputs;
  inputs.insert(QStringLiteral("center"),
                PointInput(settings, "centerX", "centerY", "centerZ"));
  inputs.insert(QStringLiteral("size"), SizeInput(settings));
  return BlockInput(BlocklyBlock("targets/box", {}, inputs));
}

QJsonObject PrismInput(const OptionSettings &settings) {
  QJsonObject inputs;
  inputs.insert(QStringLiteral("origin"),
                PointInput(settings, "originX", "originY", "originZ"));
  inputs.insert(QStringLiteral("depth"),
                Literal("values/meters", Setting(settings, "depth", "5")));
  inputs.insert(QStringLiteral("polygon"),
                Literal("values/polygon",
                        Setting(settings, "polygon", "-5,-5;5,-5;0,5")));
  return BlockInput(BlocklyBlock(
      "targets/prism",
      {{QStringLiteral("plane"), ToQString(Setting(settings, "plane", "xz"))}},
      inputs));
}

QJsonObject ExtremumObjective(const char *definitionId, QJsonObject value,
                              QJsonObject range) {
  QJsonObject inputs;
  inputs.insert(QStringLiteral("value"), BlockInput(std::move(value)));
  inputs.insert(QStringLiteral("range"), std::move(range));
  return BlocklyBlock(definitionId, {}, inputs);
}

QJsonObject Chain(std::vector<QJsonObject> blocks) {
  if (blocks.empty())
    return {};
  for (std::size_t index = blocks.size(); index > 1; --index) {
    QJsonObject next;
    next.insert(QStringLiteral("block"), blocks[index - 1]);
    blocks[index - 2].insert(QStringLiteral("next"), next);
  }
  return blocks.front();
}

QJsonObject BlocklyNodeForProgram(const blocks::VisualProgram &program,
                                  blocks::VisualNodeId id, bool topLevel) {
  const blocks::VisualNode *const node = program.find(id);
  if (node == nullptr)
    return {};
  const blocks::VisualBlockDefinition *const definition =
      blocks::FindVisualBlock(node->definitionId);
  if (definition == nullptr)
    return {};

  QJsonObject fields;
  for (const auto &[key, storedValue] : node->fields) {
    QString value = ToQString(storedValue);
    const auto field =
        std::find_if(definition->fields.begin(), definition->fields.end(),
                     [&key](const blocks::VisualFieldDefinition &candidate) {
                       return candidate.key == key;
                     });
    if (field != definition->fields.end() &&
        field->kind == blocks::VisualFieldDefinition::Kind::Boolean) {
      value = value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
                  ? QStringLiteral("TRUE")
                  : QStringLiteral("FALSE");
    }
    fields.insert(ToQString(key), value);
  }

  QJsonObject inputs;
  for (const auto &[key, childId] : node->inputs) {
    const QJsonObject child = BlocklyNodeForProgram(program, childId, false);
    if (!child.isEmpty())
      inputs.insert(ToQString(key), BlockInput(child));
  }
  for (const auto &[key, children] : node->statements) {
    std::vector<QJsonObject> sequence;
    sequence.reserve(children.size());
    for (const blocks::VisualNodeId childId : children) {
      const QJsonObject child = BlocklyNodeForProgram(program, childId, false);
      if (!child.isEmpty())
        sequence.push_back(child);
    }
    const QJsonObject body = Chain(std::move(sequence));
    if (!body.isEmpty())
      inputs.insert(ToQString(key), BlockInput(body));
  }

  QJsonObject block = BlocklyBlock(node->definitionId, fields, inputs,
                                   topLevel ? node->x : 0.0,
                                   topLevel ? node->y : 0.0);
  block.insert(QStringLiteral("id"),
               QString::number(static_cast<qulonglong>(node->id)));
  return block;
}

QString WorkspaceForProgram(const blocks::VisualProgram &program) {
  QJsonArray top;
  for (const blocks::VisualNodeId id : program.topLevel) {
    const QJsonObject block = BlocklyNodeForProgram(program, id, true);
    if (!block.isEmpty())
      top.push_back(block);
  }
  QJsonObject blocks{{QStringLiteral("languageVersion"), 0},
                     {QStringLiteral("blocks"), top}};
  return QString::fromUtf8(
      QJsonDocument(QJsonObject{{QStringLiteral("blocks"), blocks}})
          .toJson(QJsonDocument::Compact));
}

QJsonObject ObjectiveForConfiguration(const OptionConfiguration &evaluation,
                                      QString *diagnostic) {
  const OptionSettings &settings = evaluation.settings;
  QJsonObject score;

  if (evaluation.id == kPointTargetEvaluationId) {
    QJsonObject distanceInputs;
    distanceInputs.insert(QStringLiteral("a"),
                          BlockInput(BlocklyBlock("simulation/car-position")));
    distanceInputs.insert(QStringLiteral("b"), PointInput(settings));
    score = ExtremumObjective(
        "objective/minimize", BlocklyBlock("math/distance", {}, distanceInputs),
        RangeInput(Setting(settings, "minTimeMs", "1000"),
                   Setting(settings, "maxTimeMs", "6000")));
  } else if (evaluation.id == kVelocityEvaluationId) {
    const bool projected = Setting(settings, "mode", "total") == "projected";
    const bool gated = Setting(settings, "alignmentEnabled", "false") == "true";
    QJsonObject value;
    if (projected) {
      QJsonObject dotInputs;
      dotInputs.insert(QStringLiteral("a"),
                       BlockInput(BlocklyBlock("simulation/car-velocity")));
      dotInputs.insert(QStringLiteral("b"), DirectionInput(settings));
      value = BlocklyBlock("math/dot", {}, dotInputs);
    } else {
      value = BlocklyBlock("simulation/car-speed");
    }
    score =
        ExtremumObjective("objective/maximize", std::move(value),
                          RangeInput(Setting(settings, "minTimeMs", "1000"),
                                     Setting(settings, "maxTimeMs", "6000")));

    if (gated) {
      QJsonObject normalizeInputs;
      normalizeInputs.insert(
          QStringLiteral("value"),
          BlockInput(BlocklyBlock("simulation/car-velocity")));
      QJsonObject alignmentDotInputs;
      alignmentDotInputs.insert(
          QStringLiteral("a"),
          BlockInput(BlocklyBlock("math/normalize", {}, normalizeInputs)));
      alignmentDotInputs.insert(QStringLiteral("b"), DirectionInput(settings));
      QJsonObject ratioInputs;
      ratioInputs.insert(
          QStringLiteral("value"),
          Literal("values/percent",
                  Setting(settings, "minAlignmentPercent", "-100")));
      QJsonObject compareInputs;
      compareInputs.insert(
          QStringLiteral("a"),
          BlockInput(BlocklyBlock("math/dot", {}, alignmentDotInputs)));
      compareInputs.insert(
          QStringLiteral("b"),
          BlockInput(BlocklyBlock("math/percent-ratio", {}, ratioInputs)));
      QJsonObject gateInputs;
      gateInputs.insert(QStringLiteral("score"), BlockInput(score));
      gateInputs.insert(QStringLiteral("condition"),
                        BlockInput(BlocklyBlock("conditions/greater-equal", {},
                                                compareInputs)));
      score = BlocklyBlock("objective/only-when", {}, gateInputs);
    }
  } else if (evaluation.id == kPoseTargetEvaluationId) {
    QJsonObject positionDistanceInputs;
    positionDistanceInputs.insert(
        QStringLiteral("a"),
        BlockInput(BlocklyBlock("simulation/car-position")));
    positionDistanceInputs.insert(QStringLiteral("b"), PointInput(settings));
    QJsonObject rotationDistanceInputs;
    rotationDistanceInputs.insert(
        QStringLiteral("a"),
        BlockInput(BlocklyBlock("simulation/car-rotation")));
    rotationDistanceInputs.insert(QStringLiteral("b"), RotationInput(settings));
    QJsonObject blendInputs;
    blendInputs.insert(
        QStringLiteral("a"),
        BlockInput(BlocklyBlock("math/distance", {}, positionDistanceInputs)));
    blendInputs.insert(QStringLiteral("b"),
                       BlockInput(BlocklyBlock("math/rotation-distance", {},
                                               rotationDistanceInputs)));
    blendInputs.insert(
        QStringLiteral("weight"),
        Literal("values/percent",
                Setting(settings, "rotationWeightPercent", "50")));
    score =
        ExtremumObjective("objective/minimize",
                          BlocklyBlock("math/weighted-blend", {}, blendInputs),
                          RangeInput(Setting(settings, "minTimeMs", "1000"),
                                     Setting(settings, "maxTimeMs", "6000")));
  } else if (evaluation.id == kStuntPointsEvaluationId) {
    score = ExtremumObjective(
        "objective/maximize", BlocklyBlock("simulation/stunt-points"),
        AtInput(Setting(settings, "targetTimeMs", "6000")));
  } else if (evaluation.id == kPreciseFinishTimeEvaluationId) {
    score = ExtremumObjective("objective/minimize",
                              BlocklyBlock("simulation/finish-time"),
                              AllTimeInput());
  } else if (evaluation.id == kVolumeEntryEvaluationId ||
             evaluation.id == kCustomVolumeEntryEvaluationId) {
    QJsonObject insideInputs;
    insideInputs.insert(QStringLiteral("position"),
                        BlockInput(BlocklyBlock("simulation/car-position")));
    insideInputs.insert(QStringLiteral("volume"),
                        evaluation.id == kVolumeEntryEvaluationId
                            ? BoxInput(settings)
                            : PrismInput(settings));
    QJsonObject firstInputs;
    firstInputs.insert(
        QStringLiteral("condition"),
        BlockInput(BlocklyBlock("conditions/inside", {}, insideInputs)));
    firstInputs.insert(QStringLiteral("range"), AllTimeInput());
    score = BlocklyBlock("objective/first-time", {}, firstInputs);
  } else {
    if (diagnostic != nullptr) {
      *diagnostic =
          QStringLiteral("The runtime objective type '%1' has no v3 visual "
                         "migration. It was not replaced with a different "
                         "objective.")
              .arg(QString::fromStdString(evaluation.id));
    }
    return BlocklyBlock("flow/set-objective");
  }

  QJsonObject chooseInputs;
  chooseInputs.insert(QStringLiteral("score"), BlockInput(std::move(score)));
  return BlocklyBlock("flow/set-objective", {}, chooseInputs);
}

QJsonObject
MutationWindowForExpansion(const blocks::ModifierExpansion &expansion) {
  const auto setting = [&expansion](const char *key, const char *fallback) {
    const auto found = expansion.window.find(key);
    return found == expansion.window.end() ? std::string(fallback)
                                           : found->second;
  };
  QJsonObject inputs;
  inputs.insert(
      QStringLiteral("range"),
      RangeInput(setting("minTimeMs", "1000"), setting("maxTimeMs", "5990")));
  inputs.insert(QStringLiteral("seed"),
                Literal("values/integer", setting("seed", "1179926867")));

  std::vector<QJsonObject> atoms;
  for (const blocks::AtomExpansion &atom : expansion.atoms) {
    const QJsonObject block = ConfiguredVisualBlock(atom.definitionId, atom.fields);
    if (!block.isEmpty())
      atoms.push_back(block);
  }
  const QJsonObject body = Chain(std::move(atoms));
  if (!body.isEmpty()) {
    inputs.insert(QStringLiteral("body"), BlockInput(body));
  }
  return BlocklyBlock("flow/mutation-window", {}, inputs);
}

QString
WorkspaceForComponents(const blocks::SearchComponentConfiguration &components,
                       const QString &simulationHorizonMs,
                       const QString &conditionScript,
                       QString *diagnostic) {
  std::vector<QJsonObject> iterationSteps;
  iterationSteps.reserve(components.modifiers.size() + 1);
  const std::string searchDefinition =
      "search/" + components.searchAlgorithm.id;
  QJsonObject searchPolicy =
      ConfiguredVisualBlock(searchDefinition, components.searchAlgorithm.settings);
  if (searchPolicy.isEmpty()) {
    if (diagnostic != nullptr) {
      *diagnostic =
          QStringLiteral("The runtime search policy '%1' has no v3 visual migration.")
              .arg(QString::fromStdString(components.searchAlgorithm.id));
    }
  }
  for (const OptionConfiguration &modifier : components.modifiers) {
    const blocks::ModifierExpansion expansion =
        blocks::ExpandModifierAtoms(modifier);
    if (!expansion.atoms.empty()) {
      iterationSteps.push_back(MutationWindowForExpansion(expansion));
    }
  }
  QJsonObject simulateInputs;
  simulateInputs.insert(
      QStringLiteral("until"),
      Literal("values/milliseconds", simulationHorizonMs.toStdString()));
  if (conditionScript.isEmpty()) {
    simulateInputs.insert(QStringLiteral("where"),
                          Literal("values/boolean", "true"));
  } else {
    simulateInputs.insert(
        QStringLiteral("where"),
        BlockInput(BlocklyBlock(
            "conditions/legacy-script",
            QJsonObject{{QStringLiteral("source"), conditionScript}})));
  }
  iterationSteps.push_back(
      BlocklyBlock("flow/simulate", {}, simulateInputs));
  iterationSteps.push_back(
      ObjectiveForConfiguration(components.evaluationTarget, diagnostic));

  if (!searchPolicy.isEmpty()) {
    const QJsonObject iterationBody = Chain(std::move(iterationSteps));
    if (!iterationBody.isEmpty()) {
      QJsonObject searchInputs =
          searchPolicy.value(QStringLiteral("inputs")).toObject();
      searchInputs.insert(QStringLiteral("body"), BlockInput(iterationBody));
      searchPolicy.insert(QStringLiteral("inputs"), searchInputs);
    }
  }

  QJsonObject startInputs;
  if (!searchPolicy.isEmpty()) {
    startInputs.insert(QStringLiteral("body"), BlockInput(searchPolicy));
  }
  QJsonObject start =
      BlocklyBlock("flow/when-start", {}, startInputs, 54.0, 48.0);
  QJsonArray top;
  top.push_back(start);
  QJsonObject blocks{{QStringLiteral("languageVersion"), 0},
                     {QStringLiteral("blocks"), top}};
  QJsonObject root{{QStringLiteral("blocks"), blocks}};
  return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString JsonScalarToString(const QJsonValue &value) {
  if (value.isString())
    return value.toString();
  if (value.isBool())
    return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
  if (value.isDouble())
    return QString::number(value.toDouble(), 'g', 17);
  return {};
}

} // namespace

BlockEditorBridge::BlockEditorBridge(SearchController *controller,
                                     QObject *parent)
    : QObject(parent), controller_(controller),
      catalogJson_(BuildCatalogJson()) {
  Q_ASSERT(controller_ != nullptr);
  connect(controller_, &SearchController::runningChanged, this,
          &BlockEditorBridge::editableChanged);
  connect(controller_, &SearchController::darkModeChanged, this,
          &BlockEditorBridge::darkModeChanged);
  connect(controller_, &SearchController::blockStructureChanged, this,
          [this]() {
            if (!applyingWorkspace_)
              synchronizeFromController();
          });
  connect(controller_, &SearchController::simulationHorizonMsChanged, this,
          [this]() {
            if (!applyingWorkspace_)
              synchronizeFromController();
          });
  connect(controller_, &SearchController::conditionScriptChanged, this,
          [this]() {
            if (!applyingWorkspace_)
              synchronizeFromController();
          });

  QSettings settings;
  const auto base = controller_->blockComponents();
  const QString storedProgram =
      settings.value(QString::fromLatin1(kStoredProgramKey)).toString();
  if (!storedProgram.isEmpty() && base) {
    const VisualProgramJson persisted = ParseVisualProgramJson(storedProgram);
    if (!persisted.program) {
      settings.setValue(QString::fromLatin1(kCorruptProgramBackupKey),
                        storedProgram);
    } else {
      const blocks::CompileResult compiled =
          blocks::CompileVisualProgram(*persisted.program, *base);
      if (compiled.ok) {
        applyingWorkspace_ = true;
        const bool applied = controller_->applyBlockComponents(
            compiled.configuration, compiled.conditionProgram);
        if (applied && compiled.simulationHorizonMs) {
          controller_->setSimulationHorizonMs(
              ToQString(*compiled.simulationHorizonMs));
        }
        applyingWorkspace_ = false;
        if (applied) {
          const QString storedWorkspace =
              settings.value(QString::fromLatin1(kStoredWorkspaceKey)).toString();
          if (!storedWorkspace.isEmpty()) {
            const ParsedWorkspace cached = ParseBlocklyWorkspace(storedWorkspace);
            if (cached.error.isEmpty() &&
                PrintVisualProgramJson(cached.program) ==
                    PrintVisualProgramJson(*persisted.program)) {
              workspaceJson_ = storedWorkspace;
            }
          }
          if (workspaceJson_.isEmpty())
            workspaceJson_ = WorkspaceForProgram(*persisted.program);
        }
      }
    }
  }

  // One-time transitional migration from the raw Blockly cache. Once parsed,
  // semantic v3 JSON is written separately and becomes authoritative.
  if (workspaceJson_.isEmpty() && base) {
    const QString storedWorkspace =
        settings.value(QString::fromLatin1(kStoredWorkspaceKey)).toString();
    if (!storedWorkspace.isEmpty()) {
      const ParsedWorkspace parsed = ParseBlocklyWorkspace(storedWorkspace);
      if (parsed.error.isEmpty()) {
        const blocks::CompileResult compiled =
            blocks::CompileVisualProgram(parsed.program, *base);
        if (compiled.ok && compiled.configuration == *base) {
          workspaceJson_ = storedWorkspace;
          settings.setValue(QString::fromLatin1(kStoredProgramKey),
                            PrintVisualProgramJson(parsed.program));
        }
      }
    }
  }
  if (workspaceJson_.isEmpty())
    synchronizeFromController();
}

bool BlockEditorBridge::editable() const {
  return controller_ != nullptr && !controller_->running();
}

bool BlockEditorBridge::darkMode() const {
  return controller_ != nullptr && controller_->darkMode();
}

QString BlockEditorBridge::BuildCatalogJson() {
  QJsonArray categories;
  for (const blocks::VisualCategory &category : blocks::VisualCategories()) {
    categories.push_back(
        QJsonObject{{QStringLiteral("id"), ToQString(category.id)},
                    {QStringLiteral("label"), ToQString(category.label)},
                    {QStringLiteral("color"), ToQString(category.color)}});
  }

  QJsonArray definitions;
  for (const blocks::VisualBlockDefinition &definition :
       blocks::VisualBlockCatalog()) {
    QJsonArray inputs;
    for (const blocks::VisualInputDefinition &input : definition.inputs) {
      inputs.push_back(QJsonObject{
          {QStringLiteral("key"), ToQString(input.key)},
          {QStringLiteral("label"), ToQString(input.label)},
          {QStringLiteral("checks"), ChecksForInput(input.type)},
          {QStringLiteral("defaultBlock"), ToQString(input.defaultBlockId)},
          {QStringLiteral("defaultValue"), ToQString(input.defaultValue)}});
    }
    QJsonArray fields;
    for (const blocks::VisualFieldDefinition &field : definition.fields) {
      QJsonArray choices;
      for (const auto &[value, label] : field.enumValues) {
        choices.push_back(QJsonArray{ToQString(label), ToQString(value)});
      }
      fields.push_back(QJsonObject{
          {QStringLiteral("key"), ToQString(field.key)},
          {QStringLiteral("label"), ToQString(field.label)},
          {QStringLiteral("kind"), FieldKindName(field.kind)},
          {QStringLiteral("defaultValue"), ToQString(field.defaultValue)},
          {QStringLiteral("choices"), choices}});
    }
    QJsonArray statements;
    for (const blocks::VisualStatementDefinition &statement :
         definition.statements) {
      statements.push_back(
          QJsonObject{{QStringLiteral("key"), ToQString(statement.key)},
                      {QStringLiteral("label"), ToQString(statement.label)},
                      {QStringLiteral("checks"),
                       StatementChecks(statement.family)}});
    }
    definitions.push_back(QJsonObject{
        {QStringLiteral("id"), ToQString(definition.id)},
        {QStringLiteral("type"), ToQString(definition.blocklyType)},
        {QStringLiteral("category"), ToQString(definition.categoryId)},
        {QStringLiteral("label"), ToQString(definition.label)},
        {QStringLiteral("shape"), ShapeName(definition.shape)},
        {QStringLiteral("toolboxVisible"), definition.toolboxVisible},
        {QStringLiteral("inputsInline"), definition.inputsInline},
        {QStringLiteral("outputChecks"),
         ChecksForOutput(definition.outputType)},
        {QStringLiteral("statementChecks"),
         StatementChecks(definition.statementFamily)},
        {QStringLiteral("viewerPicker"), ToQString(definition.viewerPicker)},
        {QStringLiteral("inputs"), inputs},
        {QStringLiteral("fields"), fields},
        {QStringLiteral("statements"), statements}});
  }
  return QString::fromUtf8(
      QJsonDocument(QJsonObject{{QStringLiteral("version"), 3},
                                {QStringLiteral("categories"), categories},
                                {QStringLiteral("blocks"), definitions}})
          .toJson(QJsonDocument::Compact));
}

BlockEditorBridge::ParsedWorkspace
BlockEditorBridge::ParseBlocklyWorkspace(const QString &json) {
  ParsedWorkspace result;
  if (json.size() > 4 * 1024 * 1024) {
    result.error = QStringLiteral("Block workspace exceeds the 4 MiB limit.");
    return result;
  }
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(json.toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    result.error = QStringLiteral("Block workspace JSON is invalid: %1")
                       .arg(parseError.errorString());
    return result;
  }
  const QJsonArray top = document.object()
                             .value(QStringLiteral("blocks"))
                             .toObject()
                             .value(QStringLiteral("blocks"))
                             .toArray();
  if (top.size() > 4096) {
    result.error =
        QStringLiteral("Block workspace contains too many top-level blocks.");
    return result;
  }

  std::map<QString, blocks::VisualNodeId> ids;
  std::set<blocks::VisualNodeId> usedIds;
  blocks::VisualNodeId nextId = 1;
  auto allocateId = [&usedIds, &nextId]() {
    while (usedIds.count(nextId) != 0)
      ++nextId;
    const blocks::VisualNodeId value = nextId++;
    usedIds.insert(value);
    return value;
  };
  auto idFor = [&ids, &usedIds, &allocateId](const QString &blocklyId) {
    if (!blocklyId.isEmpty()) {
      const auto found = ids.find(blocklyId);
      if (found != ids.end())
        return found->second;
      bool numeric = false;
      const qulonglong parsed = blocklyId.toULongLong(&numeric, 10);
      const blocks::VisualNodeId value =
          numeric && parsed != 0 &&
                  usedIds.count(static_cast<blocks::VisualNodeId>(parsed)) == 0
              ? static_cast<blocks::VisualNodeId>(parsed)
              : allocateId();
      usedIds.insert(value);
      ids.emplace(blocklyId, value);
      return value;
    }
    return allocateId();
  };

  std::function<std::optional<blocks::VisualNodeId>(const QJsonObject &,
                                                    std::size_t, bool)>
      parseBlock;
  parseBlock = [&](const QJsonObject &object, std::size_t depth,
                   bool statementChild) -> std::optional<blocks::VisualNodeId> {
    if (depth > 128) {
      result.error = QStringLiteral("Block workspace is nested too deeply.");
      return std::nullopt;
    }
    const QString type = object.value(QStringLiteral("type")).toString();
    const blocks::VisualBlockDefinition *const definition =
        blocks::FindVisualBlockByBlocklyType(type.toStdString());
    if (definition == nullptr) {
      result.error = QStringLiteral("Unknown block type '%1'.").arg(type);
      return std::nullopt;
    }
    const blocks::VisualNodeId id =
        idFor(object.value(QStringLiteral("id")).toString());
    if (result.program.nodes.count(id) != 0)
      return id;
    if (result.program.nodes.size() >= 4096) {
      result.error =
          QStringLiteral("Block workspace exceeds the 4096-node limit.");
      return std::nullopt;
    }
    blocks::VisualNode node;
    node.id = id;
    node.definitionId = definition->id;
    node.x = object.value(QStringLiteral("x")).toDouble();
    node.y = object.value(QStringLiteral("y")).toDouble();
    const QJsonObject fieldValues =
        object.value(QStringLiteral("fields")).toObject();
    for (auto iterator = fieldValues.constBegin();
         iterator != fieldValues.constEnd(); ++iterator) {
      const bool known =
          std::any_of(definition->fields.begin(), definition->fields.end(),
                      [&iterator](const blocks::VisualFieldDefinition &field) {
                        return ToQString(field.key) == iterator.key();
                      });
      if (!known) {
        result.error = QStringLiteral("Block '%1' contains unknown field '%2'.")
                           .arg(type, iterator.key());
        return std::nullopt;
      }
    }
    for (const blocks::VisualFieldDefinition &field : definition->fields) {
      QString value =
          JsonScalarToString(fieldValues.value(ToQString(field.key)));
      if (value.isEmpty())
        value = ToQString(field.defaultValue);
      if (field.kind == blocks::VisualFieldDefinition::Kind::Boolean) {
        if (value.compare(QStringLiteral("TRUE"), Qt::CaseInsensitive) == 0)
          value = QStringLiteral("true");
        else if (value.compare(QStringLiteral("FALSE"), Qt::CaseInsensitive) ==
                 0)
          value = QStringLiteral("false");
      }
      node.fields.emplace(field.key, value.toStdString());
    }
    result.program.nodes.emplace(id, std::move(node));
    blocks::VisualNode *const stored = result.program.find(id);

    const QJsonObject serializedInputs =
        object.value(QStringLiteral("inputs")).toObject();
    for (auto iterator = serializedInputs.constBegin();
         iterator != serializedInputs.constEnd(); ++iterator) {
      const bool valueInput =
          std::any_of(definition->inputs.begin(), definition->inputs.end(),
                      [&iterator](const blocks::VisualInputDefinition &input) {
                        return ToQString(input.key) == iterator.key();
                      });
      const bool statementInput = std::any_of(
          definition->statements.begin(), definition->statements.end(),
          [&iterator](const blocks::VisualStatementDefinition &statement) {
            return ToQString(statement.key) == iterator.key();
          });
      if (!valueInput && !statementInput) {
        result.error = QStringLiteral("Block '%1' contains unknown input '%2'.")
                           .arg(type, iterator.key());
        return std::nullopt;
      }
    }
    for (const blocks::VisualInputDefinition &input : definition->inputs) {
      const QJsonObject socket =
          serializedInputs.value(ToQString(input.key)).toObject();
      QJsonObject child = socket.value(QStringLiteral("block")).toObject();
      if (child.isEmpty()) {
        child = socket.value(QStringLiteral("shadow")).toObject();
      }
      if (child.isEmpty())
        continue;
      const auto childId = parseBlock(child, depth + 1, false);
      if (!childId)
        return std::nullopt;
      stored->inputs.emplace(input.key, *childId);
    }
    for (const blocks::VisualStatementDefinition &statement :
         definition->statements) {
      QJsonObject child = serializedInputs.value(ToQString(statement.key))
                              .toObject()
                              .value(QStringLiteral("block"))
                              .toObject();
      auto &sequence = stored->statements[statement.key];
      while (!child.isEmpty()) {
        const auto childId = parseBlock(child, depth + 1, true);
        if (!childId)
          return std::nullopt;
        sequence.push_back(*childId);
        child = child.value(QStringLiteral("next"))
                    .toObject()
                    .value(QStringLiteral("block"))
                    .toObject();
      }
    }
    static_cast<void>(statementChild);
    return id;
  };

  for (const QJsonValue &value : top) {
    if (!value.isObject())
      continue;
    QJsonObject current = value.toObject();
    while (!current.isEmpty()) {
      const auto id = parseBlock(current, 1, false);
      if (!id)
        return result;
      result.program.topLevel.push_back(*id);
      current = current.value(QStringLiteral("next"))
                    .toObject()
                    .value(QStringLiteral("block"))
                    .toObject();
    }
  }
  const blocks::VisualProgramValidation validation =
      blocks::ValidateVisualProgram(result.program);
  if (!validation.ok) {
    QStringList messages;
    for (const std::string &error : validation.errors) {
      messages.push_back(ToQString(error));
    }
    result.error = messages.join(QLatin1Char('\n'));
  }
  return result;
}

bool BlockEditorBridge::applyWorkspace(const QString &workspaceJson,
                                       qulonglong revision) {
  if (!editable()) {
    publishDiagnostics(
        {QStringLiteral("Stop the search before editing blocks.")});
    return false;
  }
  if (revision <= lastAcceptedRevision_)
    return false;
  const ParsedWorkspace parsed = ParseBlocklyWorkspace(workspaceJson);
  if (!parsed.error.isEmpty()) {
    publishDiagnostics(parsed.error.split(QLatin1Char('\n')));
    return false;
  }
  const auto base = controller_->blockComponents();
  if (!base) {
    publishDiagnostics({QStringLiteral(
        "The current runtime block configuration cannot be compiled.")});
    return false;
  }
  const blocks::CompileResult compiled =
      blocks::CompileVisualProgram(parsed.program, *base);
  if (!compiled.ok) {
    QStringList messages;
    for (const std::string &error : compiled.errors) {
      messages.push_back(ToQString(error));
    }
    publishDiagnostics(messages);
    return false;
  }
  applyingWorkspace_ = true;
  const bool applied =
      controller_->applyBlockComponents(compiled.configuration,
                                        compiled.conditionProgram);
  if (applied && compiled.simulationHorizonMs) {
    controller_->setSimulationHorizonMs(
        ToQString(*compiled.simulationHorizonMs));
  }
  applyingWorkspace_ = false;
  if (!applied) {
    publishDiagnostics({QStringLiteral(
        "The native runtime rejected the compiled block configuration.")});
    return false;
  }

  lastAcceptedRevision_ = revision;
  workspaceJson_ = workspaceJson;
  QSettings settings;
  settings.setValue(QString::fromLatin1(kStoredProgramKey),
                    PrintVisualProgramJson(parsed.program));
  settings.setValue(QString::fromLatin1(kStoredWorkspaceKey), workspaceJson_);
  publishDiagnostics({}, false);
  // Blockly already owns this exact state. Echoing it back through the
  // property would clear selection, viewport and undo history on every
  // accepted edit.
  return true;
}

void BlockEditorBridge::editorReady() {
  if (editorReady_)
    return;
  editorReady_ = true;
  emit editorReadyChanged();
}

void BlockEditorBridge::requestNativeWorkspace() {
  emit workspaceJsonChanged();
}

QString BlockEditorBridge::selectedViewerTargetJson(const QString &kind) const {
  if (controller_ == nullptr) return QStringLiteral("{}");
  QJsonObject result;
  QVariantMap target;
  if (kind == QStringLiteral("box")) {
    target = controller_->cuboidTargets()->selectedTarget();
    for (const char *key : {"id", "name", "centerX", "centerY", "centerZ",
                            "sizeX", "sizeY", "sizeZ"}) {
      CopyTargetValue(&result, target, key);
    }
  } else if (kind == QStringLiteral("prism")) {
    target = controller_->customVolumeTargets()->selectedTarget();
    for (const char *key : {"id", "name", "plane", "originX", "originY",
                            "originZ", "depth", "polygon"}) {
      CopyTargetValue(&result, target, key);
    }
  } else if (kind == QStringLiteral("rotation")) {
    target = controller_->poseTargets()->selectedTarget();
    for (const char *key : {"id", "name", "yawDegrees", "pitchDegrees",
                            "rollDegrees"}) {
      CopyTargetValue(&result, target, key);
    }
  } else {
    return QStringLiteral("{}");
  }
  if (target.isEmpty()) return QStringLiteral("{}");
  result.insert(QStringLiteral("kind"), kind);
  return JsonText(result);
}

void BlockEditorBridge::requestViewerPointPick(const QString &blockId) {
  if (!editable() || blockId.isEmpty()) return;
  pendingViewerPointBlockId_ = blockId;
  emit viewerPointPickRequested(blockId);
}

void BlockEditorBridge::completeViewerPointPick(const QString &blockId,
                                                double x,
                                                double y,
                                                double z) {
  if (blockId.isEmpty() || blockId != pendingViewerPointBlockId_ ||
      !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
    return;
  }
  pendingViewerPointBlockId_.clear();
  emit viewerPointPicked(blockId, x, y, z);
}

QString
BlockEditorBridge::BuildWorkspaceFromController(QString *diagnostic) const {
  const auto components = controller_->blockComponents();
  if (!components) {
    if (diagnostic != nullptr) {
      *diagnostic =
          QStringLiteral("The current runtime block program cannot be "
                         "represented because it does not compile.");
    }
    return QStringLiteral(R"({"blocks":{"languageVersion":0,"blocks":[]}})");
  }
  return WorkspaceForComponents(*components, controller_->simulationHorizonMs(),
                                controller_->conditionScript(), diagnostic);
}

void BlockEditorBridge::publishDiagnostics(const QStringList &messages,
                                           bool isError) {
  QJsonArray array;
  for (const QString &message : messages) {
    array.push_back(QJsonObject{
        {QStringLiteral("message"), message},
        {QStringLiteral("severity"),
         isError ? QStringLiteral("error") : QStringLiteral("info")}});
  }
  const QString next =
      QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
  if (next == diagnosticsJson_)
    return;
  diagnosticsJson_ = next;
  emit diagnosticsJsonChanged();
}

void BlockEditorBridge::synchronizeFromController() {
  QString diagnostic;
  const QString next = BuildWorkspaceFromController(&diagnostic);
  const ParsedWorkspace parsed = ParseBlocklyWorkspace(next);
  if (parsed.error.isEmpty()) {
    QSettings().setValue(QString::fromLatin1(kStoredProgramKey),
                         PrintVisualProgramJson(parsed.program));
  }
  if (next != workspaceJson_) {
    workspaceJson_ = next;
    ++lastAcceptedRevision_;
    emit workspaceRevisionChanged();
    emit workspaceJsonChanged();
  }
  if (diagnostic.isEmpty())
    publishDiagnostics({}, false);
  else
    publishDiagnostics({diagnostic}, false);
}

} // namespace forevertas::app
