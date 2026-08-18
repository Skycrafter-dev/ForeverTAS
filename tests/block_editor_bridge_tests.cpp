#include "app/block_editor_bridge.h"
#include "app/search_controller.h"
#include "app/visual_program_json.h"
#include "blocks/block_compiler.h"
#include "blocks/visual_compiler.h"
#include "evaluators/custom_volume_entry_evaluator.h"
#include "evaluators/point_target_evaluator.h"
#include "evaluators/pose_target_evaluator.h"
#include "evaluators/precise_finish_time_evaluator.h"
#include "evaluators/stunt_points_evaluator.h"
#include "evaluators/velocity_evaluator.h"
#include "evaluators/visual_expression_evaluator.h"
#include "evaluators/volume_entry_evaluator.h"
#include "searches/algorithm_registry.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include <initializer_list>
#include <iostream>
#include <utility>
#include <vector>

namespace {

using namespace forevertas;
using namespace forevertas::app;
using namespace forevertas::blocks;

bool Check(bool condition, const std::string &message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

QJsonObject CatalogBlock(const BlockEditorBridge &bridge,
                         const QString &definitionId) {
  const QJsonDocument document =
      QJsonDocument::fromJson(bridge.catalogJson().toUtf8());
  for (const QJsonValue &value :
       document.object().value(QStringLiteral("blocks")).toArray()) {
    const QJsonObject block = value.toObject();
    if (block.value(QStringLiteral("id")).toString() == definitionId)
      return block;
  }
  return {};
}

bool TestCatalogPublishesStatementFamilies(const BlockEditorBridge &bridge) {
  const QJsonObject start =
      CatalogBlock(bridge, QStringLiteral("flow/when-start"));
  const QJsonObject window =
      CatalogBlock(bridge, QStringLiteral("flow/mutation-window"));
  const QJsonObject mutation =
      CatalogBlock(bridge, QStringLiteral("mutate/reroll-steering"));
  const QJsonObject search =
      CatalogBlock(bridge, QStringLiteral("search/basic-brute-force"));
  const QJsonObject choose =
      CatalogBlock(bridge, QStringLiteral("flow/set-objective"));
  const QJsonObject simulate =
      CatalogBlock(bridge, QStringLiteral("flow/simulate"));
  const QJsonObject maximize =
      CatalogBlock(bridge, QStringLiteral("objective/maximize"));
  const QJsonObject timeRange =
      CatalogBlock(bridge, QStringLiteral("time/range"));
  const QJsonObject legacyCondition =
      CatalogBlock(bridge, QStringLiteral("conditions/legacy-script"));
  bool okay = Check(!start.isEmpty() && !window.isEmpty() &&
                        !mutation.isEmpty() && !search.isEmpty() &&
                        !choose.isEmpty() && !simulate.isEmpty() &&
                        !maximize.isEmpty() && !timeRange.isEmpty() &&
                        !legacyCondition.isEmpty(),
                    "statement-family catalog fixtures are missing");
  if (!okay) return false;

  const QJsonArray rootStatements =
      start.value(QStringLiteral("statements")).toArray();
  const QJsonArray mutationStatements =
      window.value(QStringLiteral("statements")).toArray();
  const QJsonArray iterationStatements =
      search.value(QStringLiteral("statements")).toArray();
  okay &= Check(rootStatements.size() == 1 &&
                    rootStatements.at(0).toObject()
                            .value(QStringLiteral("checks"))
                            .toArray() ==
                        QJsonArray{QStringLiteral("root-command")},
                "root Blockly statement socket is not typed");
  okay &= Check(mutationStatements.size() == 1 &&
                    mutationStatements.at(0).toObject()
                            .value(QStringLiteral("checks"))
                            .toArray() ==
                        QJsonArray{QStringLiteral("mutation-command")},
                "mutation Blockly statement socket is not typed");
  okay &= Check(mutation.value(QStringLiteral("statementChecks")).toArray() ==
                    QJsonArray{QStringLiteral("mutation-command")},
                "mutation command does not publish its connection family");
  okay &= Check(search.value(QStringLiteral("statementChecks")).toArray() ==
                    QJsonArray{QStringLiteral("root-command")},
                "search command does not publish its connection family");
  okay &= Check(iterationStatements.size() == 1 &&
                    iterationStatements.at(0).toObject()
                            .value(QStringLiteral("checks"))
                            .toArray() ==
                        QJsonArray{QStringLiteral("iteration-command")},
                "bruteforce iteration statement socket is not typed");
  okay &= Check(window.value(QStringLiteral("statementChecks")).toArray() ==
                    QJsonArray{QStringLiteral("iteration-command")} &&
                    choose.value(QStringLiteral("statementChecks")).toArray() ==
                        QJsonArray{QStringLiteral("iteration-command")} &&
                    simulate.value(QStringLiteral("statementChecks")).toArray() ==
                        QJsonArray{QStringLiteral("iteration-command")},
                "iteration steps can escape the bruteforce body");
  okay &= Check(!legacyCondition.value(QStringLiteral("toolboxVisible")).toBool(true),
                "legacy condition compatibility block leaked into the toolbox");
  okay &= Check(!search.value(QStringLiteral("inputsInline")).toBool(true) &&
                    !window.value(QStringLiteral("inputsInline")).toBool(true) &&
                    !simulate.value(QStringLiteral("inputsInline")).toBool(true) &&
                    !maximize.value(QStringLiteral("inputsInline")).toBool(true) &&
                    choose.value(QStringLiteral("inputsInline")).toBool(false),
                "compact process/objective row layout is not published by catalog");
  okay &= Check(timeRange.value(QStringLiteral("label")).toString().isEmpty(),
                "time range still carries a redundant width-expanding header");
  return okay;
}

bool TestBridgeFollowsDarkMode(SearchController *controller,
                               BlockEditorBridge *bridge) {
  const bool original = controller->darkMode();
  bool notified = false;
  const QMetaObject::Connection connection = QObject::connect(
      bridge, &BlockEditorBridge::darkModeChanged, [&notified]() {
        notified = true;
      });
  controller->setDarkMode(!original);
  QCoreApplication::processEvents();
  bool okay = Check(bridge->darkMode() == !original,
                    "Blockly bridge did not follow the application theme");
  okay &= Check(notified,
                "Blockly bridge did not publish a theme change to WebChannel");
  QObject::disconnect(connection);
  controller->setDarkMode(original);
  QCoreApplication::processEvents();
  okay &= Check(bridge->darkMode() == original,
                "Blockly bridge did not restore the application theme");
  return okay;
}

bool TestLegacyConditionMigratesInsideSimulation() {
  QSettings().clear();
  SearchController controller;
  controller.setConditionScript(QStringLiteral("kmh(car.speed) >= 200"));
  BlockEditorBridge bridge(&controller);
  bool okay = Check(
      bridge.workspaceJson().contains(QStringLiteral("ft_conditions_legacy_script")),
      "legacy condition script was not migrated into the simulate predicate");
  okay &= Check(bridge.applyWorkspace(bridge.workspaceJson(),
                                      bridge.workspaceRevision() + 1),
                "migrated legacy condition workspace did not compile");
  const QString semantic =
      QSettings().value(QStringLiteral("blockEditor/v3Program")).toString();
  okay &= Check(semantic.contains(QStringLiteral("conditions/legacy-script")) &&
                    semantic.contains(QStringLiteral("kmh(car.speed) >= 200")),
                "legacy condition migration was not preserved semantically");
  return okay;
}

bool TestCatalogPublishesViewerPickers(const BlockEditorBridge &bridge) {
  bool okay = true;
  for (const auto &[definitionId, picker] :
       std::initializer_list<std::pair<const char *, const char *>>{
           {"targets/point", "point"},
           {"targets/rotation", "rotation"},
           {"targets/box", "box"},
           {"targets/prism", "prism"}}) {
    const QJsonObject block =
        CatalogBlock(bridge, QString::fromLatin1(definitionId));
    okay &= Check(!block.isEmpty() &&
                       block.value(QStringLiteral("viewerPicker")).toString() ==
                           QString::fromLatin1(picker),
                   std::string("viewer picker metadata missing for ") +
                       definitionId);
  }
  return okay;
}

QJsonObject ParsedObject(const QString &json) {
  const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
  return document.isObject() ? document.object() : QJsonObject{};
}

bool TestViewerTargetBridge(SearchController *controller,
                            BlockEditorBridge *bridge) {
  bool okay = true;
  auto *const cuboids = controller->cuboidTargets();
  if (cuboids->count() == 0) cuboids->addTarget(0.0, 0.0, 0.0);
  cuboids->selectTarget(0);
  okay &= cuboids->setCenterComponent(0, QStringLiteral("x"),
                                     QStringLiteral("12.5"));
  okay &= cuboids->setCenterComponent(0, QStringLiteral("y"),
                                     QStringLiteral("-3"));
  okay &= cuboids->setSizeComponent(0, QStringLiteral("z"),
                                   QStringLiteral("7.25"));
  const QJsonObject box =
      ParsedObject(bridge->selectedViewerTargetJson(QStringLiteral("box")));
  okay &= Check(box.value(QStringLiteral("kind")).toString() ==
                        QStringLiteral("box") &&
                    box.value(QStringLiteral("centerX")).toString() ==
                        QStringLiteral("12.5") &&
                    box.value(QStringLiteral("centerY")).toString() ==
                        QStringLiteral("-3") &&
                    box.value(QStringLiteral("sizeZ")).toString() ==
                        QStringLiteral("7.25"),
                "selected cuboid did not cross the Blockly bridge exactly");

  auto *const poses = controller->poseTargets();
  if (poses->count() == 0)
    poses->addTarget(0.0, 0.0, 0.0, QQuaternion());
  poses->selectTarget(0);
  okay &= poses->setRotationComponent(0, QStringLiteral("yaw"),
                                      QStringLiteral("47.5"));
  okay &= poses->setRotationComponent(0, QStringLiteral("roll"),
                                      QStringLiteral("-12"));
  const QJsonObject rotation = ParsedObject(
      bridge->selectedViewerTargetJson(QStringLiteral("rotation")));
  okay &= Check(rotation.value(QStringLiteral("kind")).toString() ==
                        QStringLiteral("rotation") &&
                    rotation.value(QStringLiteral("yawDegrees")).toString() ==
                        QStringLiteral("47.5") &&
                    rotation.value(QStringLiteral("rollDegrees")).toString() ==
                        QStringLiteral("-12"),
                "selected pose rotation did not cross the Blockly bridge");

  auto *const volumes = controller->customVolumeTargets();
  if (volumes->count() == 0) volumes->addTarget(QStringLiteral("xz"), 0, 0, 0);
  volumes->selectTarget(0);
  okay &= volumes->setPlane(0, QStringLiteral("xy"));
  okay &= volumes->setOriginComponent(0, QStringLiteral("z"),
                                     QStringLiteral("22"));
  okay &= volumes->setDepth(0, QStringLiteral("9"));
  const QJsonObject prism =
      ParsedObject(bridge->selectedViewerTargetJson(QStringLiteral("prism")));
  okay &= Check(prism.value(QStringLiteral("kind")).toString() ==
                        QStringLiteral("prism") &&
                    prism.value(QStringLiteral("plane")).toString() ==
                        QStringLiteral("xy") &&
                    prism.value(QStringLiteral("originZ")).toString() ==
                        QStringLiteral("22") &&
                    prism.value(QStringLiteral("depth")).toString() ==
                        QStringLiteral("9") &&
                    !prism.value(QStringLiteral("polygon")).toString().isEmpty(),
                "selected prism did not cross the Blockly bridge");

  QString requested;
  QString pickedBlock;
  QVector3D pickedPoint;
  QObject::connect(bridge, &BlockEditorBridge::viewerPointPickRequested,
                   [&](const QString &blockId) { requested = blockId; });
  QObject::connect(
      bridge, &BlockEditorBridge::viewerPointPicked,
      [&](const QString &blockId, double x, double y, double z) {
        pickedBlock = blockId;
        pickedPoint = QVector3D(static_cast<float>(x), static_cast<float>(y),
                                static_cast<float>(z));
      });
  bridge->requestViewerPointPick(QStringLiteral("block-42"));
  okay &= Check(requested == QStringLiteral("block-42"),
                "point pick request did not reach the native viewer");
  bridge->completeViewerPointPick(QStringLiteral("wrong-block"), 1, 2, 3);
  okay &= Check(pickedBlock.isEmpty(),
                "mismatched point-pick completion was accepted");
  bridge->completeViewerPointPick(QStringLiteral("block-42"), 1.25, -2.5, 3.75);
  okay &= Check(pickedBlock == QStringLiteral("block-42") &&
                    qFuzzyCompare(pickedPoint.x(), 1.25f) &&
                    qFuzzyCompare(pickedPoint.y(), -2.5f) &&
                    qFuzzyCompare(pickedPoint.z(), 3.75f),
                "point pick completion did not return to Blockly");
  return okay;
}

std::vector<OptionConfiguration> Evaluations() {
  std::vector<OptionConfiguration> values;

  OptionSettings total = DefaultVelocityOptionSettings();
  total["minTimeMs"] = "1500";
  total["maxTimeMs"] = "5000";
  values.push_back({kVelocityEvaluationId, total});

  OptionSettings projected = DefaultVelocityOptionSettings();
  projected["minTimeMs"] = "2000";
  projected["maxTimeMs"] = "5100";
  projected["mode"] = "projected";
  projected["alignmentEnabled"] = "true";
  projected["directionX"] = "2";
  projected["directionY"] = "0.5";
  projected["directionZ"] = "-3";
  projected["minAlignmentPercent"] = "35";
  values.push_back({kVelocityEvaluationId, projected});

  OptionSettings point = DefaultPointTargetOptionSettings();
  point["minTimeMs"] = "1200";
  point["maxTimeMs"] = "5700";
  point["x"] = "12.5";
  point["y"] = "4";
  point["z"] = "-8.25";
  values.push_back({kPointTargetEvaluationId, point});

  OptionSettings pose = DefaultPoseTargetOptionSettings();
  pose["minTimeMs"] = "1800";
  pose["maxTimeMs"] = "5900";
  pose["x"] = "3";
  pose["y"] = "4";
  pose["z"] = "5";
  pose["yawDegrees"] = "30";
  pose["pitchDegrees"] = "-10";
  pose["rollDegrees"] = "170";
  pose["rotationWeightPercent"] = "62";
  values.push_back({kPoseTargetEvaluationId, pose});

  OptionSettings stunt = DefaultStuntPointsOptionSettings();
  stunt["targetTimeMs"] = "5400";
  values.push_back({kStuntPointsEvaluationId, stunt});

  values.push_back({kPreciseFinishTimeEvaluationId,
                    DefaultPreciseFinishTimeOptionSettings()});

  OptionSettings box = DefaultVolumeEntryOptionSettings();
  box["centerX"] = "20";
  box["centerY"] = "1.5";
  box["centerZ"] = "-7";
  box["sizeX"] = "4";
  box["sizeY"] = "5";
  box["sizeZ"] = "6";
  values.push_back({kVolumeEntryEvaluationId, box});

  OptionSettings prism = DefaultCustomVolumeEntryOptionSettings();
  prism["plane"] = "yz";
  prism["originX"] = "2";
  prism["originY"] = "3";
  prism["originZ"] = "4";
  prism["depth"] = "7.5";
  prism["polygon"] = "0,0;10,0;0,10";
  values.push_back({kCustomVolumeEntryEvaluationId, prism});

  return values;
}

OptionConfiguration Modifier(
    const std::string &id,
    std::initializer_list<std::pair<const char *, const char *>> overrides) {
  const ModifierRegistration *const registration = FindModifier(id);
  OptionSettings settings = registration->defaultSettings;
  for (const auto &[key, value] : overrides)
    settings[key] = value;
  return {id, std::move(settings)};
}

std::vector<OptionConfiguration> Modifiers() {
  return {
      Modifier(kRandomSteeringModifierId,
               {{"minTimeMs", "1200"}, {"maxTimeMs", "5200"}, {"seed", "42"}}),
      Modifier(kExistingEventPerturbationModifierId,
               {{"minTimeMs", "1100"},
                {"maxTimeMs", "5100"},
                {"seed", "43"},
                {"minCount", "2"},
                {"maxCount", "5"},
                {"maxTimeShiftMs", "120"},
                {"steerMode", "delta"},
                {"steerDeltaMin", "-0.25"},
                {"steerDeltaMax", "0.3"},
                {"toggleAccelerate", "true"},
                {"toggleBrake", "true"}}),
      Modifier(kExistingEventPerturbationModifierId,
               {{"minTimeMs", "1300"},
                {"maxTimeMs", "5300"},
                {"seed", "44"},
                {"minCount", "1"},
                {"maxCount", "4"},
                {"maxTimeShiftMs", "80"},
                {"steerMode", "absolute"},
                {"steerAbsoluteMin", "-0.75"},
                {"steerAbsoluteMax", "0.8"},
                {"toggleAccelerate", "false"},
                {"toggleBrake", "true"}}),
      Modifier(kSmoothSteeringModifierId,
               {{"minTimeMs", "1400"},
                {"maxTimeMs", "5400"},
                {"seed", "45"},
                {"deformationCount", "3"},
                {"radiusMs", "300"},
                {"amplitudeMin", "-0.35"},
                {"amplitudeMax", "0.4"}}),
      Modifier(kInputInsertionModifierId,
               {{"minTimeMs", "1500"},
                {"maxTimeMs", "5500"},
                {"seed", "46"},
                {"steerEnabled", "true"},
                {"steerMode", "absolute"},
                {"steerAbsoluteMin", "-0.7"},
                {"steerAbsoluteMax", "0.65"},
                {"steerMinCount", "1"},
                {"steerMaxCount", "3"},
                {"steerMaxHoldMs", "300"},
                {"accelerateEnabled", "true"},
                {"accelerateMinCount", "1"},
                {"accelerateMaxCount", "2"},
                {"accelerateMaxHoldMs", "100"},
                {"brakeEnabled", "true"},
                {"brakeMinCount", "0"},
                {"brakeMaxCount", "1"},
                {"brakeMaxHoldMs", "200"}}),
      Modifier(kInputInsertionModifierId,
               {{"minTimeMs", "1600"},
                {"maxTimeMs", "5600"},
                {"seed", "47"},
                {"steerEnabled", "true"},
                {"steerMode", "offset"},
                {"steerOffsetMin", "-0.3"},
                {"steerOffsetMax", "0.25"},
                {"steerMinCount", "0"},
                {"steerMaxCount", "4"},
                {"steerMaxHoldMs", "400"}}),
      Modifier(kInputDeletionModifierId,
               {{"minTimeMs", "1700"},
                {"maxTimeMs", "5700"},
                {"seed", "48"},
                {"steerEnabled", "true"},
                {"steerMaxCount", "4"},
                {"accelerateEnabled", "true"},
                {"accelerateMaxCount", "2"},
                {"brakeEnabled", "true"},
                {"brakeMaxCount", "3"}}),
  };
}

VisualProgram PersistenceFixture() {
  VisualProgram program;
  auto add = [&program](VisualNodeId id, const char *definitionId) -> VisualNode & {
    VisualNode node;
    node.id = id;
    node.definitionId = definitionId;
    return program.nodes.emplace(id, std::move(node)).first->second;
  };

  VisualNode &start = add(1, "flow/when-start");
  start.x = 54.25;
  start.y = 48.5;
  start.statements["body"] = {14};
  program.topLevel.push_back(1);

  VisualNode &search = add(14, "search/basic-brute-force");
  search.inputs["autoPromoteBest"] = 15;
  search.statements["body"] = {6, 18, 2};
  VisualNode &promote = add(15, "values/boolean");
  promote.fields["value"] = "true";

  VisualNode &simulate = add(18, "flow/simulate");
  simulate.inputs["until"] = 19;
  simulate.inputs["where"] = 20;
  VisualNode &horizon = add(19, "values/milliseconds");
  horizon.fields["value"] = "6500";
  VisualNode &where = add(20, "values/boolean");
  where.fields["value"] = "true";

  VisualNode &choose = add(2, "flow/set-objective");
  choose.inputs["score"] = 3;
  VisualNode &maximize = add(3, "objective/maximize");
  maximize.inputs["value"] = 4;
  maximize.inputs["range"] = 5;
  add(4, "simulation/car-speed");
  VisualNode &all = add(5, "time/range");
  all.inputs["from"] = 10;
  all.inputs["to"] = 11;

  VisualNode &window = add(6, "flow/mutation-window");
  window.inputs["range"] = 7;
  window.inputs["seed"] = 8;
  window.statements["body"] = {9};
  VisualNode &mutationRange = add(7, "time/range");
  mutationRange.inputs["from"] = 12;
  mutationRange.inputs["to"] = 13;
  VisualNode &seed = add(8, "values/integer");
  seed.fields["value"] = "1179926867";
  add(9, "mutate/reroll-steering");

  VisualNode &from = add(10, "values/milliseconds");
  from.fields["value"] = "1000";
  VisualNode &to = add(11, "values/milliseconds");
  to.fields["value"] = "6000";
  VisualNode &mutationFrom = add(12, "values/milliseconds");
  mutationFrom.fields["value"] = "1200";
  VisualNode &mutationTo = add(13, "values/milliseconds");
  mutationTo.fields["value"] = "5200";
  return program;
}

VisualProgram GenericPersistenceFixture() {
  VisualProgram program = PersistenceFixture();
  VisualNode add;
  add.id = 16;
  add.definitionId = "math/add";
  add.inputs["a"] = 4;
  add.inputs["b"] = 17;
  program.nodes.emplace(add.id, std::move(add));
  VisualNode five;
  five.id = 17;
  five.definitionId = "values/number";
  five.fields["value"] = "5";
  program.nodes.emplace(five.id, std::move(five));
  program.find(3)->inputs["value"] = 16;
  return program;
}

bool TestVisualProgramPersistence() {
  const VisualProgram fixture = PersistenceFixture();
  const QString first = PrintVisualProgramJson(fixture);
  const QString second = PrintVisualProgramJson(fixture);
  bool okay = Check(first == second, "v3 program serialization is not deterministic");
  const VisualProgramJson parsed = ParseVisualProgramJson(first);
  okay &= Check(parsed.program.has_value(),
                "v3 program serialization did not parse: " +
                    parsed.error.toStdString());
  if (parsed.program) {
    okay &= Check(PrintVisualProgramJson(*parsed.program) == first,
                  "v3 program changed after JSON round-trip");
  }

  const VisualProgramJson wrongVersion = ParseVisualProgramJson(
      QStringLiteral(R"({"version":2,"topLevel":[],"nodes":[]})"));
  okay &= Check(!wrongVersion.program.has_value(),
                "v3 parser accepted a version-2 document");
  const VisualProgramJson malformedId = ParseVisualProgramJson(QStringLiteral(
      R"({"version":3,"topLevel":["1"],"nodes":[{"id":"banana","definitionId":"flow/when-start","fields":{},"inputs":{},"statements":{},"x":0,"y":0}]})"));
  okay &= Check(!malformedId.program.has_value(),
                "v3 parser accepted a malformed node id");
  return okay;
}

bool TestSemanticPersistenceRestoresWithoutBlocklyCache() {
  QSettings().clear();
  SearchController controller;
  const VisualProgram semantic = PersistenceFixture();
  const QString persisted = PrintVisualProgramJson(semantic);
  QSettings settings;
  settings.setValue(QStringLiteral("blockEditor/v3Program"), persisted);
  settings.setValue(QStringLiteral("blockEditor/v3Workspace"),
                    QStringLiteral("not valid Blockly JSON"));

  BlockEditorBridge bridge(&controller);
  bool okay = Check(bridge.workspaceJson() != QStringLiteral("not valid Blockly JSON"),
                    "semantic v3 restore depended on the raw Blockly cache");
  const auto components = controller.blockComponents();
  okay &= Check(components.has_value(),
                "semantic v3 restore left the runtime configuration invalid");
  if (components && !components->modifiers.empty()) {
    const OptionSettings &settingsMap = components->modifiers.front().settings;
    okay &= Check(settingsMap.at("minTimeMs") == "1200" &&
                        settingsMap.at("maxTimeMs") == "5200" &&
                        settingsMap.at("seed") == "1179926867",
                    "semantic v3 program did not become runtime authority");
    okay &= Check(components->searchAlgorithm.settings.at("autoPromoteBest") ==
                      "true",
                  "semantic v3 search policy did not become runtime authority");
    okay &= Check(controller.simulationHorizonMs() == QStringLiteral("6500"),
                  "explicit simulate step did not become runtime authority");
  } else {
    okay = false;
  }

  const qulonglong revision = bridge.workspaceRevision() + 1;
  okay &= Check(bridge.applyWorkspace(bridge.workspaceJson(), revision),
                "workspace regenerated from semantic v3 JSON did not round-trip");
  const QString recovered =
      QSettings().value(QStringLiteral("blockEditor/v3Program")).toString();
  okay &= Check(recovered == persisted,
                "semantic v3 persistence changed during recovery\nexpected: " +
                    persisted.toStdString() + "\nactual:   " +
                    recovered.toStdString());
  return okay;
}

bool TestGenericSemanticPersistenceOwnsRuntimeConfiguration() {
  QSettings().clear();
  SearchController controller;
  const VisualProgram semantic = GenericPersistenceFixture();
  const QString persisted = PrintVisualProgramJson(semantic);
  QSettings settings;
  settings.setValue(QStringLiteral("blockEditor/v3Program"), persisted);
  settings.setValue(QStringLiteral("blockEditor/v3Workspace"),
                    QStringLiteral("broken Blockly cache"));

  BlockEditorBridge bridge(&controller);
  bool okay = Check(bridge.workspaceJson() != QStringLiteral("broken Blockly cache"),
                    "generic semantic restore depended on the Blockly cache");
  const auto components = controller.blockComponents();
  okay &= Check(components.has_value(),
                "generic semantic restore did not install runtime components");
  if (components) {
    okay &= Check(components->evaluationTarget.id ==
                          kVisualExpressionEvaluationId,
                  "generic v3 objective was forced back through the v2 evaluator model");
    okay &= Check(components->evaluationTarget.settings.at("expression") ==
                          "add carspeed num 5",
                  "generic v3 objective changed while becoming runtime authority");
    okay &= Check(controller.evaluationTargetId() ==
                          QString::fromLatin1(kVisualExpressionEvaluationId),
                  "controller did not expose the active generic evaluator id");
  }

  const qulonglong revision = bridge.workspaceRevision() + 1;
  okay &= Check(bridge.applyWorkspace(bridge.workspaceJson(), revision),
                "generic semantic workspace did not round-trip through Blockly");
  okay &= Check(QSettings().value(QStringLiteral("blockEditor/v3Program")).toString() ==
                        persisted,
                "generic semantic v3 graph changed after Blockly round-trip");
  return okay;
}

bool TestLegacyEditReplacesGenericRuntimeAuthority() {
  QSettings().clear();
  SearchController controller;
  const CompileResult generic = CompileVisualProgram(
      GenericPersistenceFixture(),
      {DefaultSearchAlgorithmConfiguration(), DefaultModifierConfigurations(),
       DefaultEvaluationTargetConfiguration()});
  bool okay = Check(generic.ok,
                    "generic fixture did not compile for authority test");
  if (!generic.ok) return false;
  okay &= Check(controller.applyBlockComponents(generic.configuration),
                "controller rejected generic runtime authority");
  okay &= Check(controller.evaluationTargetId() ==
                        QString::fromLatin1(kVisualExpressionEvaluationId),
                "generic runtime authority was not installed");

  controller.setEvaluationTargetId(QString::fromLatin1(kVelocityEvaluationId));
  const auto components = controller.blockComponents();
  okay &= Check(components &&
                        components->evaluationTarget.id == kVelocityEvaluationId,
                "legacy edit left a stale generic runtime override active");
  okay &= Check(controller.evaluationTargetId() ==
                        QString::fromLatin1(kVelocityEvaluationId),
                "legacy edit did not regain evaluation-target authority");
  return okay;
}

bool TestFiveHundredBlockWorkspaceStress() {
  QSettings().clear();
  SearchController controller;
  BlockEditorBridge bridge(&controller);
  const QJsonObject numberDefinition =
      CatalogBlock(bridge, QStringLiteral("values/number"));
  if (!Check(!numberDefinition.isEmpty(),
             "number block missing for 500-block stress fixture")) {
    return false;
  }

  QJsonDocument document = QJsonDocument::fromJson(bridge.workspaceJson().toUtf8());
  QJsonObject root = document.object();
  QJsonObject blocks = root.value(QStringLiteral("blocks")).toObject();
  QJsonArray top = blocks.value(QStringLiteral("blocks")).toArray();
  const QString type = numberDefinition.value(QStringLiteral("type")).toString();
  for (int index = 0; index < 500; ++index) {
    top.push_back(QJsonObject{
        {QStringLiteral("type"), type},
        {QStringLiteral("id"), QStringLiteral("stress-%1").arg(index)},
        {QStringLiteral("x"), 900.0 + static_cast<double>((index % 20) * 28)},
        {QStringLiteral("y"), 40.0 + static_cast<double>((index / 20) * 28)},
        {QStringLiteral("fields"),
         QJsonObject{{QStringLiteral("value"), index}}}});
  }
  blocks.insert(QStringLiteral("blocks"), top);
  root.insert(QStringLiteral("blocks"), blocks);
  const QString workspace = QString::fromUtf8(
      QJsonDocument(root).toJson(QJsonDocument::Compact));

  QElapsedTimer timer;
  timer.start();
  const bool accepted =
      bridge.applyWorkspace(workspace, bridge.workspaceRevision() + 1);
  const qint64 elapsedMs = timer.elapsed();
  bool okay = Check(
      accepted,
      "500-block workspace was rejected: " + bridge.diagnosticsJson().toStdString());
  okay &= Check(elapsedMs < 2500,
                "500-block workspace parse/validate/compile exceeded 2.5 seconds");
  okay &= Check(bridge.workspaceJson().size() > 0,
                "500-block workspace disappeared after acceptance");
  return okay;
}

} // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("ForeverTAS-tests"));
  QCoreApplication::setApplicationName(QStringLiteral("block-editor-bridge"));
  QTemporaryDir settingsDir;
  if (!settingsDir.isValid()) {
    std::cerr << "could not create temporary settings directory\n";
    return 1;
  }
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     settingsDir.path());
  QSettings().clear();

  SearchController controller;
  BlockEditorBridge bridge(&controller);
  bool okay = TestCatalogPublishesStatementFamilies(bridge);
  okay &= TestBridgeFollowsDarkMode(&controller, &bridge);
  okay &= TestCatalogPublishesViewerPickers(bridge);
  okay &= TestViewerTargetBridge(&controller, &bridge);
  okay &= TestVisualProgramPersistence();
  okay &= TestSemanticPersistenceRestoresWithoutBlocklyCache();
  okay &= TestGenericSemanticPersistenceOwnsRuntimeConfiguration();
  okay &= TestLegacyEditReplacesGenericRuntimeAuthority();
  okay &= TestLegacyConditionMigratesInsideSimulation();
  QSettings().clear();

  {
    OptionConfiguration search = DefaultSearchAlgorithmConfiguration();
    search.settings["autoPromoteBest"] = "true";
    SearchComponentConfiguration components{
        search, DefaultModifierConfigurations(),
        DefaultEvaluationTargetConfiguration()};
    okay &= Check(controller.applyBlockComponents(components),
                  "native setup rejected non-default search policy");
    const qulonglong revision = bridge.workspaceRevision() + 1;
    okay &= Check(bridge.applyWorkspace(bridge.workspaceJson(), revision),
                  "v3 workspace rejected non-default search policy");
    const auto roundTrip = controller.blockComponents();
    okay &= Check(roundTrip && roundTrip->searchAlgorithm == search,
                  "v3 workspace changed search policy settings");
  }

  for (const OptionConfiguration &evaluation : Evaluations()) {
    SearchComponentConfiguration components{
        DefaultSearchAlgorithmConfiguration(), DefaultModifierConfigurations(),
        evaluation};
    okay &= Check(controller.applyBlockComponents(components),
                  "native setup rejected " + evaluation.id);
    if (!okay)
      continue;

    const qulonglong revision = bridge.workspaceRevision() + 1;
    const bool accepted =
        bridge.applyWorkspace(bridge.workspaceJson(), revision);
    okay &=
        Check(accepted, "v3 workspace round-trip rejected " + evaluation.id +
                            ": " + bridge.diagnosticsJson().toStdString());
    const auto roundTrip = controller.blockComponents();
    okay &= Check(roundTrip.has_value(),
                  "runtime configuration disappeared for " + evaluation.id);
    if (roundTrip) {
      okay &=
          Check(roundTrip->evaluationTarget == evaluation,
                "v3 workspace changed evaluator settings for " + evaluation.id);
    }
  }

  for (const OptionConfiguration &modifier : Modifiers()) {
    SearchComponentConfiguration components{
        DefaultSearchAlgorithmConfiguration(), {modifier},
        DefaultEvaluationTargetConfiguration()};
    okay &= Check(controller.applyBlockComponents(components),
                  "native setup rejected modifier " + modifier.id);
    if (!okay)
      continue;

    const qulonglong revision = bridge.workspaceRevision() + 1;
    const bool accepted =
        bridge.applyWorkspace(bridge.workspaceJson(), revision);
    okay &= Check(accepted,
                  "v3 workspace round-trip rejected modifier " + modifier.id +
                      ": " + bridge.diagnosticsJson().toStdString());
    const auto roundTrip = controller.blockComponents();
    okay &= Check(roundTrip.has_value(),
                  "runtime configuration disappeared for modifier " +
                      modifier.id);
    if (roundTrip) {
      okay &= Check(roundTrip->modifiers == components.modifiers,
                    "v3 workspace changed modifier settings for " +
                        modifier.id);
    }
  }

  okay &= TestFiveHundredBlockWorkspaceStress();

  return okay ? 0 : 1;
}
