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
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <initializer_list>
#include <iostream>
#include <cmath>
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

bool TestExecutablePersistence() {
  QSettings().clear();
  VisualProgram program;
  const auto add = [&](VisualNodeId id, const char *type,
                       std::map<std::string,std::string> fields,
                       std::map<std::string,VisualNodeId> inputs,
                       std::map<std::string,std::vector<VisualNodeId>> statements) {
    program.nodes.emplace(id, VisualNode{id,type,std::move(fields),std::move(inputs),std::move(statements)});
  };
  add(1,"flow/when-start",{},{},{{"body",{2}}});
  add(2,"data/set",{{"name","observed"}},{{"value",3}},{});
  add(3,"procedures/value",{{"name","advance"},{"parameters","ticks"}},{{"arg0",4}},{});
  add(4,"values/number",{{"value","2"}},{},{});
  add(5,"procedures/define",{{"name","advance"},{"parameters","ticks"}},{},{{"body",{6,8}}});
  add(6,"flow/repeat",{},{{"count",7}},{{"body",{10}}});
  add(10,"simulation/step",{},{},{});
  add(7,"data/get",{{"name","ticks"}},{},{});
  add(8,"procedures/return",{},{{"value",9}},{});
  add(9,"simulation/state",{},{},{});
  program.topLevel={1,5};
  QSettings().setValue(QStringLiteral("blockEditor/v3Program"),PrintVisualProgramJson(program));
  SearchController controller;
  BlockEditorBridge bridge(&controller);
  bool okay=Check(controller.executableBlockProgram()!=nullptr,
                  "persisted block program was not wired into the controller");
  okay &= Check(bridge.applyWorkspace(bridge.workspaceJson(),bridge.workspaceRevision()+1),
                "dynamic procedure sockets did not survive the semantic/Blockly round-trip: "+bridge.diagnosticsJson().toStdString());
  const auto workspace=bridge.workspaceJson();
  const auto executable=controller.executableBlockProgram();
  controller.setSimulationHorizonMs(QStringLiteral("8000"));
  okay &= Check(workspace==bridge.workspaceJson() && executable==controller.executableBlockProgram(),
                "changing the horizon regenerated executable source from native settings");
  QTemporaryDir assets;
  QFile scenario(assets.filePath(QStringLiteral("scenario.Gbx")));
  okay &= Check(scenario.open(QIODevice::WriteOnly),"cannot create validation fixture");
  scenario.write("fixture"); scenario.close();
  controller.setPacksDirectory(assets.path());
  controller.setReplayPath(scenario.fileName());
  controller.setSimulationBackendId(QStringLiteral("reference"));
  okay &= Check(controller.canStart(),"program inherited an unrelated native search validation error: "+controller.validationMessage().toStdString());
  const auto exported=bridge.projectFromWorkspace(bridge.workspaceJson());
  okay &= Check(!exported.isEmpty() &&
      bridge.projectFromWorkspace(bridge.workspaceFromProject(exported))==exported,
      "project export/import did not preserve the executable source graph");
  controller.setSimulationBackendId(QStringLiteral("multi-threaded-cpu"));
  controller.setCpuWorkerCount(QStringLiteral("2"));
  okay &= Check(controller.canStart(),"general programs are still barred from the parallel CPU backend");
  controller.setCpuWorkerCount(QStringLiteral("0"));
  okay &= Check(!controller.canStart(),"general programs accepted an invalid worker count");
  controller.setCpuWorkerCount(QStringLiteral("2"));
  controller.setSimulationBackendId(QStringLiteral("reference"));
  {
    SearchController restored;
    BlockEditorBridge restoredBridge(&restored);
    okay &= Check(restored.executableBlockProgram() &&
        PrintVisualProgramJson(*restored.executableBlockProgram())==PrintVisualProgramJson(*executable),
        "program was lost on application restart");
    okay &= Check(restoredBridge.workspaceJson()==workspace,"procedure workspace changed on restart");
  }
  const QString draft = QStringLiteral(R"({"blocks":{"languageVersion":0,"blocks":[{"type":"ft_flow_when_start","inputs":{"body":{"block":{"type":"ft_data_set","fields":{"name":"draft"}}}}}]}})");
  okay &= Check(!bridge.applyWorkspace(draft,bridge.workspaceRevision()+1) && !controller.canStart(),
                "an incomplete workspace could still run the previous program");
  bridge.workspaceFromProject(QStringLiteral("not a project"));
  okay &= Check(!controller.canStart(),"a failed project import re-enabled an invalid visible program");
  {
    SearchController restored;
    BlockEditorBridge restoredBridge(&restored);
    okay &= Check(!restored.canStart() && restoredBridge.workspaceJson()==draft,
                  "an incomplete draft was lost or became runnable after restart");
  }
  okay &= Check(bridge.applyWorkspace(workspace,bridge.workspaceRevision()+2) && controller.canStart(),
                "fixing a draft did not re-enable program execution");
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

bool TestMacroPersistenceAndRetiredSources() {
  QSettings().clear();
  SearchController controller;
  BlockEditorBridge bridge(&controller);
  bool okay=true;
  const auto catalog=ParsedObject(bridge.catalogJson());
  const auto macros=catalog.value(QStringLiteral("macros")).toArray();
  okay &= Check(macros.size()>=18,"Macroblocks are missing from the native editor catalog");
  for (const auto &entry : macros) {
    const auto macro=entry.toObject();
    const QJsonObject start{{"type","ft_flow_when_start"},{"id","entry"},
      {"inputs",QJsonObject{{"body",QJsonObject{{"block",macro.value("stack")}}}}}};
    const QString workspace=QString::fromUtf8(QJsonDocument(QJsonObject{
      {"blocks",QJsonObject{{"languageVersion",0},{"blocks",QJsonArray{start}}}}}).toJson(QJsonDocument::Compact));
    okay &= Check(bridge.applyWorkspace(workspace,bridge.workspaceRevision()+1),
        "Expanded macro rejected: "+macro.value("id").toString().toStdString()+" "+bridge.diagnosticsJson().toStdString());
    const QString project=bridge.projectFromWorkspace(workspace);
    okay &= Check(!project.isEmpty() && bridge.projectFromWorkspace(bridge.workspaceFromProject(project))==project,
                  "Expanded macro lost its primitive source in project round-trip");
  }
  const QString visible=bridge.workspaceJson();
  controller.setConditionScript(QStringLiteral("intentionally invalid native condition"));
  controller.applyBlockComponents({DefaultSearchAlgorithmConfiguration(),DefaultModifierConfigurations(),DefaultEvaluationTargetConfiguration()});
  okay &= Check(bridge.workspaceJson()==visible,"Native settings replaced the visual source with a predefined search");
  okay &= Check(bridge.applyWorkspace(visible,bridge.workspaceRevision()+1),"Native settings prevent the source graph from being reapplied");
  const QString retired=QStringLiteral(R"({"version":3,"nodes":[{"id":"1","definitionId":"mutate/delete-accelerate","fields":{},"inputs":{},"statements":{},"x":0,"y":0}],"topLevel":["1"]})");
  QSettings().clear();
  QSettings().setValue(QStringLiteral("blockEditor/v3Program"),retired);
  SearchController recovered;
  BlockEditorBridge recoveredBridge(&recovered);
  okay &= Check(QSettings().value(QStringLiteral("blockEditor/v3ProgramCorruptBackup")).toString()==retired &&
                recoveredBridge.diagnosticsJson().contains(QStringLiteral("backed up")),
                "Retired wrapper source was silently discarded or silently reinterpreted");
  okay &= Check(recovered.executableBlockProgram() && recovered.executableBlockProgram()->nodes.size()==1,
                "Retired native wrappers were recreated in the new workspace");
  const auto goodProject=recoveredBridge.projectFromWorkspace(recoveredBridge.workspaceJson());
  for (const QString &bad : {QStringLiteral("not json"),QStringLiteral("{}"),retired})
    okay &= Check(recoveredBridge.workspaceFromProject(bad).isEmpty(),"Malformed/retired project imported as an executable");
  okay &= Check(recoveredBridge.projectFromWorkspace(recoveredBridge.workspaceJson())==goodProject,
                "Failed import changed the current source");
  QSettings().clear();
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

bool TestDebugRuntime(const QString &packs, const QString &scenario) {
  QSettings().clear();
  SearchController controller;
  controller.setPacksDirectory(packs);
  controller.setReplayPath(scenario);
  controller.setSimulationBackendId(QStringLiteral("reference"));
  BlockEditorBridge bridge(&controller);
  const QString workspace = QStringLiteral(R"({"blocks":{"languageVersion":0,"blocks":[{
    "type":"ft_flow_when_start","id":"entry","inputs":{"body":{"block":{
      "type":"ft_data_set","id":"set-value","fields":{"name":"watched"},"inputs":{"value":{"block":{
        "type":"ft_values_number","id":"seven","fields":{"value":7}}}},"next":{"block":{
          "type":"ft_flow_forever","id":"empty-loop"}}}}}}]}})");
  const auto waitFor = [](const std::function<bool()> &predicate) {
    QElapsedTimer elapsed; elapsed.start();
    while (!predicate() && elapsed.elapsed()<15000) {
      QCoreApplication::processEvents();
      QThread::msleep(1);
    }
    return predicate();
  };
  const auto debug = [&] { return QJsonDocument::fromJson(bridge.debugJson().toUtf8()).object(); };
  if (!Check(bridge.runWorkspace(workspace,bridge.workspaceRevision()+1,true),
      "could not start a debug program: "+controller.validationMessage().toStdString())) return false;
  bool okay=Check(waitFor([&] { return debug().value("paused").toBool(); }),"native worker did not pause before execution");
  okay &= Check(debug().value("block").toString()==QStringLiteral("set-value"),
                "debugger failed to map executable node IDs to the visible Blockly source");
  bridge.resumeProgram(QStringLiteral("over"));
  okay &= Check(waitFor([&] { return debug().value("paused").toBool(); }),"Step Over did not stop at the next command");
  okay &= Check(debug().value("block").toString()==QStringLiteral("empty-loop") &&
      debug().value("variables").toArray().at(0).toObject().value("value").toString()==QStringLiteral("7"),
      "Step Over did not execute the assignment or publish its variable");
  bridge.stopProgram();
  okay &= Check(waitFor([&] { return !controller.running(); }),"Stop failed to wake the paused native worker");
  okay &= Check(debug().value("finished").toBool(),"debug session did not publish completion");
  // A second run must not inherit the old pause state or a dead observer.
  bridge.inspectProgram(false);
  okay &= Check(bridge.runWorkspace(workspace,bridge.workspaceRevision()+1,false),"could not restart after a stopped debug run");
  bridge.stopProgram();
  okay &= Check(waitFor([&] { return !controller.running(); }),"restarted program failed to stop");
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
  if (argc==4 && std::string(argv[1])=="--debug-runtime") {
    const bool okay=TestDebugRuntime(QString::fromUtf8(argv[2]),QString::fromUtf8(argv[3]));
    if (okay) std::cout << "PASS native block debugger: source mapping, Step Over, watches, Stop, restart\n";
    return okay ? 0 : 1;
  }
  if (argc==2 && std::string(argv[1])=="--dump-catalog") {
    std::cout << bridge.catalogJson().toStdString() << '\n';
    return 0;
  }
  if ((argc==3 || argc==5) && std::string(argv[1])=="--validate-workspace") {
    QFile file(QString::fromUtf8(argv[2]));
    if (!file.open(QIODevice::ReadOnly)) return 2;
    const bool accepted=bridge.applyWorkspace(QString::fromUtf8(file.readAll()),bridge.workspaceRevision()+1);
    if (!accepted) std::cerr << bridge.diagnosticsJson().toStdString() << '\n';
    if (accepted && argc==5) {
      if (!controller.executableBlockProgram()) return 3;
      SearchRequest request{argv[3],argv[4]};
      request.backend=PhysicsBackend::MultiThreadedCpu;
      request.parallelSampleCount=3;
      request.executable=std::make_shared<const VisualProgram>(*controller.executableBlockProgram());
      try {
        const auto result=RunSearch(request);
        if (!std::isfinite(result.bestScore) || result.bestTimeline.empty()) return 4;
      } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 5;
      }
    }
    return accepted ? 0 : 1;
  }
  bool okay = TestBridgeFollowsDarkMode(&controller, &bridge);
  okay &= TestCatalogPublishesViewerPickers(bridge);
  okay &= TestViewerTargetBridge(&controller, &bridge);
  okay &= TestExecutablePersistence();
  okay &= TestMacroPersistenceAndRetiredSources();
  okay &= TestFiveHundredBlockWorkspaceStress();
  return okay ? 0 : 1;
}
