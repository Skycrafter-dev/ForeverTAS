#include "app/cuboid_target_model.h"
#include "app/compact_number_format.h"
#include "app/custom_volume_target_model.h"
#include "app/pose_target_model.h"
#include "app/packs_directory_finder.h"
#include "app/search_configuration_model.h"
#include "app/search_controller.h"
#include "app/search_worker.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QVariantMap>

#include <clocale>
#include <functional>
#include <iostream>
#include <limits>
#include <string>

namespace {

using forevertas::app::SearchController;
using forevertas::app::CuboidTargetModel;
using forevertas::app::CustomVolumeTargetModel;
using forevertas::app::PoseTargetModel;
using forevertas::app::FormatCompactNumber;

bool Check(bool condition, const char *message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}


bool WaitUntil(const std::function<bool()> &condition, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return condition();
}

class NumericLocaleGuard final {
public:
    NumericLocaleGuard() {
        if (const char *const current = std::setlocale(LC_NUMERIC, nullptr)) {
            original_ = current;
        }
    }

    ~NumericLocaleGuard() {
        if (!original_.empty()) {
            std::setlocale(LC_NUMERIC, original_.c_str());
        }
    }

    bool ActivateCommaDecimalLocale() {
        constexpr const char *localeNames[] = {
                "fr_FR.utf8", "fr_FR.UTF-8", "de_DE.utf8", "de_DE.UTF-8"};
        for (const char *const localeName : localeNames) {
            if (std::setlocale(LC_NUMERIC, localeName) == nullptr) continue;
            const lconv *const details = std::localeconv();
            if (details != nullptr && details->decimal_point != nullptr &&
                std::string(details->decimal_point) == ",") {
                return true;
            }
        }
        return false;
    }

private:
    std::string original_;
};

bool HasOption(const QVariantList &options,
               const QString &id,
               const QString &component) {
    for (const QVariant &value : options) {
        const QVariantMap option = value.toMap();
        if (option.value(QStringLiteral("id")).toString() == id &&
            option.value(QStringLiteral("settingsComponent")).toString() ==
                    component) {
            return true;
        }
    }
    return false;
}

bool HasBackendOption(const QVariantList &options,
                      const QString &id,
                      const QString &label,
                      const QString &description) {
    for (const QVariant &value : options) {
        const QVariantMap option = value.toMap();
        if (option.value(QStringLiteral("id")).toString() == id &&
            option.value(QStringLiteral("label")).toString() == label &&
            option.value(QStringLiteral("description")).toString() ==
                    description) {
            return true;
        }
    }
    return false;
}

bool TestCompactNumberFormatting() {
    bool okay = true;
    const auto expect = [&okay](double value, const char *expected) {
        const QString actual = FormatCompactNumber(value);
        const QString expectedText = QString::fromLatin1(expected);
        if (actual != expectedText) {
            std::cerr << "compact number mismatch for " << value << ": "
                      << actual.toStdString() << " != " << expected << '\n';
            okay = false;
        }
    };
    expect(0.0, "0");
    expect(4.0, "4");
    expect(4.5, "4.50");
    expect(999.0, "999");
    expect(1000.0, "1.00k");
    expect(1230.0, "1.23k");
    expect(999999.0, "1.00M");
    expect(1250000.0, "1.25M");
    expect(1230000000.0, "1.23B");
    expect(1230000000000.0, "1.23T");
    expect(999999999999999.0, "1.00Q");
    expect(1230000000000000.0, "1.23Q");
    expect(1000000000000000000.0, "1000.00Q");
    expect(-12.0, "-12");
    expect(-12500.0, "-12.50k");
    return okay;
}

bool TestAutomaticSeedRandomization() {
    QSettings().clear();
    {
        SearchController controller;
        if (!Check(controller.randomizeSeedsOnStart() &&
                           QSettings()
                                   .value(QStringLiteral(
                                           "search/randomizeSeedsOnStart"))
                                   .toBool(),
                   "automatic seed randomization was not default-on")) {
            return false;
        }
        controller.setRandomizeSeedsOnStart(false);
    }
    {
        SearchController restored;
        if (!Check(!restored.randomizeSeedsOnStart(),
                   "automatic seed randomization was not persisted")) {
            return false;
        }
    }

    QSettings().clear();
    forevertas::app::SearchConfigurationModel configuration;
    configuration.addModifierPass(
            QStringLiteral("existing-event-perturbation"));
    const QVariantList before = configuration.modifierPasses();
    if (!Check(configuration.randomizeModifierSeeds(123456789u),
               "seeded modifier passes were not randomized")) {
        return false;
    }
    const QVariantList after = configuration.modifierPasses();
    bool okay = Check(after.size() == before.size(),
                      "seed randomization changed modifier composition");
    for (qsizetype index = 0; index < after.size(); ++index) {
        const QVariantMap beforeSettings = before.at(index)
                .toMap()
                .value(QStringLiteral("settings"))
                .toMap();
        const QVariantMap afterSettings = after.at(index)
                .toMap()
                .value(QStringLiteral("settings"))
                .toMap();
        okay &= Check(afterSettings.value(QStringLiteral("seed")) !=
                                      beforeSettings.value(
                                              QStringLiteral("seed")),
                      "a modifier seed did not change");
    }
    forevertas::app::SearchConfigurationModel restored;
    okay &= Check(restored.modifierPasses() == after,
                  "randomized modifier seeds were not persisted");
    return okay;
}

bool TestTargetVisibilityPersistence() {
    QSettings().clear();
    {
        SearchController controller;
        if (!Check(!controller.drawTargetsThroughBlocks(),
                   "targets did not default to block-occluded rendering")) {
            return false;
        }
        controller.setDrawTargetsThroughBlocks(true);
    }
    SearchController restored;
    return Check(restored.drawTargetsThroughBlocks(),
                 "draw-through target rendering was not persisted");
}

bool TestAbsoluteTargetPlacement() {
    QSettings().clear();
    CuboidTargetModel cuboid;
    CustomVolumeTargetModel customVolume;
    PoseTargetModel pose;
    QSignalSpy cuboidChanged(&cuboid,
                             &CuboidTargetModel::selectedTargetChanged);
    QSignalSpy customChanged(
            &customVolume,
            &CustomVolumeTargetModel::selectedTargetChanged);
    QSignalSpy poseChanged(&pose,
                           &PoseTargetModel::selectedTargetChanged);

    const QString polygon = customVolume.selectedTarget()
            .value(QStringLiteral("polygon"))
            .toString();
    const QQuaternion rotation =
            QQuaternion::fromEulerAngles(15.0F, -25.0F, 35.0F);
    bool okay = Check(cuboid.moveSelectedTo(11.0, 12.0, 13.0) &&
                              customVolume.moveSelectedTo(
                                      -21.0, -22.0, -23.0) &&
                              pose.moveSelectedTo(
                                      31.0, 32.0, 33.0, rotation),
                      "absolute target placement failed");
    const QVariantMap cuboidTarget = cuboid.selectedTarget();
    const QVariantMap customTarget = customVolume.selectedTarget();
    const QVariantMap poseTarget = pose.selectedTarget();
    okay &= Check(
            cuboidTarget.value(QStringLiteral("centerX")).toString() ==
                            QStringLiteral("11") &&
                    cuboidTarget.value(QStringLiteral("centerY")).toString() ==
                            QStringLiteral("12") &&
                    cuboidTarget.value(QStringLiteral("centerZ")).toString() ==
                            QStringLiteral("13") &&
                    customTarget.value(QStringLiteral("originX")).toString() ==
                            QStringLiteral("-21") &&
                    customTarget.value(QStringLiteral("originY")).toString() ==
                            QStringLiteral("-22") &&
                    customTarget.value(QStringLiteral("originZ")).toString() ==
                            QStringLiteral("-23") &&
                    customTarget.value(QStringLiteral("polygon")).toString() ==
                            polygon &&
                    poseTarget.value(QStringLiteral("x")).toString() ==
                            QStringLiteral("31") &&
                    poseTarget.value(QStringLiteral("y")).toString() ==
                            QStringLiteral("32") &&
                    poseTarget.value(QStringLiteral("z")).toString() ==
                            QStringLiteral("33") &&
                    std::abs(QQuaternion::dotProduct(
                            poseTarget.value(QStringLiteral("rotation"))
                                    .value<QQuaternion>(),
                            rotation.normalized())) > 0.99999F,
            "absolute placement stored an incorrect target pose");
    okay &= Check(cuboidChanged.count() == 1 &&
                          customChanged.count() == 1 &&
                          poseChanged.count() == 1,
                  "absolute placement was not an atomic model edit");
    okay &= Check(!cuboid.moveSelectedTo(
                            std::numeric_limits<double>::infinity(), 0, 0) &&
                          !customVolume.moveSelectedTo(10000001.0, 0, 0) &&
                          !pose.moveSelectedTo(
                                  0,
                                  0,
                                  0,
                                  QQuaternion(
                                          std::numeric_limits<float>::quiet_NaN(),
                                          0,
                                          0,
                                          0)),
                  "absolute placement accepted an invalid pose");
    return okay;
}

bool TestCuboidTargetModel() {
    QSettings().clear();
    const QVariantMap legacy{
            {QStringLiteral("centerX"), QStringLiteral("1.5")},
            {QStringLiteral("centerY"), QStringLiteral("-2")},
            {QStringLiteral("centerZ"), QStringLiteral("3")},
            {QStringLiteral("sizeX"), QStringLiteral("4")},
            {QStringLiteral("sizeY"), QStringLiteral("5")},
            {QStringLiteral("sizeZ"), QStringLiteral("6")}};
    CuboidTargetModel model(legacy);
    bool okay = Check(model.count() == 1 && model.selectedIndex() == 0,
                      "cuboid model did not create its legacy target");
    QVariantMap selected = model.selectedTarget();
    okay &= Check(selected.value(QStringLiteral("centerX")).toString() ==
                                  QStringLiteral("1.5") &&
                          selected.value(QStringLiteral("sizeZ")).toString() ==
                                  QStringLiteral("6"),
                  "cuboid model did not migrate legacy dimensions");
    okay &= Check(model.addTarget(
                              std::numeric_limits<double>::infinity(),
                              0.0,
                              0.0) == -1 &&
                          !model.setCenterComponent(
                                  0,
                                  QStringLiteral("x"),
                                  QStringLiteral("nan")) &&
                          !model.setCenterComponent(
                                  0,
                                  QStringLiteral("x"),
                                  QStringLiteral("1e300")) &&
                          !model.setSizeComponent(
                                  0,
                                  QStringLiteral("x"),
                                  QStringLiteral("0")),
                  "cuboid model accepted non-finite or non-positive values");

    const int added = model.addTarget(10.0, 20.0, 30.0);
    okay &= Check(added == 1 && model.selectedIndex() == 1 &&
                          model.count() == 2,
                  "cuboid placement did not add and select a target");
    okay &= Check(model.setName(added, QStringLiteral("  Finish box  ")) &&
                          model.setCenterComponent(
                                  added,
                                  QStringLiteral("y"),
                                  QStringLiteral("21.25")) &&
                          model.setSizeComponent(
                                  added,
                                  QStringLiteral("z"),
                                  QStringLiteral("2.5")) &&
                          model.translateSelected(1.0, -1.0, 2.0) &&
                          model.resizeSelected(
                                  QStringLiteral("x"), -9.5),
                  "cuboid direct or 3D-style edits failed");
    selected = model.selectedTarget();
    okay &= Check(selected.value(QStringLiteral("name")).toString() ==
                                  QStringLiteral("Finish box") &&
                          selected.value(QStringLiteral("centerX")).toString() ==
                                  QStringLiteral("11") &&
                          selected.value(QStringLiteral("centerY")).toString() ==
                                  QStringLiteral("20.25") &&
                          selected.value(QStringLiteral("sizeX")).toString() ==
                                  QStringLiteral("0.5"),
                  "cuboid edits produced incorrect properties");

    QSettings settings;
    settings.setValue(QStringLiteral("unrelated/largePayload"),
                      QByteArray(512 * 1024, 'x'));
    settings.sync();
    QElapsedTimer resizeTimer;
    resizeTimer.start();
    constexpr int kResizeCount = 2000;
    for (int edit = 0; edit < kResizeCount; ++edit) {
        okay &= model.resizeSelected(QStringLiteral("x"), 0.001);
    }
    const qint64 resizeElapsedMs = resizeTimer.elapsed();
    okay &= Check(resizeElapsedMs < 250,
                  "continuous cuboid resize synchronously rewrote settings");
    const QString finalSizeX =
            model.selectedTarget().value(QStringLiteral("sizeX")).toString();
    okay &= Check(WaitUntil(
                              [&]() {
                                  const QJsonDocument persisted =
                                          QJsonDocument::fromJson(
                                                  QSettings()
                                                          .value(QStringLiteral(
                                                                  "targets/cuboids"))
                                                          .toByteArray());
                                  const QJsonArray targets =
                                          persisted.object()
                                                  .value(QStringLiteral("targets"))
                                                  .toArray();
                                  return targets.size() > added &&
                                          QString::number(
                                                  targets.at(added)
                                                          .toObject()
                                                          .value(QStringLiteral("size"))
                                                          .toArray()
                                                          .at(0)
                                                          .toDouble(),
                                                  'g',
                                                  15) == finalSizeX;
                              },
                              1000),
                  "deferred cuboid resize did not persist its final value");
    const int duplicate = model.duplicateSelected();
    okay &= Check(duplicate == 2 && model.count() == 3 &&
                          model.selectedTarget()
                                          .value(QStringLiteral("centerX"))
                                          .toString() ==
                                  QStringLiteral("12"),
                  "cuboid duplication did not offset and select the copy");
    okay &= Check(model.removeTarget(1) && model.count() == 2 &&
                          model.selectedIndex() == 1,
                  "cuboid removal did not preserve the selected copy");

    const QString selectedId =
            model.selectedTarget().value(QStringLiteral("id")).toString();
    CuboidTargetModel restored;
    okay &= Check(restored.count() == 2 &&
                          restored.selectedTarget()
                                          .value(QStringLiteral("id"))
                                          .toString() == selectedId,
                  "cuboid collection or selection did not persist");
    okay &= Check(restored.removeTarget(0) && restored.count() == 1 &&
                          !restored.removeTarget(0),
                  "cuboid model allowed removal of the final target");
    restored.setEditingEnabled(false);
    okay &= Check(!restored.editingEnabled() &&
                          restored.addTarget(0.0, 0.0, 0.0) == -1 &&
                          !restored.setName(
                                  0, QStringLiteral("Locked")) &&
                          !restored.translateSelected(1.0, 0.0, 0.0),
                  "cuboid edits were not frozen for a running search");

    QSettings().setValue(
            QStringLiteral("targets/cuboids"),
            QByteArrayLiteral("{\"version\":1,\"targets\":["
                              "{\"id\":\"bad\",\"name\":\"Bad\","
                              "\"center\":[0,0,0],\"size\":[1,0,1]}]}"));
    CuboidTargetModel recovered(legacy);
    okay &= Check(recovered.count() == 1 &&
                          recovered.selectedTarget()
                                          .value(QStringLiteral("sizeY"))
                                          .toString() ==
                                  QStringLiteral("5"),
                  "corrupt cuboid persistence did not recover safely");
    return okay;
}

bool TestCuboidControllerSynchronization() {
    QSettings().clear();
    SearchController controller;
    controller.setEvaluationTargetId(QStringLiteral("volume-entry-time"));
    CuboidTargetModel *const cuboids = controller.cuboidTargets();
    bool okay = Check(cuboids != nullptr && cuboids->count() == 1,
                      "controller did not expose its cuboid collection");
    const int second = cuboids->addTarget(7.0, 8.0, 9.0);
    okay &= Check(second == 1 &&
                          controller.evaluationTargetSettings()
                                          .value(QStringLiteral("centerX"))
                                          .toString() ==
                                  QStringLiteral("7") &&
                          controller.evaluationTargetSettings()
                                          .value(QStringLiteral("sizeX"))
                                          .toString() ==
                                  QStringLiteral("10"),
                  "selected cuboid did not become the active search target");
    okay &= Check(cuboids->setSizeComponent(
                              second,
                              QStringLiteral("y"),
                              QStringLiteral("3.25")) &&
                          controller.evaluationTargetSettings()
                                          .value(QStringLiteral("sizeY"))
                                          .toString() ==
                                  QStringLiteral("3.25"),
                  "cuboid property edit did not update evaluation settings");
    controller.setEvaluationTargetSetting(
            QStringLiteral("centerZ"), QStringLiteral("12.5"));
    okay &= Check(cuboids->selectedTarget()
                                  .value(QStringLiteral("centerZ"))
                                  .toString() == QStringLiteral("12.5"),
                  "legacy setting edit did not update the selected cuboid");
    cuboids->selectTarget(0);
    okay &= Check(controller.evaluationTargetSettings()
                                  .value(QStringLiteral("centerX"))
                                  .toString() == QStringLiteral("0"),
                  "cuboid selection did not switch the active target");
    return okay;
}

bool TestCustomVolumeTargets() {
    QSettings().clear();
    CustomVolumeTargetModel model;
    QObject *const initialGeometry =
            model.selectedTarget()
                    .value(QStringLiteral("geometry"))
                    .value<QObject *>();
    bool okay = Check(model.count() == 1 &&
                              model.selectedTarget()
                                      .value(QStringLiteral("valid"))
                                      .toBool(),
                      "custom volume model did not create a valid target");
    const QString originalPolygon =
            model.selectedTarget()
                    .value(QStringLiteral("polygon"))
                    .toString();
    okay &= Check(model.setPlane(0, QStringLiteral("xy")) &&
                          model.setDepth(0, QStringLiteral("7.5")) &&
                          model.setVertex(
                                  0,
                                  0,
                                  QStringLiteral("u"),
                                  QStringLiteral("-6")),
                  "custom volume property edits failed");
    const QString editedPolygon =
            model.selectedTarget()
                    .value(QStringLiteral("polygon"))
                    .toString();
    QObject *const editedGeometry =
            model.selectedTarget()
                    .value(QStringLiteral("geometry"))
                    .value<QObject *>();
    okay &= Check(editedPolygon != originalPolygon &&
                          initialGeometry != nullptr &&
                          editedGeometry != nullptr &&
                          editedGeometry != initialGeometry &&
                          model.selectedTarget()
                                          .value(QStringLiteral("depth"))
                                          .toString() ==
                                  QStringLiteral("7.5"),
                  "polygon and extrusion geometry did not update");
    okay &= Check(model.beginDrawing() && model.drawing() &&
                          model.selectedTarget()
                                          .value(QStringLiteral("vertexCount"))
                                          .toInt() == 0 &&
                          model.addVertexWorld(0.0, 0.0, 0.0) &&
                          model.addVertexWorld(4.0, 0.0, 0.0) &&
                          model.addVertexWorld(0.0, 4.0, 0.0) &&
                          model.finishDrawing() && !model.drawing() &&
                          model.setVertex(
                                  0,
                                  0,
                                  QStringLiteral("u"),
                                  QStringLiteral("1.23456789")),
                  "3D polygon drawing lifecycle failed");
    const QString drawnPolygon =
            model.selectedTarget()
                    .value(QStringLiteral("polygon"))
                    .toString();
    const QVariantMap displayedVertex =
            model.selectedTarget()
                    .value(QStringLiteral("vertices"))
                    .toList()
                    .front()
                    .toMap();
    okay &= Check(
            displayedVertex.value(QStringLiteral("u")).toString() ==
                    QStringLiteral("1.235"),
            "custom volume vertex properties were not display-formatted");
    okay &= Check(model.resizeDepthSelected(1.0) &&
                          model.selectedTarget()
                                          .value(QStringLiteral("polygon"))
                                          .toString() == drawnPolygon,
                  "extrusion editing changed the 2D polygon");
    okay &= Check(!model.setDepth(0, QStringLiteral("10000001")) &&
                          !model.translateSelected(
                                  10000001.0, 0.0, 0.0),
                  "custom volume accepted out-of-range geometry");
    okay &= Check(model.beginDrawing() &&
                          model.addVertexWorld(1.0, 1.0, 0.0),
                  "custom volume redraw did not start");
    model.cancelDrawing();
    okay &= Check(!model.drawing() &&
                          model.selectedTarget()
                                          .value(QStringLiteral("polygon"))
                                          .toString() == drawnPolygon,
                  "cancel drawing did not restore the polygon");
    okay &= Check(model.beginDrawing() &&
                          model.addVertexWorld(0.0, 0.0, 0.0) &&
                          model.addVertexWorld(4.0, 4.0, 0.0) &&
                          model.addVertexWorld(0.0, 4.0, 0.0) &&
                          model.addVertexWorld(4.0, 0.0, 0.0) &&
                          !model.finishDrawing() && model.drawing(),
                  "self-intersecting drawn polygon was accepted");
    model.cancelDrawing();
    okay &= Check(
            model.setOriginComponent(
                    0, QStringLiteral("x"), QStringLiteral("10000000")) &&
                    model.setOriginComponent(
                            0,
                            QStringLiteral("z"),
                            QStringLiteral("10000000")) &&
                    model.duplicateSelected() == 1 &&
                    std::abs(
                            model.selectedTarget()
                                    .value(QStringLiteral("origin"))
                                    .value<QVector3D>()
                                    .x()) <= 10000000.0F &&
                    std::abs(
                            model.selectedTarget()
                                    .value(QStringLiteral("origin"))
                                    .value<QVector3D>()
                                    .z()) <= 10000000.0F,
            "custom volume duplication exceeded coordinate limits");
    model.selectTarget(0);
    CustomVolumeTargetModel restored;
    okay &= Check(restored.selectedTarget()
                                  .value(QStringLiteral("polygon"))
                                  .toString() == drawnPolygon,
                  "custom volume did not persist");
    QSettings().setValue(
            QStringLiteral("targets/customVolumes"),
            QByteArrayLiteral("{not valid json"));
    CustomVolumeTargetModel recovered;
    okay &= Check(recovered.count() == 1 &&
                          recovered.selectedTarget()
                                  .value(QStringLiteral("valid"))
                                  .toBool(),
                  "custom volume did not recover from corrupt persistence");

    QSettings().clear();
    SearchController controller;
    controller.setEvaluationTargetId(
            QStringLiteral("custom-volume-entry-time"));
    CustomVolumeTargetModel *const targets =
            controller.customVolumeTargets();
    targets->setDepth(0, QStringLiteral("8"));
    okay &= Check(controller.evaluationTargetSettings()
                                  .value(QStringLiteral("depth"))
                                  .toString() == QStringLiteral("8"),
                  "custom volume did not synchronize with search settings");
    return okay;
}

bool TestPoseTargets() {
    QSettings().clear();
    PoseTargetModel model;
    bool okay = Check(
            model.count() == 1 &&
                    model.selectedTarget()
                                    .value(QStringLiteral("name"))
                                    .toString() ==
                            QStringLiteral("Car pose 1"),
            "pose target model did not create its default target");
    okay &= Check(
            model.setPositionComponent(
                    0, QStringLiteral("x"), QStringLiteral("12.5")) &&
                    model.setPositionComponent(
                            0, QStringLiteral("y"), QStringLiteral("3")) &&
                    model.setPositionComponent(
                            0, QStringLiteral("z"), QStringLiteral("-4")) &&
                    model.setRotationComponent(
                            0, QStringLiteral("yaw"), QStringLiteral("90")) &&
                    model.setRotationComponent(
                            0,
                            QStringLiteral("pitch"),
                            QStringLiteral("-20")) &&
                    model.setRotationComponent(
                            0,
                            QStringLiteral("roll"),
                            QStringLiteral("45")),
            "pose target property edits failed");
    const QQuaternion expected = QQuaternion::fromEulerAngles(
            45.0F, -20.0F, 90.0F);
    const QQuaternion actual = model.selectedTarget()
                                       .value(QStringLiteral("rotation"))
                                       .value<QQuaternion>();
    okay &= Check(
            std::abs(QQuaternion::dotProduct(
                    expected.normalized(), actual.normalized())) > 0.9999F,
            "pose target Euler properties produced the wrong orientation");
    okay &= Check(
            model.translateSelected(1.0, 0.0, 0.0) &&
                    model.rotateSelected(QStringLiteral("yaw"), 300.0) &&
                    model.selectedTarget()
                                    .value(QStringLiteral("yawDegrees"))
                                    .toString() ==
                            QStringLiteral("30"),
            "pose target direct manipulation failed");
    const QVariantMap edited = model.selectedTarget();
    okay &= Check(
            model.duplicateSelected() == 1 &&
                    model.selectedTarget()
                                    .value(QStringLiteral("name"))
                                    .toString() ==
                            QStringLiteral("Car pose 2") &&
                    model.selectTarget(0),
            "pose target list operations failed");
    PoseTargetModel restored;
    okay &= Check(
            restored.selectedTarget()
                            .value(QStringLiteral("position"))
                            .value<QVector3D>() ==
                    edited.value(QStringLiteral("position"))
                            .value<QVector3D>() &&
                    restored.selectedTarget()
                                    .value(QStringLiteral("yawDegrees"))
                                    .toString() ==
                            QStringLiteral("30"),
            "pose targets did not persist");
    model.setEditingEnabled(false);
    okay &= Check(
            !model.translateSelected(1.0, 0.0, 0.0) &&
                    !model.rotateSelected(QStringLiteral("yaw"), 1.0) &&
                    !model.selectTarget(1) &&
                    !model.setPositionComponent(
                            0,
                            QStringLiteral("x"),
                            QStringLiteral("10000001")),
            "pose target editing lock or bounds were bypassed");
    model.setEditingEnabled(true);
    const QVariantMap lockedTarget = model.selectedTarget();
    okay &= Check(
            !model.setPositionComponent(
                    0, QStringLiteral("x"), QStringLiteral("nan")) &&
                    !model.setPositionComponent(
                            0,
                            QStringLiteral("z"),
                            QStringLiteral("10000001")) &&
                    !model.setRotationComponent(
                            0, QStringLiteral("spin"), QStringLiteral("5")) &&
                    !model.setRotationComponent(
                            0,
                            QStringLiteral("yaw"),
                            QStringLiteral("-10000001")) &&
                    !model.translateSelected(
                            std::numeric_limits<double>::infinity(),
                            0.0,
                            0.0) &&
                    !model.rotateSelected(
                            QStringLiteral("pitch"),
                            std::numeric_limits<double>::quiet_NaN()) &&
                    model.addTarget(
                            0.0,
                            0.0,
                            0.0,
                            QQuaternion(
                                    std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::max(),
                                    0.0F,
                                    0.0F)) == -1 &&
                    model.addTarget(
                            0.0,
                            0.0,
                            0.0,
                            QQuaternion(0.0F, 0.0F, 0.0F, 0.0F)) == -1 &&
                    !model.setName(0, QStringLiteral("   ")) &&
                    !model.removeTarget(99) &&
                    model.selectedTarget() == lockedTarget,
            "invalid pose target edits changed model state");
    okay &= Check(
            model.removeTarget(1) &&
                    model.count() == 1 &&
                    !model.removeTarget(0),
            "pose target removal did not preserve a usable final target");

    QSettings().setValue(
            QStringLiteral("targets/poses"),
            QByteArrayLiteral(
                    "{\"version\":1,\"selectedId\":\"missing\","
                    "\"targets\":[{\"id\":\"\",\"name\":\"Broken\","
                    "\"position\":[0,0,0],\"rotation\":[0,0,0]},"
                    "{\"id\":\"valid\",\"name\":\"Recovered\","
                    "\"position\":[1,2,3],\"rotation\":[450,-540,720]},"
                    "{\"id\":\"valid\",\"name\":\"Duplicate\","
                    "\"position\":[4,5,6],\"rotation\":[0,0,0]}]}"));
    PoseTargetModel recovered;
    okay &= Check(
            recovered.count() == 1 &&
                    recovered.selectedIndex() == 0 &&
                    recovered.selectedTarget()
                                    .value(QStringLiteral("name"))
                                    .toString() ==
                            QStringLiteral("Recovered") &&
                    recovered.selectedTarget()
                                    .value(QStringLiteral("yawDegrees"))
                                    .toString() ==
                            QStringLiteral("90") &&
                    recovered.selectedTarget()
                                    .value(QStringLiteral("pitchDegrees"))
                                    .toString() ==
                            QStringLiteral("-180") &&
                    recovered.selectedTarget()
                                    .value(QStringLiteral("rollDegrees"))
                                    .toString() ==
                            QStringLiteral("0"),
            "pose target persistence did not reject or normalize bad data");

    QSettings().clear();
    SearchController controller;
    controller.setEvaluationTargetId(QStringLiteral("pose-target"));
    PoseTargetModel *const targets = controller.poseTargets();
    targets->setPositionComponent(
            0, QStringLiteral("x"), QStringLiteral("18"));
    targets->setRotationComponent(
            0, QStringLiteral("yaw"), QStringLiteral("75"));
    okay &= Check(
            controller.evaluationTargetSettings()
                            .value(QStringLiteral("x"))
                            .toString() == QStringLiteral("18") &&
                    controller.evaluationTargetSettings()
                                    .value(QStringLiteral("yawDegrees"))
                                    .toString() ==
                            QStringLiteral("75"),
            "selected pose target did not synchronize search settings");
    controller.setEvaluationTargetSetting(
            QStringLiteral("rollDegrees"), QStringLiteral("-35"));
    okay &= Check(
            targets->selectedTarget()
                            .value(QStringLiteral("rollDegrees"))
                                    .toString() == QStringLiteral("-35"),
            "pose search setting did not synchronize the selected target");
    const int alternateIndex = targets->addTarget(
            -8.0,
            4.0,
            6.0,
            QQuaternion::fromEulerAngles(15.0F, 25.0F, 35.0F));
    okay &= Check(
            alternateIndex == 1 &&
                    controller.evaluationTargetSettings()
                                    .value(QStringLiteral("x"))
                                    .toString() ==
                            QStringLiteral("-8") &&
                    std::abs(
                            controller.evaluationTargetSettings()
                                            .value(
                                                    QStringLiteral(
                                                            "yawDegrees"))
                                            .toDouble() -
                            35.0) < 0.001 &&
                    targets->selectTarget(0) &&
                    controller.evaluationTargetSettings()
                                    .value(QStringLiteral("x"))
                                    .toString() ==
                            QStringLiteral("18") &&
                    controller.evaluationTargetSettings()
                                    .value(QStringLiteral("rollDegrees"))
                                    .toString() ==
                            QStringLiteral("-35"),
            "pose target selection did not switch the brute-force goal");
    return okay;
}

QVariantMap Pass(const SearchController &controller, int index) {
    return controller.modifierPasses().at(index).toMap();
}

QString PassId(const SearchController &controller, int index) {
    return Pass(controller, index).value(QStringLiteral("id")).toString();
}

QVariantMap PassSettings(const SearchController &controller, int index) {
    return Pass(controller, index)
            .value(QStringLiteral("settings"))
            .toMap();
}

void SetValidPaths(SearchController &controller,
                   const QString &packsDirectory,
                   const QString &replayPath) {
    controller.setPacksDirectory(packsDirectory);
    controller.setReplayPath(replayPath);
}

bool TestScenarioInputExtractionAvailability(
        const QString &packsDirectory,
        const QString &replayPath) {
    const QString challengePath = QDir(packsDirectory).filePath(
            QStringLiteral("map.Challenge.Gbx"));
    QFile challenge(challengePath);
    if (!challenge.open(QIODevice::WriteOnly)) {
        return Check(false, "failed to create challenge path fixture");
    }
    challenge.write("test");
    challenge.close();

    QSettings().clear();
    SearchController controller;
    SetValidPaths(controller, packsDirectory, replayPath);
    bool okay = Check(controller.canExtractReplayInputs(),
                      "replay did not offer explicit input extraction");
    controller.setReplayPath(challengePath);
    okay &= Check(!controller.canExtractReplayInputs(),
                  "standalone challenge offered replay input extraction");
    return okay;
}

bool TestUserTimelineConfigurationBoundary() {
    QSettings().clear();
    forevertas::app::SearchConfigurationModel configuration;
    bool okay = Check(
            configuration.setModifierPassId(
                    0, QStringLiteral("smooth-steering")),
            "failed to select a duration-bearing modifier");
    okay &= Check(configuration.setModifierPassSetting(
                              0,
                              QStringLiteral("minTimeMs"),
                              QStringLiteral("0")) &&
                          configuration.setModifierPassSetting(
                                  0,
                                  QStringLiteral("maxTimeMs"),
                                  QStringLiteral("20")) &&
                          configuration.setModifierPassSetting(
                                  0,
                                  QStringLiteral("radiusMs"),
                                  QStringLiteral("210")),
                  "failed to configure user timeline modifier values");
    okay &= Check(configuration.setEvaluationTargetSetting(
                              QStringLiteral("minTimeMs"),
                              QStringLiteral("0")) &&
                          configuration.setEvaluationTargetSetting(
                                  QStringLiteral("maxTimeMs"),
                                  QStringLiteral("20")),
                  "failed to configure user timeline evaluation values");

    const auto validated = configuration.validate(
            10u, forevertas::kDefaultSimulationHorizonMs);
    okay &= Check(validated.configuration.has_value() &&
                          validated.error.isEmpty(),
                  "zero-based user timeline settings did not validate");
    if (!validated.configuration) return false;

    const QVariantMap userModifier =
            configuration.modifierPasses().front().toMap()
                    .value(QStringLiteral("settings"))
                    .toMap();
    const QVariantMap userEvaluation =
            configuration.evaluationTargetSettings();
    const forevertas::OptionSettings &configuredModifier =
            validated.configuration->modifiers.front().settings;
    const forevertas::OptionSettings &configuredEvaluation =
            validated.configuration->evaluationTarget.settings;
    okay &= Check(userModifier.value(QStringLiteral("minTimeMs")).toString() ==
                                  QStringLiteral("0") &&
                          userModifier.value(QStringLiteral("maxTimeMs"))
                                          .toString() ==
                                  QStringLiteral("20") &&
                          userModifier.value(QStringLiteral("radiusMs"))
                                          .toString() ==
                                  QStringLiteral("210") &&
                          userEvaluation.value(QStringLiteral("minTimeMs"))
                                          .toString() ==
                                  QStringLiteral("0") &&
                          userEvaluation.value(QStringLiteral("maxTimeMs"))
                                          .toString() ==
                                  QStringLiteral("20"),
                  "validation rewrote persisted user timeline values");
    okay &= Check(configuredModifier.at("minTimeMs") == "0" &&
                          configuredModifier.at("maxTimeMs") == "20" &&
                          configuredModifier.at("radiusMs") == "210" &&
                          configuredEvaluation.at("minTimeMs") == "0" &&
                          configuredEvaluation.at("maxTimeMs") == "20",
                  "validated configuration did not preserve user timeline values");

    const auto *const modifierRegistration = forevertas::FindModifier(
            validated.configuration->modifiers.front().id);
    const auto *const evaluationRegistration =
            forevertas::FindEvaluationTarget(
                    validated.configuration->evaluationTarget.id);
    okay &= Check(modifierRegistration != nullptr &&
                          evaluationRegistration != nullptr,
                  "validated configuration referenced an unknown component");
    if (modifierRegistration == nullptr || evaluationRegistration == nullptr) {
        return false;
    }
    const std::unique_ptr<forevertas::InputMutator> modifier =
            modifierRegistration->create(configuredModifier, 10u);
    const std::unique_ptr<forevertas::IterationEvaluator> evaluator =
            evaluationRegistration->create(configuredEvaluation, 10u);
    const forevertas::EvaluationPlan plan = evaluator->Plan(
            1000, modifier->EarliestMutationTimeMs(), 10u);
    okay &= Check(modifier->EarliestMutationTimeMs() == 10 &&
                          plan.startTimeMs == 10 && plan.endTimeMs == 20,
                  "registry did not limit the one-tick offset to input "
                  "settings");
    return okay;
}

bool TestRegistryAndValidation(const QString &packsDirectory,
                               const QString &replayPath) {
    QSettings().clear();
    SearchController controller;
    SetValidPaths(controller, packsDirectory, replayPath);

    bool okay = Check(controller.canStart(),
                      "valid defaults and paths did not enable Start");
    okay &= Check(controller.baseInputScript().isEmpty() &&
                          controller.baseInputScriptError().isEmpty(),
                  "empty base input script was not valid by default");
    controller.setBaseInputScript(
            QStringLiteral("0.00 press up\n0.20 steer 32768"));
    okay &= Check(controller.canStart() &&
                          controller.baseInputScriptError().isEmpty(),
                  "valid base input script disabled Start");
    controller.setBaseInputScript(QStringLiteral("0.001 press up"));
    okay &= Check(!controller.canStart() &&
                          controller.baseInputScriptError().contains(
                                  QStringLiteral("Line 1")),
                  "invalid base input script did not disable Start");
    okay &= Check(controller.canUndoBaseInputScript() &&
                          controller.undoBaseInputScript() &&
                          controller.baseInputScript() ==
                                  QStringLiteral(
                                          "0.00 press up\n0.20 steer 32768") &&
                          controller.baseInputScriptError().isEmpty(),
                  "script undo did not restore a non-manual replacement");
    controller.setBaseInputScript(QStringLiteral("0.001 press up"));
    controller.setBaseInputScript({});
    okay &= Check(controller.canStart(),
                  "empty base input script did not restore Start");
#if FOREVERVALIDATOR_HAS_CUDA
    constexpr qsizetype expectedBackendCount = 4;
#else
    constexpr qsizetype expectedBackendCount = 3;
#endif
    okay &= Check(controller.simulationBackendOptions().size() ==
                          expectedBackendCount,
                  "unexpected physics backend count");
    okay &= Check(controller.simulationBackendId() ==
                          QStringLiteral("reference"),
                  "Reference was not the default physics backend");
    okay &= Check(controller.simulationHorizonMs() ==
                          QStringLiteral("6000"),
                  "Simulation horizon did not default to 6000 ms");
    controller.setSimulationHorizonMs(QStringLiteral("5999"));
    okay &= Check(!controller.canStart() &&
                          controller.validationMessage().contains(
                                  QStringLiteral("Simulation horizon")),
                  "unaligned Simulation horizon enabled Start");
    controller.setSimulationHorizonMs(QStringLiteral("6000"));
    controller.setModifierPassSetting(
            0, QStringLiteral("maxTimeMs"), QStringLiteral("6000"));
    okay &= Check(controller.canStart() &&
                          !controller.validationMessage().contains(
                                  QStringLiteral("maps to simulation time")),
                  "modifier time beyond the horizon was not silently clamped");
    controller.setModifierPassSetting(
            0, QStringLiteral("maxTimeMs"), QStringLiteral("4990"));
    controller.setSimulationHorizonMs(QStringLiteral("5000"));
    okay &= Check(!controller.canStart() &&
                          controller.validationMessage().contains(
                                  QStringLiteral("Evaluation maximum time")),
                  "evaluation window beyond the Simulation horizon was accepted");
    controller.setSimulationHorizonMs(QStringLiteral("6010"));
    okay &= Check(controller.canStart(),
                  "valid Simulation horizon did not enable Start");
    controller.setModifierPassSetting(
            0, QStringLiteral("maxTimeMs"), QStringLiteral("5990"));
    controller.setSimulationHorizonMs(QStringLiteral("6000"));
    controller.setConditionScript(QStringLiteral("iterations > 0"));
    okay &= Check(controller.canStart() &&
                          QSettings().value(QStringLiteral(
                                  "search/conditionScript")) ==
                                  QStringLiteral("iterations > 0"),
                  "valid condition script was not accepted and persisted");
    controller.setConditionScript(QStringLiteral(
            "not_a_condition_variable = 1"));
    okay &= Check(!controller.canStart() &&
                          controller.validationMessage().contains(
                                  QStringLiteral("Condition line 1")),
                  "invalid condition script did not disable Start");
    controller.setConditionScript({});
    okay &= Check(controller.canStart(),
                  "clearing the condition script did not restore Start");
    okay &= Check(HasBackendOption(
                          controller.simulationBackendOptions(),
                          QStringLiteral("reference"),
                          QStringLiteral("Reference"),
                          QStringLiteral("Broadest compatibility")) &&
                          HasBackendOption(
                                  controller.simulationBackendOptions(),
                                  QStringLiteral("optimized-cpu"),
                                  QStringLiteral("CPU Optimized"),
                                  QStringLiteral(
                                          "Faster runtime optimized for "
                                          "Stadium, may break compatibility "
                                          "in other environments")) &&
                          HasBackendOption(
                                  controller.simulationBackendOptions(),
                                  QStringLiteral("multi-threaded-cpu"),
                                  QStringLiteral("CPU Multi-threaded"),
                                  QStringLiteral(
                                          "Runs independent optimized CPU "
                                          "simulations across multiple "
                                          "worker threads")),
                  "physics backend metadata was not exposed");
    okay &= Check(
            controller.cpuWorkerCount() ==
                    QString::number(forevertas::DefaultCpuWorkerCount()),
            "unexpected default CPU worker count");
#if FOREVERVALIDATOR_HAS_CUDA
    okay &= Check(HasBackendOption(
                          controller.simulationBackendOptions(),
                          QStringLiteral("cuda"),
                          QStringLiteral("CUDA"),
                          QStringLiteral(
                                  "Fastest runtime optimized for Stadium, "
                                  "needs a modern NVIDIA GPU and may break "
                                  "compatibility in other environments")),
                  "CUDA metadata was not exposed");
#endif
    okay &= Check(controller.cudaParallelSampleCount() ==
                          QString::number(
                                  forevertas::kDefaultCudaParallelSampleCount),
                  "unexpected default CUDA parallel sample count");
    okay &= Check(!controller.cudaCalibrationEnabled(),
                  "CUDA calibration was unexpectedly enabled by default");
    okay &= Check(
            controller.cudaCalibrationStartSampleCount() ==
                    QString::number(
                            forevertas::
                                    kDefaultCudaCalibrationStartSampleCount),
            "unexpected default CUDA calibration starting sample count");
    okay &= Check(
            controller.cudaActiveCalibrationBatchSampleCount().isEmpty(),
            "inactive CUDA calibration exposed a stale active batch");
    okay &= Check(controller.cudaActiveBatchSampleCount().isEmpty(),
                  "inactive search exposed a stale CUDA batch");
    okay &= Check(controller.cudaSessionSpecializationEnabled(),
                  "CUDA fast mode was not enabled by default");
    okay &= Check(controller.searchAlgorithmOptions().size() == 1,
                  "unexpected search algorithm count");
    okay &= Check(
            controller.searchAlgorithmSettings()
                            .value(QStringLiteral("autoPromoteBest"))
                            .toString() == QStringLiteral("false"),
            "auto-promote search mode was unexpectedly enabled by default");
    okay &= Check(controller.modifierOptions().size() == 5,
                  "required modifier options were not exposed");
    okay &= Check(controller.evaluationTargetOptions().size() == 7,
                  "required evaluation targets were not exposed");
    okay &= Check(
            HasOption(controller.modifierOptions(),
                      QStringLiteral("existing-event-perturbation"),
                      QStringLiteral("ExistingEventPerturbationSettings.qml")),
            "existing-event perturbation metadata was not exposed");
    okay &= Check(
            HasOption(controller.modifierOptions(),
                      QStringLiteral("smooth-steering"),
                      QStringLiteral("SmoothSteeringSettings.qml")),
            "smooth steering metadata was not exposed");
    okay &= Check(
            HasOption(controller.modifierOptions(),
                      QStringLiteral("input-insertion"),
                      QStringLiteral("InputInsertionSettings.qml")),
            "input insertion metadata was not exposed");
    okay &= Check(
            HasOption(controller.modifierOptions(),
                      QStringLiteral("input-deletion"),
                      QStringLiteral("InputDeletionSettings.qml")),
            "input deletion metadata was not exposed");
    okay &= Check(
            HasOption(controller.evaluationTargetOptions(),
                      QStringLiteral("precise-finish-time"),
                      QStringLiteral(
                              "PreciseFinishTimeEvaluationSettings.qml")),
            "precise finish target metadata was not exposed");
    okay &= Check(
            HasOption(controller.evaluationTargetOptions(),
                      QStringLiteral("volume-entry-time"),
                      QStringLiteral("VolumeEntryEvaluationSettings.qml")),
            "volume target metadata was not exposed");
    okay &= Check(
            HasOption(controller.evaluationTargetOptions(),
                      QStringLiteral("custom-volume-entry-time"),
                      QStringLiteral("VolumeEntryEvaluationSettings.qml")),
            "custom volume target metadata was not exposed");
    okay &= Check(
            HasOption(controller.evaluationTargetOptions(),
                      QStringLiteral("stunt-points"),
                      QStringLiteral("StuntPointsEvaluationSettings.qml")),
            "stunt points target metadata was not exposed");
    okay &= Check(controller.modifierPasses().size() == 1 &&
                          PassId(controller, 0) ==
                                  QStringLiteral("random-steering"),
                  "default modifier pass was incorrect");

    controller.setSimulationBackendId(QStringLiteral("optimized-cpu"));
    okay &= Check(controller.simulationBackendId() ==
                          QStringLiteral("optimized-cpu") &&
                          controller.canStart(),
                  "CPU Optimized backend was not selectable");
    controller.setSimulationBackendId(
            QStringLiteral("multi-threaded-cpu"));
    okay &= Check(controller.simulationBackendId() ==
                          QStringLiteral("multi-threaded-cpu") &&
                          controller.canStart(),
                  "CPU Multi-threaded backend was not selectable");
    controller.setCpuWorkerCount(QStringLiteral("0"));
    okay &= Check(!controller.canStart(),
                  "zero CPU workers enabled Start");
    controller.setCpuWorkerCount(QStringLiteral("257"));
    okay &= Check(!controller.canStart(),
                  "excessive CPU workers enabled Start");
    controller.setCpuWorkerCount(QStringLiteral("2"));
    okay &= Check(controller.canStart(),
                  "valid CPU worker count did not enable Start");
    controller.setSimulationBackendId(QStringLiteral("optimized-cpu"));
#if FOREVERVALIDATOR_HAS_CUDA
    controller.setSimulationBackendId(QStringLiteral("cuda"));
    okay &= Check(controller.simulationBackendId() ==
                          QStringLiteral("cuda") &&
                          controller.canStart(),
                  "CUDA backend was not selectable");
    controller.setCudaParallelSampleCount(QStringLiteral("0"));
    okay &= Check(!controller.canStart(),
                  "zero CUDA parallel samples enabled Start");
    controller.setCudaParallelSampleCount(QStringLiteral("8192"));
    okay &= Check(controller.canStart(),
                  "CUDA batch size above 4096 did not enable Start");
    controller.setCudaParallelSampleCount(QStringLiteral("4294967296"));
    okay &= Check(!controller.canStart(),
                  "unrepresentable CUDA parallel sample count enabled Start");
    controller.setCudaCalibrationEnabled(true);
    okay &= Check(controller.cudaCalibrationEnabled() &&
                          controller.canStart(),
                  "CUDA calibration depended on the manual sample count");
    controller.setCudaCalibrationStartSampleCount(QStringLiteral("0"));
    okay &= Check(!controller.canStart(),
                  "zero CUDA calibration starting samples enabled Start");
    controller.setCudaCalibrationStartSampleCount(
            QStringLiteral("150000"));
    okay &= Check(controller.canStart(),
                  "valid CUDA calibration starting samples disabled Start");
    controller.setCudaCalibrationEnabled(false);
    okay &= Check(!controller.canStart(),
                  "manual CUDA mode ignored its invalid sample count");
    controller.setCudaParallelSampleCount(QStringLiteral("512"));
    controller.setCudaSessionSpecializationEnabled(false);
    okay &= Check(!controller.cudaSessionSpecializationEnabled() &&
                          controller.canStart(),
                  "regular CUDA mode was not selectable");
    controller.setCudaSessionSpecializationEnabled(true);
    okay &= Check(controller.cudaSessionSpecializationEnabled() &&
                          controller.canStart(),
                  "CUDA fast mode was not selectable");
    controller.setSimulationBackendId(QStringLiteral("optimized-cpu"));
#endif
    controller.setSimulationBackendId(QStringLiteral("missing-backend"));
    okay &= Check(controller.simulationBackendId() ==
                          QStringLiteral("optimized-cpu"),
                  "invalid physics backend changed the selection");
    controller.setSimulationBackendId(QStringLiteral("reference"));


    controller.setModifierPassSetting(
            0, QStringLiteral("seed"), QStringLiteral("4294967296"));
    okay &= Check(!controller.canStart(),
                  "modifier seed overflow enabled Start");
    controller.setModifierPassSetting(
            0, QStringLiteral("seed"), QStringLiteral("123"));

    controller.setModifierPassSetting(
            0, QStringLiteral("minTimeMs"), QStringLiteral("1001"));
    okay &= Check(!controller.canStart(),
                  "unaligned modifier time enabled Start");
    controller.setModifierPassSetting(
            0, QStringLiteral("minTimeMs"), QStringLiteral("1000"));

    controller.setEvaluationTargetSetting(
            QStringLiteral("minTimeMs"), QStringLiteral("1001"));
    okay &= Check(!controller.canStart(),
                  "unaligned evaluation time enabled Start");
    controller.setEvaluationTargetSetting(
            QStringLiteral("minTimeMs"), QStringLiteral("1000"));

    controller.setEvaluationTargetId(QStringLiteral("finish-time"));
    okay &= Check(
            controller.evaluationTargetId() ==
                    QStringLiteral("precise-finish-time"),
            "legacy finish target ID did not migrate to precise finish");
    controller.setEvaluationTargetId(
            QStringLiteral("precise-finish-time"));
    okay &= Check(controller.canStart(),
                  "precise finish target defaults did not validate");
    controller.setEvaluationTargetId(QStringLiteral("stunt-points"));
    okay &= Check(
            controller.canStart() &&
                    controller.evaluationTargetSettings()
                                    .value(QStringLiteral("targetTimeMs"))
                                    .toString() == QStringLiteral("6000"),
            "stunt target defaults did not validate");
    controller.setEvaluationTargetSetting(
            QStringLiteral("targetTimeMs"), QStringLiteral("6001"));
    okay &= Check(!controller.canStart(),
                  "unaligned stunt target time enabled Start");
    controller.setEvaluationTargetSetting(
            QStringLiteral("targetTimeMs"), QStringLiteral("4320"));
    okay &= Check(controller.canStart(),
                  "valid stunt target time did not enable Start");
    controller.setEvaluationTargetSetting(
            QStringLiteral("targetTimeMs"), QStringLiteral("500"));
    okay &= Check(
            !controller.canStart() &&
                    controller.validationMessage().contains(
                            QStringLiteral("first modifier time")),
            "stunt target accepted a deadline before any mutation");
    controller.setEvaluationTargetSetting(
            QStringLiteral("targetTimeMs"), QStringLiteral("4320"));
    controller.setEvaluationTargetId(QStringLiteral("missing-target"));
    okay &= Check(!controller.canStart(),
                  "unknown evaluation target enabled Start");
    controller.setEvaluationTargetId(QStringLiteral("velocity"));
    okay &= Check(controller.canStart(),
                  "restored valid target did not enable Start");
    return okay;
}

bool TestCompositionEditing(const QString &packsDirectory,
                            const QString &replayPath) {
    QSettings().clear();
    SearchController controller;
    SetValidPaths(controller, packsDirectory, replayPath);

    controller.addModifierPass(QStringLiteral("input-deletion"));
    bool okay = Check(controller.modifierPasses().size() == 2,
                      "modifier pass was not added");
    controller.setModifierPassSetting(
            1, QStringLiteral("steerMaxCount"), QStringLiteral("4"));
    okay &= Check(PassSettings(controller, 1)
                                  .value(QStringLiteral("steerMaxCount"))
                                  .toString() == QStringLiteral("4"),
                  "pass-owned setting was not changed");

    controller.moveModifierPass(1, 0);
    okay &= Check(PassId(controller, 0) ==
                          QStringLiteral("input-deletion") &&
                          PassId(controller, 1) ==
                                  QStringLiteral("random-steering"),
                  "modifier pass order did not change");

    controller.setModifierPassId(1, QStringLiteral("smooth-steering"));
    okay &= Check(PassId(controller, 1) ==
                          QStringLiteral("smooth-steering") &&
                          PassSettings(controller, 1).contains(
                                  QStringLiteral("radiusMs")),
                  "modifier pass type did not replace its settings");

    controller.removeModifierPass(1);
    controller.removeModifierPass(0);
    okay &= Check(controller.modifierPasses().isEmpty(),
                  "modifier passes were not removed");
    okay &= Check(!controller.canStart(),
                  "empty modifier composition enabled Start");
    controller.addModifierPass(QStringLiteral("random-steering"));
    okay &= Check(controller.canStart(),
                  "restored modifier composition did not enable Start");
    return okay;
}

bool TestPersistence(const QString &packsDirectory,
                     const QString &replayPath) {
    QSettings().clear();
    {
        SearchController controller;
        if (!Check(!controller.darkMode(),
                   "light appearance was not the default theme")) {
            return false;
        }
        SetValidPaths(controller, packsDirectory, replayPath);
        controller.setModifierPassSetting(
                0, QStringLiteral("seed"), QStringLiteral("321"));
        controller.addModifierPass(QStringLiteral("input-deletion"));
        controller.setModifierPassSetting(
                1, QStringLiteral("steerMaxCount"), QStringLiteral("4"));
        controller.moveModifierPass(1, 0);
        controller.setSimulationBackendId(QStringLiteral("optimized-cpu"));
        controller.setCpuWorkerCount(QStringLiteral("6"));
        controller.setCudaParallelSampleCount(QStringLiteral("384"));
        controller.setCudaCalibrationEnabled(true);
        controller.setCudaCalibrationStartSampleCount(
                QStringLiteral("150000"));
        controller.setCudaSessionSpecializationEnabled(false);
        controller.setDarkMode(true);
        controller.setSearchAlgorithmSetting(
                QStringLiteral("autoPromoteBest"),
                QStringLiteral("true"));
        controller.setEvaluationTargetId(QStringLiteral("point-target"));
        controller.setEvaluationTargetSetting(
                QStringLiteral("x"), QStringLiteral("12.5"));
        controller.setEvaluationTargetId(QStringLiteral("stunt-points"));
        controller.setEvaluationTargetSetting(
                QStringLiteral("targetTimeMs"), QStringLiteral("4320"));
        controller.setBaseInputScript(
                QStringLiteral("0.00 press up\n0.50 steer -16384"));
        QSettings().sync();
    }

    SearchController restored;
    bool okay = Check(
            restored.searchAlgorithmSettings()
                            .value(QStringLiteral("autoPromoteBest"))
                            .toString() == QStringLiteral("true"),
            "auto-promote search mode was not persisted");
    okay &= Check(
            restored.baseInputScript() ==
                    QStringLiteral("0.00 press up\n0.50 steer -16384") &&
                    restored.baseInputScriptError().isEmpty(),
            "base input script was not persisted");
    okay &= Check(restored.simulationBackendId() ==
                          QStringLiteral("optimized-cpu"),
                  "physics backend selection was not persisted");
    okay &= Check(restored.cudaParallelSampleCount() ==
                          QStringLiteral("384"),
                  "CUDA parallel sample count was not persisted");
    okay &= Check(restored.cpuWorkerCount() == QStringLiteral("6"),
                  "CPU worker count was not persisted");
    okay &= Check(restored.cudaCalibrationEnabled(),
                  "CUDA calibration mode was not persisted");
    okay &= Check(
            restored.cudaCalibrationStartSampleCount() ==
                    QStringLiteral("150000"),
            "CUDA calibration starting sample count was not persisted");
    okay &= Check(!restored.cudaSessionSpecializationEnabled(),
                  "CUDA fast mode selection was not persisted");
    okay &= Check(restored.darkMode(),
                  "dark appearance mode was not persisted");
    QSignalSpy darkModeSpy(&restored, &SearchController::darkModeChanged);
    restored.setDarkMode(false);
    restored.setDarkMode(false);
    okay &= Check(!restored.darkMode() && darkModeSpy.count() == 1 &&
                          !QSettings()
                                   .value(QStringLiteral(
                                           "appearance/darkMode"))
                                   .toBool(),
                  "dark appearance mode did not update atomically");
    okay &= Check(restored.modifierPasses().size() == 2,
                  "modifier pass count was not persisted");
    okay &= Check(PassId(restored, 0) == QStringLiteral("input-deletion") &&
                          PassSettings(restored, 0)
                                          .value(QStringLiteral(
                                                  "steerMaxCount"))
                                          .toString() == QStringLiteral("4"),
                  "first modifier pass was not persisted");
    okay &= Check(PassId(restored, 1) == QStringLiteral("random-steering") &&
                          PassSettings(restored, 1)
                                          .value(QStringLiteral("seed"))
                                          .toString() == QStringLiteral("321"),
                  "second modifier pass was not persisted");
    okay &= Check(restored.evaluationTargetId() ==
                          QStringLiteral("stunt-points") &&
                          restored.evaluationTargetSettings()
                                          .value(QStringLiteral("targetTimeMs"))
                                          .toString() == QStringLiteral("4320"),
                  "evaluation target configuration was not persisted");
    okay &= Check(QSettings().contains(
                          QStringLiteral("composition/modifiers")),
                  "modifier composition JSON was not persisted");
    okay &= Check(QSettings().value(
                                  QStringLiteral(
                                          "selection/simulationBackend"))
                                  .toString() ==
                          QStringLiteral("optimized-cpu"),
                  "physics backend setting was not stored canonically");
    okay &= Check(QSettings().value(QStringLiteral(
                                  "backends/cuda/parallelSampleCount"))
                                  .toString() == QStringLiteral("384"),
                  "CUDA parallel sample count was not stored canonically");
    okay &= Check(QSettings().value(QStringLiteral(
                                  "backends/cpu/workerCount"))
                                  .toString() == QStringLiteral("6"),
                  "CPU worker count was not stored canonically");
    okay &= Check(QSettings().value(QStringLiteral(
                                  "backends/cuda/calibrationEnabled"))
                                  .toBool(),
                  "CUDA calibration mode was not stored canonically");
    okay &= Check(
            QSettings()
                            .value(QStringLiteral(
                                    "backends/cuda/"
                                    "calibrationStartSampleCount"))
                            .toString() == QStringLiteral("150000"),
            "CUDA calibration starting sample count was not stored "
            "canonically");
    okay &= Check(!QSettings().value(QStringLiteral(
                                  "backends/cuda/sessionSpecializationEnabled"))
                                   .toBool(),
                  "CUDA fast mode was not stored canonically");
    return okay;
}

bool TestExtractionFailurePreservesDraft(const QString &packsDirectory,
                                         const QString &replayPath) {
    QSettings().clear();
    SearchController controller;
    SetValidPaths(controller, packsDirectory, replayPath);
    const QString draft =
            QStringLiteral("0.00 press up\n0.50 steer -16384");
    controller.setBaseInputScript(draft);
    controller.extractReplayInputs();
    const bool finished = WaitUntil(
            [&controller]() {
                return !controller.extractingReplayInputs();
            },
            5000);
    return Check(
            finished &&
                    controller.baseInputScript() == draft &&
                    controller.replayInputStatusText().startsWith(
                            QStringLiteral("Input extraction failed:")),
            "failed extraction replaced the existing base script");
}

bool TestExtractionWorkerShutdown(const QString &packsDirectory,
                                  const QString &replayPath) {
    QElapsedTimer elapsed;
    elapsed.start();
    {
        SearchController controller;
        SetValidPaths(controller, packsDirectory, replayPath);
        controller.extractReplayInputs();
    }
    return Check(elapsed.elapsed() < 5000,
                 "input extraction worker did not stop during shutdown");
}

bool TestDescriptiveSearchStageStatuses() {
    using forevertas::SearchProgressStage;
    using forevertas::app::SearchStageStatus;

    bool okay = Check(
            SearchStageStatus(
                    SearchProgressStage::OpeningPacksDirectory,
                    "reference") ==
                    QStringLiteral("Opening Packs directory..."),
            "Packs loading stage was not descriptive");
    okay &= Check(
            SearchStageStatus(
                    SearchProgressStage::ReadingScenario,
                    "reference") ==
                    QStringLiteral("Reading scenario file..."),
            "replay reading stage was not descriptive");
    okay &= Check(
            SearchStageStatus(
                    SearchProgressStage::CreatingSimulation,
                    "optimized-cpu")
                    .contains(QStringLiteral("optimized CPU")),
            "optimized CPU initialization was not identified");
    okay &= Check(
            SearchStageStatus(
                    SearchProgressStage::PreparingSearch,
                    "multi-threaded-cpu")
                            .contains(QStringLiteral(
                                    "independent optimized CPU workers")) &&
                    SearchStageStatus(
                            SearchProgressStage::Mutations,
                            "multi-threaded-cpu")
                            .contains(QStringLiteral(
                                    "across optimized CPU workers")),
            "multi-threaded CPU stages did not identify worker aggregation");
    const QString cudaInitialization = SearchStageStatus(
            SearchProgressStage::CreatingSimulation,
            "cuda");
    const QString cudaReplayLoadRegular = SearchStageStatus(
            SearchProgressStage::LoadingScenario,
            "cuda",
            false);
    const QString cudaReplayLoadFast = SearchStageStatus(
            SearchProgressStage::LoadingScenario,
            "cuda",
            true);
    const QString cudaBaseline = SearchStageStatus(
            SearchProgressStage::Baseline,
            "cuda");
    const QString cudaCalibration = SearchStageStatus(
            SearchProgressStage::Calibration,
            "cuda");
    const QString cudaMutations = SearchStageStatus(
            SearchProgressStage::Mutations,
            "cuda");
    okay &= Check(
            cudaInitialization.contains(QStringLiteral("CUDA")) &&
                    cudaReplayLoadRegular ==
                            QStringLiteral("Loading the map onto CUDA...") &&
                    cudaReplayLoadFast.contains(
                            QStringLiteral("building the fast CUDA kernel")) &&
                    cudaBaseline.contains(QStringLiteral("CUDA baseline")) &&
                    cudaCalibration.contains(
                            QStringLiteral("CUDA throughput")) &&
                    cudaMutations.contains(QStringLiteral("Searching on CUDA")) &&
                    !cudaInitialization.contains(
                            QStringLiteral("GPU availability")) &&
                    !cudaReplayLoadRegular.contains(
                            QStringLiteral("building")) &&
                    !cudaReplayLoadRegular.contains(
                            QStringLiteral("GPU availability")),
            "CUDA stages did not describe the actual work clearly");
    okay &= Check(
            SearchStageStatus(
                    SearchProgressStage::FinalSamplingSetup,
                    "reference")
                    .contains(QStringLiteral("final best-run sampling")),
            "final sampling setup was not identified");
    return okay;
}

bool TestIterationBoundaryArbitration() {
    using forevertas::SearchIterationPhase;
    using forevertas::app::TryBeginSearchIteration;
    using forevertas::app::TryCancelBeforeSearchIteration;

    auto cancelled =
            std::make_shared<std::atomic<SearchIterationPhase>>(
                    SearchIterationPhase::Pending);
    bool okay = Check(
            TryCancelBeforeSearchIteration(cancelled) &&
                    !TryBeginSearchIteration(cancelled) &&
                    cancelled->load(std::memory_order_acquire) ==
                            SearchIterationPhase::Cancelled,
            "a pre-iteration cancellation did not win the boundary");

    auto started =
            std::make_shared<std::atomic<SearchIterationPhase>>(
                    SearchIterationPhase::Pending);
    okay &= Check(
            TryBeginSearchIteration(started) &&
                    !TryCancelBeforeSearchIteration(started) &&
                    TryBeginSearchIteration(started) &&
                    started->load(std::memory_order_acquire) ==
                            SearchIterationPhase::Started,
            "a started iteration did not retain the boundary");
    return okay;
}

bool TestStopAbortsBeforeFirstIteration(const QString &packsDirectory,
                                        const QString &replayPath) {
    QSettings().clear();
    SearchController controller;
    SetValidPaths(controller, packsDirectory, replayPath);
#if FOREVERVALIDATOR_HAS_CUDA
    controller.setSimulationBackendId(QStringLiteral("cuda"));
    controller.setCudaParallelSampleCount(QStringLiteral("777"));
    controller.setCudaCalibrationEnabled(true);
    controller.setCudaCalibrationStartSampleCount(QStringLiteral("3"));
    QSignalSpy activeBatchSpy(
            &controller,
            &SearchController::cudaActiveCalibrationBatchSampleCountChanged);
    QSignalSpy cudaBatchSpy(
            &controller,
            &SearchController::cudaActiveBatchSampleCountChanged);
#endif
    QSignalSpy completionSpy(
            &controller, &SearchController::searchCompleted);

    QElapsedTimer elapsed;
    elapsed.start();
    controller.startSearch();
#if FOREVERVALIDATOR_HAS_CUDA
    bool okay = Check(
            controller.cudaActiveCalibrationBatchSampleCount().isEmpty() &&
                    controller.cudaActiveBatchSampleCount().isEmpty() &&
                    controller.liveMetricsVisible() &&
                    !controller.iterationCountText().isEmpty() &&
                    !controller.elapsedText().isEmpty() &&
                    !controller.evaluationCountText().isEmpty() &&
                    !controller.mutationCountText().isEmpty() &&
                    !controller.improvementCountText().isEmpty() &&
                    controller.cudaParallelSampleCount() ==
                            QStringLiteral("777") &&
                    QSettings()
                                    .value(QStringLiteral(
                                            "backends/cuda/"
                                            "parallelSampleCount"))
                                    .toString() == QStringLiteral("777"),
            "CUDA startup did not immediately expose activity metrics or "
            "overwrote the saved manual batch");
#else
    bool okay = true;
#endif
    controller.stopSearch();
    okay &= Check(
            controller.running() && controller.stopping() &&
                    controller.statusText() ==
                            QStringLiteral("Aborting search startup..."),
            "Stop did not request an immediate startup abort");
    okay &= Check(
            WaitUntil([&controller]() { return !controller.running(); }, 5000),
            "startup abort did not terminate promptly");
    okay &= Check(
            elapsed.elapsed() < 5000 &&
                    controller.statusText() ==
                            QStringLiteral("Search aborted") &&
                    completionSpy.isEmpty() &&
                    controller.resultText().isEmpty(),
            "startup abort ran or completed a search iteration");
#if FOREVERVALIDATOR_HAS_CUDA
    okay &= Check(
            controller.cudaActiveCalibrationBatchSampleCount().isEmpty() &&
                    controller.cudaActiveBatchSampleCount().isEmpty() &&
                    controller.cudaParallelSampleCount() ==
                            QStringLiteral("777") &&
                    activeBatchSpy.isEmpty() &&
                    cudaBatchSpy.isEmpty(),
            "ending calibration did not clear only the transient CUDA batch");
#endif
    return okay;
}

bool TestLocaleIndependentPersistedDecimals(const QString &packsDirectory,
                                             const QString &replayPath) {
    NumericLocaleGuard locale;
    if (!locale.ActivateCommaDecimalLocale()) {
        return Check(false, "no comma-decimal locale is installed for testing");
    }

    QSettings().clear();
    {
        SearchController controller;
        SetValidPaths(controller, packsDirectory, replayPath);
        controller.setEvaluationTargetId(QStringLiteral("point-target"));
        controller.setEvaluationTargetSetting(
                QStringLiteral("x"), QStringLiteral("12.5"));
        controller.setEvaluationTargetSetting(
                QStringLiteral("y"), QStringLiteral("-3.25"));
        controller.setModifierPassId(
                0, QStringLiteral("existing-event-perturbation"));
        controller.setModifierPassSetting(
                0, QStringLiteral("steerDeltaMin"), QStringLiteral("-0.25"));
        controller.setModifierPassSetting(
                0, QStringLiteral("steerDeltaMax"), QStringLiteral("0.25"));

        bool okay = Check(
                controller.canStart(),
                "UI-entered dot decimals failed under comma LC_NUMERIC");
        controller.setEvaluationTargetSetting(
                QStringLiteral("x"), QStringLiteral("12,5"));
        okay &= Check(!controller.canStart(),
                      "UI-entered comma decimal was accepted");
        controller.setEvaluationTargetSetting(
                QStringLiteral("x"), QStringLiteral("12.5"));
        okay &= Check(controller.canStart(),
                      "restoring a dot decimal did not restore validation");
        if (!okay) return false;
        QSettings().sync();
    }

    SearchController restored;
    SetValidPaths(restored, packsDirectory, replayPath);
    bool okay = Check(
            restored.canStart(),
            "persisted dot decimals failed under comma LC_NUMERIC");
    okay &= Check(
            restored.evaluationTargetId() == QStringLiteral("point-target") &&
                    restored.evaluationTargetSettings()
                                    .value(QStringLiteral("x"))
                                    .toString() == QStringLiteral("12.5") &&
                    restored.evaluationTargetSettings()
                                    .value(QStringLiteral("y"))
                                    .toString() == QStringLiteral("-3.25"),
            "persisted evaluation decimals changed representation");
    okay &= Check(
            PassId(restored, 0) ==
                            QStringLiteral("existing-event-perturbation") &&
                    PassSettings(restored, 0)
                                    .value(QStringLiteral("steerDeltaMin"))
                                    .toString() == QStringLiteral("-0.25") &&
                    PassSettings(restored, 0)
                                    .value(QStringLiteral("steerDeltaMax"))
                                    .toString() == QStringLiteral("0.25"),
            "persisted modifier decimals changed representation");
    return okay;
}

bool TestLegacyMigration() {
    QSettings().clear();
    const QString retiredBudgetKey = QString::fromLatin1(
            QByteArray::fromHex("617474656d7074436f756e74"));
    QSettings().setValue(
            QStringLiteral("search/") + retiredBudgetKey,
            QStringLiteral("1000"));
    QSettings().setValue(QStringLiteral("selection/mutationAlgorithm"),
                         QStringLiteral("random-steering"));
    QSettings().setValue(QStringLiteral("search/minMutateMs"),
                         QStringLiteral("1200"));
    QSettings().setValue(QStringLiteral("search/maxMutateMs"),
                         QStringLiteral("2400"));
    QSettings().setValue(QStringLiteral("search/mutationSeed"),
                         QStringLiteral("987"));
    QSettings().setValue(QStringLiteral("selection/evaluationTarget"),
                         QStringLiteral("maximum-speed"));

    SearchController controller;
    bool okay = Check(
            !QSettings().contains(
                    QStringLiteral("search/") + retiredBudgetKey),
            "retired search budget was not removed");
    okay &= Check(controller.modifierPasses().size() == 1 &&
                              PassId(controller, 0) ==
                                      QStringLiteral("random-steering"),
                      "legacy mutation selection was not migrated");
    okay &= Check(PassSettings(controller, 0)
                                  .value(QStringLiteral("minTimeMs"))
                                  .toString() == QStringLiteral("1200") &&
                          PassSettings(controller, 0)
                                  .value(QStringLiteral("maxTimeMs"))
                                  .toString() == QStringLiteral("2400") &&
                          PassSettings(controller, 0)
                                  .value(QStringLiteral("seed"))
                                  .toString() == QStringLiteral("987"),
                  "legacy modifier settings were not migrated");
    okay &= Check(controller.evaluationTargetId() ==
                          QStringLiteral("velocity") &&
                          QSettings()
                                          .value(QStringLiteral(
                                                  "selection/evaluationTarget"))
                                          .toString() ==
                                  QStringLiteral("velocity"),
                  "legacy evaluation target was not canonicalized");
    okay &= Check(QSettings().contains(
                          QStringLiteral("composition/modifiers")),
                  "migrated modifier composition was not persisted");
    return okay;
}


bool TestIndefiniteSearchLifecycle(const QString &packsDirectory,
                                   const QString &replayPath) {
    QSettings().clear();
    SearchController controller;
    SetValidPaths(controller, packsDirectory, replayPath);
    controller.setSimulationBackendId(QStringLiteral("optimized-cpu"));
    controller.setModifierPassSetting(
            0,
            QStringLiteral("minTimeMs"),
            QStringLiteral("0"));
    controller.setModifierPassSetting(
            0,
            QStringLiteral("maxTimeMs"),
            QStringLiteral("20"));
    controller.setEvaluationTargetSetting(
            QStringLiteral("minTimeMs"),
            QStringLiteral("0"));
    controller.setEvaluationTargetSetting(
            QStringLiteral("maxTimeMs"),
            QStringLiteral("20"));
    const bool zeroOriginConfigured =
            PassSettings(controller, 0)
                            .value(QStringLiteral("minTimeMs"))
                            .toString() == QStringLiteral("0") &&
            PassSettings(controller, 0)
                            .value(QStringLiteral("maxTimeMs"))
                            .toString() == QStringLiteral("20") &&
            controller.evaluationTargetSettings()
                            .value(QStringLiteral("minTimeMs"))
                            .toString() == QStringLiteral("0") &&
            controller.evaluationTargetSettings()
                            .value(QStringLiteral("maxTimeMs"))
                            .toString() == QStringLiteral("20");
    if (!Check(zeroOriginConfigured,
               "failed to configure the zero-based first input")) {
        return false;
    }
    if (!Check(controller.canStart(),
               "real replay configuration did not enable Start")) {
        return false;
    }
    controller.extractReplayInputs();
    if (!Check(
                WaitUntil(
                        [&controller]() {
                            return !controller.extractingReplayInputs();
                        },
                        30000) &&
                        controller.replayInputStatusText() ==
                                QStringLiteral("Replay inputs extracted") &&
                        !controller.baseInputScript().isEmpty() &&
                        controller.baseInputScriptError().isEmpty(),
                "real replay inputs were not extracted into the base script")) {
        return false;
    }

    QSignalSpy completionSpy(
            &controller, &SearchController::searchCompleted);
    QSignalSpy improvementSpy(
            &controller, &SearchController::searchImprovement);
    const QString seedBeforeStart = PassSettings(controller, 0)
            .value(QStringLiteral("seed"))
            .toString();
    controller.startSearch();
    bool okay = Check(controller.running() && !controller.canStart(),
                      "Start did not enter the running state");
    okay &= Check(PassSettings(controller, 0)
                                  .value(QStringLiteral("seed"))
                                  .toString() != seedBeforeStart,
                  "Start did not randomize modifier seeds");
    okay &= Check(
            WaitUntil(
                    [&controller]() {
                        return controller.running() &&
                                controller.statusText() ==
                                        QStringLiteral("Searching...") &&
                                controller.liveMetricsVisible() &&
                                QRegularExpression(QStringLiteral(
                                        "^[0-9]+\\.[0-9]{2}[kMBTQ]?$"))
                                        .match(controller.iterationCountText())
                                        .hasMatch() &&
                                QRegularExpression(QStringLiteral(
                                        "^[0-9]+\\.[0-9]{2}[kMBTQ]?$"))
                                        .match(controller.throughputText())
                                        .hasMatch() &&
                                controller.elapsedText().startsWith(
                                        QStringLiteral("00:")) &&
                                !controller.elapsedText().contains(
                                        QLatin1Char('.')) &&
                                !controller.evaluationCountText().isEmpty() &&
                                !controller.mutationCountText().isEmpty() &&
                                !controller.improvementCountText().isEmpty() &&
                                controller.resultText().contains(
                                        QStringLiteral("Last improvement:")) &&
                                !controller.resultText()
                                         .section(QStringLiteral(
                                                          "Last improvement: "),
                                                  1,
                                                  1)
                                         .section(QLatin1Char('\n'), 0, 0)
                                         .contains(QLatin1Char('.')) &&
                                !controller.resultText().contains(
                                        QStringLiteral("iterations so far")) &&
                                !controller.bestInputsText().isEmpty();
                    },
                    30000),
            "live iteration metrics were not shown while running");
    if (!okay) {
        return false;
    }
    okay &= Check(
            WaitUntil(
                    [&improvementSpy]() {
                        return improvementSpy.count() > 0;
                    },
                    10000),
            "search did not publish a best-run improvement trajectory");
    std::uint64_t searchId = 0u;
    std::uint64_t improvementNumber = 0u;
    for (const QList<QVariant> &arguments : improvementSpy) {
        const auto improvement =
                qvariant_cast<forevertas::app::SearchImprovementPtr>(
                        arguments.at(0));
        const bool complete =
                improvement != nullptr &&
                improvement->searchId != 0u &&
                improvement->improvementNumber > improvementNumber &&
                improvement->packsDirectory == packsDirectory &&
                improvement->replayPath == replayPath &&
                improvement->simulationBackendId ==
                        QStringLiteral("optimized-cpu") &&
                !improvement->timeline.empty() &&
                improvement->timeline.front().timeMs == 0 &&
                improvement->timeline.back().timeMs > 0;
        if (improvement != nullptr) {
            if (searchId == 0u) {
                searchId = improvement->searchId;
            }
            improvementNumber = improvement->improvementNumber;
        }
        okay &= Check(complete && improvement->searchId == searchId,
                      "published improvement trajectory was incomplete");
    }
    if (!okay) {
        controller.stopSearch();
        WaitUntil([&controller]() { return !controller.running(); }, 30000);
        return false;
    }
    const QString firstElapsed = controller.elapsedText();
    okay &= Check(
            WaitUntil(
                    [&controller, &firstElapsed]() {
                        return controller.running() &&
                                controller.elapsedText() != firstElapsed;
                    },
                    5000),
            "live elapsed metric did not refresh without completion");
    if (!okay) {
        return false;
    }

    controller.stopSearch();
    okay &= Check(controller.stopping(),
                  "Stop did not enter the stopping state");
    okay &= Check(
            WaitUntil([&completionSpy]() {
                return completionSpy.count() > 0;
            }, 30000),
            "Stop did not complete final best-run sampling");
    okay &= Check(
            WaitUntil([&controller]() { return !controller.running(); }, 5000),
            "worker did not leave the running state after completion");
    if (completionSpy.isEmpty()) {
        return false;
    }

    const auto completion = qvariant_cast<
            forevertas::app::SearchCompletionPtr>(
            completionSpy.takeFirst().at(0));
    okay &= Check(completion != nullptr &&
                          completion->simulationBackendId ==
                                  QStringLiteral("optimized-cpu") &&
                          !completion->bestInputs.empty() &&
                          !completion->bestTimeline.empty(),
                  "completed search did not retain its backend and best run");
    if (completion && !completion->bestTimeline.empty()) {
        okay &= Check(completion->bestTimeline.front().timeMs == 0 &&
                              completion->bestTimeline.back().timeMs > 0,
                      "final sampling did not cover the replay timeline");
    }
    okay &= Check(!controller.stopping() &&
                          controller.statusText() ==
                                  QStringLiteral("Search complete") &&
                          controller.liveMetricsVisible() &&
                          !controller.iterationCountText().isEmpty() &&
                          !controller.throughputText().isEmpty() &&
                          !controller.elapsedText().isEmpty() &&
                          !controller.evaluationCountText().isEmpty() &&
                          !controller.mutationCountText().isEmpty() &&
                          !controller.improvementCountText().isEmpty() &&
                          !controller.resultText().isEmpty() &&
                          !controller.bestInputsText().isEmpty(),
                  "completed search did not preserve the final best display");
    return okay;
}

bool TestAutomaticPacksDetection() {
    QSettings().clear();
    QTemporaryDir root;
    if (!root.isValid()) return Check(false, "failed to create search root");
    const QString packs = root.filePath(QStringLiteral(
            "Games/prefix/drive_c/Program Files (x86)/"
            "TmUnitedForever/Packs"));
    if (!QDir().mkpath(packs)) {
        return Check(false, "failed to create detected Packs directory");
    }
    QFile packList(QDir(packs).filePath(QStringLiteral("packlist.dat")));
    if (!packList.open(QIODevice::WriteOnly)) {
        return Check(false, "failed to create packlist.dat");
    }
    packList.write("test");
    packList.close();
    const QString pattern = root.filePath(QStringLiteral(
            "Games/*/drive_c/Program Files (x86)/TmUnitedForever/Packs"));
    const QString canonical = QFileInfo(packs).canonicalFilePath();

    SearchController controller(QStringList{pattern});
    QSignalSpy spy(&controller,
                   &SearchController::autoDetectedPacksDirectoryChanged);
    QThread *publicationThread = nullptr;
    QObject::connect(
            &controller,
            &SearchController::autoDetectedPacksDirectoryChanged,
            &controller,
            [&]() { publicationThread = QThread::currentThread(); });
    bool okay = Check(controller.autoDetectedPacksDirectory().isEmpty(),
                      "automatic detection blocked construction");
    okay &= Check(spy.wait(2000),
                  "automatic detection did not publish asynchronously");
    okay &= Check(controller.autoDetectedPacksDirectory() == canonical,
                  "automatic detection proposed the wrong path");
    okay &= Check(publicationThread == controller.thread(),
                  "automatic detection published off the controller thread");
    controller.applyAutoDetectedPacksDirectory();
    okay &= Check(controller.packsDirectory() == canonical &&
                          controller.autoDetectedPacksDirectory().isEmpty(),
                  "Apply did not activate and hide the detected path");
    return okay;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForeverTASTests"));
    QCoreApplication::setApplicationName(
            QStringLiteral("SearchControllerTests"));
    QStandardPaths::setTestModeEnabled(true);

    QTemporaryDir packsDirectory;
    if (!packsDirectory.isValid()) {
        std::cerr << "failed to create temporary Packs directory\n";
        return 1;
    }
    const QString replayPath =
            packsDirectory.filePath(QStringLiteral("run.Replay.Gbx"));
    QFile replay(replayPath);
    if (!replay.open(QIODevice::WriteOnly)) {
        std::cerr << "failed to create temporary replay file\n";
        return 1;
    }
    replay.write("test");
    replay.close();

    bool okay = TestCompactNumberFormatting() &&
            TestAutomaticSeedRandomization() &&
            TestTargetVisibilityPersistence() &&
            TestAbsoluteTargetPlacement() &&
            TestCuboidTargetModel() &&
            TestCuboidControllerSynchronization() &&
            TestCustomVolumeTargets() &&
            TestPoseTargets() &&
            TestAutomaticPacksDetection() &&
            TestDescriptiveSearchStageStatuses() &&
            TestIterationBoundaryArbitration() &&
            TestUserTimelineConfigurationBoundary() &&
            TestRegistryAndValidation(packsDirectory.path(), replayPath) &&
            TestScenarioInputExtractionAvailability(
                    packsDirectory.path(), replayPath) &&
            TestCompositionEditing(packsDirectory.path(), replayPath) &&
            TestPersistence(packsDirectory.path(), replayPath) &&
            TestStopAbortsBeforeFirstIteration(
                    packsDirectory.path(), replayPath) &&
            TestExtractionFailurePreservesDraft(
                    packsDirectory.path(), replayPath) &&
            TestExtractionWorkerShutdown(
                    packsDirectory.path(), replayPath) &&
            TestLocaleIndependentPersistedDecimals(
                    packsDirectory.path(), replayPath) &&
            TestLegacyMigration();
    if (okay && argc == 4 &&
        QString::fromLocal8Bit(argv[1]) == QStringLiteral("--lifecycle")) {
        okay = TestIndefiniteSearchLifecycle(
                QString::fromLocal8Bit(argv[2]),
                QString::fromLocal8Bit(argv[3]));
    }
    QSettings().clear();
    return okay ? 0 : 1;
}
