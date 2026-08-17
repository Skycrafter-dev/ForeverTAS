#include "app/cuboid_target_model.h"
#include "app/compact_number_format.h"
#include "app/custom_volume_target_model.h"
#include "app/pose_target_model.h"
#include "app/packs_directory_finder.h"
#include "app/block_program_model.h"
#include "app/search_controller.h"
#include "app/search_worker.h"

#include <forevervalidator/validation.h>

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
using forevertas::app::BlockProgramModel;
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

int HatBlockId(const SearchController &controller) {
    return controller.blockScript()
            .value(QStringLiteral("hat"))
            .toInt();
}

int EvaluatorBlockId(const SearchController &controller) {
    return controller.blockScript()
            .value(QStringLiteral("evaluator"))
            .toInt();
}

QVariantList WindowGroupMaps(const QVariantMap &script) {
    return script.value(QStringLiteral("groups")).toList();
}

QVariantList MutatorBlockIds(const QVariantMap &script) {
    QVariantList ids;
    for (const QVariant &value : WindowGroupMaps(script)) {
        ids.push_back(value.toMap().value(QStringLiteral("blockId")));
    }
    return ids;
}

QVariantList WindowAtomIds(const QVariantMap &script, int windowIndex) {
    const QVariantList groups = WindowGroupMaps(script);
    if (windowIndex < 0 || windowIndex >= groups.size()) return {};
    return groups.at(windowIndex)
            .toMap()
            .value(QStringLiteral("atoms"))
            .toList();
}

int FirstWindowAtomId(const QVariantMap &script, int windowIndex) {
    const QVariantList atoms = WindowAtomIds(script, windowIndex);
    return atoms.isEmpty() ? 0 : atoms.front().toInt();
}

QString BlockOptionId(const SearchController &controller, int blockId) {
    return controller.blockData(blockId)
            .value(QStringLiteral("optionId"))
            .toString();
}

QString BlockField(const SearchController &controller,
                   int blockId,
                   const QString &key) {
    const QVariantList fields = controller.blockData(blockId)
            .value(QStringLiteral("fields"))
            .toList();
    for (const QVariant &value : fields) {
        const QVariantMap field = value.toMap();
        if (field.value(QStringLiteral("key")).toString() == key) {
            return field.value(QStringLiteral("value")).toString();
        }
    }
    return QString();
}

int MutatorBlockId(const SearchController &controller, int index) {
    return MutatorBlockIds(controller.blockScript()).at(index).toInt();
}

bool HasPaletteBlock(const QVariantList &palette,
                     const QString &definitionId) {
    for (const QVariant &categoryValue : palette) {
        const QVariantList blocks = categoryValue.toMap()
                .value(QStringLiteral("blocks"))
                .toList();
        for (const QVariant &blockValue : blocks) {
            if (blockValue.toMap().value(QStringLiteral("id")).toString() ==
                definitionId) {
                return true;
            }
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
    {
        BlockProgramModel configuration;
        configuration.addBlock(QStringLiteral(
                "mutate/nudge-steering"));
        const QString before = configuration.programText();
        if (!Check(configuration.randomizeSeeds(123456789u),
                   "seeded modifier blocks were not randomized")) {
            return false;
        }
        const QString after = configuration.programText();
        bool okay = Check(after != before,
                          "program text did not reflect seed changes");
        const QRegularExpression seedExpression(
                QStringLiteral("seed = (\\d+)"));
        QRegularExpressionMatchIterator beforeMatches =
                seedExpression.globalMatch(before);
        QRegularExpressionMatchIterator afterMatches =
                seedExpression.globalMatch(after);
        while (beforeMatches.hasNext() && afterMatches.hasNext()) {
            const QString beforeSeed = beforeMatches.next()
                    .captured(1);
            const QString afterSeed = afterMatches.next().captured(1);
            okay &= Check(beforeSeed != afterSeed,
                          "a modifier seed did not change");
        }
    }
    BlockProgramModel restored;
    const bool okay = Check(restored.randomizeSeeds(42u) ||
                            !restored.programText().isEmpty(),
                            "restored program kept its seeds");
    return okay;
}

bool TestLayoutPersistence() {
    QSettings().clear();
    {
        SearchController controller;
        if (!Check(qFuzzyCompare(controller.layoutSettingsPanelWidth(),
                                 390.0) &&
                           qFuzzyCompare(controller.layoutTimelineWidth(),
                                         252.0),
                   "layout widths did not default to the designed sizes")) {
            return false;
        }
        controller.setLayoutSettingsPanelWidth(455.5);
        controller.setLayoutTimelineWidth(240.0);
        // Out-of-range values (corrupted storage, tiny windows) clamp to
        // the ranges the views enforce instead of breaking the layout.
        controller.setLayoutSettingsPanelWidth(5000.0);
        controller.setLayoutTimelineWidth(10.0);
    }
    {
        SearchController restored;
        if (!Check(qFuzzyCompare(restored.layoutSettingsPanelWidth(),
                                 480.0) &&
                           qFuzzyCompare(restored.layoutTimelineWidth(),
                                         220.0),
                   "clamped layout widths were not restored")) {
            return false;
        }
        restored.setLayoutTimelineWidth(300.0);
        restored.setLayoutTimelineWidth(300.0);
        if (!Check(QSettings()
                           .value(QStringLiteral("layout/timelineWidth"))
                           .toDouble() <= 300.0,
                   "layout width storage exceeded the maximum")) {
            return false;
        }
    }

    QSettings().clear();
    return true;
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

QString SnapshotString(const QVariantMap &target, const char *key) {
    return target.value(QString::fromLatin1(key)).toString();
}

bool TestCuboidControllerSynchronization() {
    QSettings().clear();
    SearchController controller;
    controller.setEvaluationTargetId(QStringLiteral("volume-entry-time"));
    CuboidTargetModel *const cuboids = controller.cuboidTargets();
    bool okay = Check(cuboids != nullptr && cuboids->count() == 1,
                      "controller did not expose its cuboid collection");
    okay &= Check(controller.evaluationTargetId() ==
                          QStringLiteral("volume-entry-time"),
                  "volume target did not become the active evaluator");
    const int second = cuboids->addTarget(7.0, 8.0, 9.0);
    okay &= Check(second == 1 &&
                          SnapshotString(cuboids->selectedTarget(),
                                         "centerX") ==
                                  QStringLiteral("7") &&
                          SnapshotString(cuboids->selectedTarget(),
                                         "sizeX") ==
                                  QStringLiteral("10"),
                  "selected cuboid did not become the active search target");
    okay &= Check(cuboids->setSizeComponent(
                          second, QStringLiteral("y"),
                          QStringLiteral("3.25")),
                  "cuboid property edit failed");
    cuboids->selectTarget(0);
    okay &= Check(SnapshotString(cuboids->selectedTarget(), "centerX") ==
                          QStringLiteral("0"),
                  "cuboid selection did not switch the active target");

    // The evaluation block owns its geometry fields; the cuboid picker
    // fills them, and the compiler carries them into the settings.
    QSettings().clear();
    BlockProgramModel program;
    program.setEvaluator(QStringLiteral("evaluate/box-entry-time"));
    const int evaluatorId = program.scriptSummary()
            .value(QStringLiteral("evaluator"))
            .toInt();
    program.setBlockField(evaluatorId, QStringLiteral("centerX"),
                          QStringLiteral("12.5"));
    program.setBlockField(evaluatorId, QStringLiteral("centerY"),
                          QStringLiteral("-2"));
    program.setBlockField(evaluatorId, QStringLiteral("centerZ"),
                          QStringLiteral("40"));
    program.setBlockField(evaluatorId, QStringLiteral("sizeX"),
                          QStringLiteral("8"));
    program.setBlockField(evaluatorId, QStringLiteral("sizeY"),
                          QStringLiteral("3.25"));
    program.setBlockField(evaluatorId, QStringLiteral("sizeZ"),
                          QStringLiteral("12"));
    const forevertas::app::BlockConfigurationValidation validated =
            program.validate(10u, forevertas::kDefaultSimulationHorizonMs);
    okay &= Check(validated.configuration.has_value(),
                  "volume-entry program did not validate");
    if (validated.configuration) {
        const forevertas::OptionSettings &settings =
                validated.configuration->evaluationTarget.settings;
        okay &= Check(settings.at("centerX") == "12.5" &&
                              settings.at("sizeY") == "3.25",
                      "block-owned cuboid fields did not reach the evaluation");
    }
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
    okay &= Check(targets->setDepth(0, QStringLiteral("8")) &&
                          controller.evaluationTargetId() ==
                                  QStringLiteral(
                                          "custom-volume-entry-time"),
                  "custom volume did not remain the active evaluator");
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
    okay &= Check(targets->setPositionComponent(
                          0, QStringLiteral("x"),
                          QStringLiteral("18")) &&
                          targets->setRotationComponent(
                                  0, QStringLiteral("yaw"),
                                  QStringLiteral("75")) &&
                          targets->selectedTarget()
                                          .value(QStringLiteral("x"))
                                          .toString() ==
                                  QStringLiteral("18") &&
                          targets->selectedTarget()
                                          .value(QStringLiteral(
                                                  "yawDegrees"))
                                          .toString() ==
                                  QStringLiteral("75"),
                  "selected pose target edits failed");
    okay &= Check(controller.evaluationTargetId() ==
                          QStringLiteral("pose-target"),
                  "pose target did not remain the active evaluator");

    // The evaluation block owns its pose fields; the pose picker fills
    // them, and the compiler carries them into the settings.
    QSettings().clear();
    BlockProgramModel program;
    program.setEvaluator(QStringLiteral("evaluate/distance-to-pose"));
    const int evaluatorId = program.scriptSummary()
            .value(QStringLiteral("evaluator"))
            .toInt();
    program.setBlockField(evaluatorId, QStringLiteral("x"),
                          QStringLiteral("-8"));
    program.setBlockField(evaluatorId, QStringLiteral("y"),
                          QStringLiteral("4"));
    program.setBlockField(evaluatorId, QStringLiteral("z"),
                          QStringLiteral("6"));
    program.setBlockField(evaluatorId, QStringLiteral("yawDegrees"),
                          QStringLiteral("35"));
    program.setBlockField(evaluatorId, QStringLiteral("pitchDegrees"),
                          QStringLiteral("0"));
    program.setBlockField(evaluatorId, QStringLiteral("rollDegrees"),
                          QStringLiteral("-35"));
    const forevertas::app::BlockConfigurationValidation validated =
            program.validate(10u, forevertas::kDefaultSimulationHorizonMs);
    okay &= Check(validated.configuration.has_value(),
                  "pose program did not validate");
    if (validated.configuration) {
        const forevertas::OptionSettings &settings =
                validated.configuration->evaluationTarget.settings;
        okay &= Check(settings.at("x") == "-8" &&
                              settings.at("rollDegrees") == "-35",
                      "block-owned pose fields did not reach the evaluation");
    }
    return okay;
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
    BlockProgramModel configuration;
    const QVariantMap script = configuration.scriptSummary();
    const int windowId =
            MutatorBlockIds(configuration.scriptSummary()).front().toInt();
    const int evaluatorId = script.value(QStringLiteral("evaluator")).toInt();
    bool okay = true;
    okay &= Check(configuration.removeBlock(windowId) &&
                          configuration.addBlock(
                                  QStringLiteral(
                                          "mutate/smooth-steering")),
                  "failed to select a duration-bearing modifier");
    const int smoothWindowId =
            MutatorBlockIds(configuration.scriptSummary()).front().toInt();
    const int smoothId = FirstWindowAtomId(configuration.scriptSummary(), 0);
    okay &= Check(configuration.setBlockField(
                          smoothWindowId, QStringLiteral("minTimeMs"),
                          QStringLiteral("0")) &&
                          configuration.setBlockField(
                                  smoothWindowId, QStringLiteral("maxTimeMs"),
                                  QStringLiteral("20")) &&
                          configuration.setBlockField(
                                  smoothId, QStringLiteral("radiusMs"),
                                  QStringLiteral("210")),
                  "failed to configure user timeline modifier values");
    okay &= Check(configuration.setBlockField(
                          evaluatorId, QStringLiteral("minTimeMs"),
                          QStringLiteral("0")) &&
                          configuration.setBlockField(
                                  evaluatorId, QStringLiteral("maxTimeMs"),
                                  QStringLiteral("20")),
                  "failed to configure user timeline evaluation values");

    const auto validated = configuration.validate(
            10u, forevertas::kDefaultSimulationHorizonMs);
    okay &= Check(validated.configuration.has_value() &&
                          validated.error.isEmpty(),
                  "zero-based user timeline settings did not validate");
    if (!validated.configuration) return false;

    const auto fieldOf = [&configuration](int blockId, const QString &key) {
        const QVariantList fields = configuration.blockData(blockId)
                .value(QStringLiteral("fields"))
                .toList();
        for (const QVariant &value : fields) {
            const QVariantMap field = value.toMap();
            if (field.value(QStringLiteral("key")).toString() == key)
                return field.value(QStringLiteral("value")).toString();
        }
        return QString();
    };
    const forevertas::OptionSettings &configuredModifier =
            validated.configuration->modifiers.front().settings;
    const forevertas::OptionSettings &configuredEvaluation =
            validated.configuration->evaluationTarget.settings;
    okay &= Check(fieldOf(smoothWindowId, QStringLiteral("minTimeMs")) ==
                                  QStringLiteral("0") &&
                          fieldOf(smoothWindowId,
                                  QStringLiteral("maxTimeMs")) ==
                                  QStringLiteral("20") &&
                          fieldOf(smoothId, QStringLiteral("radiusMs")) ==
                                  QStringLiteral("210") &&
                          fieldOf(evaluatorId, QStringLiteral("minTimeMs")) ==
                                  QStringLiteral("0") &&
                          fieldOf(evaluatorId, QStringLiteral("maxTimeMs")) ==
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
    controller.setBlockField(
            MutatorBlockId(controller, 0), QStringLiteral("maxTimeMs"),
            QStringLiteral("6000"));
    okay &= Check(controller.canStart() &&
                          !controller.validationMessage().contains(
                                  QStringLiteral("maps to simulation time")),
                  "modifier time beyond the horizon was not silently clamped");
    controller.setBlockField(
            MutatorBlockId(controller, 0), QStringLiteral("maxTimeMs"),
            QStringLiteral("4990"));
    controller.setSimulationHorizonMs(QStringLiteral("5000"));
    okay &= Check(!controller.canStart() &&
                          controller.validationMessage().contains(
                                  QStringLiteral("Evaluation maximum time")),
                  "evaluation window beyond the Simulation horizon was accepted");
    controller.setSimulationHorizonMs(QStringLiteral("6010"));
    okay &= Check(controller.canStart(),
                  "valid Simulation horizon did not enable Start");
    controller.setBlockField(
            MutatorBlockId(controller, 0), QStringLiteral("maxTimeMs"),
            QStringLiteral("5990"));
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
                                  "NVIDIA CUDA for Stadium; compute "
                                  "capability 5.0+ is supported, with Fast "
                                  "CUDA on 7.5+")),
                  "CUDA metadata was not exposed");
    const forevervalidator::CudaBackendDiagnostics cudaDiagnostics =
            forevervalidator::QueryCudaBackendDiagnostics();
    okay &= Check(controller.cudaAvailable() == cudaDiagnostics.IsReady() &&
                          controller.cudaFastModeAvailable() ==
                                  cudaDiagnostics
                                          .SupportsSessionSpecialization() &&
                          !controller.cudaStatusText().isEmpty(),
                  "CUDA compatibility status did not match runtime diagnostics");
#endif
    okay &= Check(controller.cudaParallelSampleCount() ==
                          QString::number(
                                  forevertas::kDefaultCudaParallelSampleCount),
                  "unexpected default CUDA parallel sample count");
    okay &= Check(!controller.cudaCalibrationEnabled(),
                  "CUDA calibration was unexpectedly enabled by default");
    okay &= Check(controller.cudaSessionSpecializationEnabled(),
                  "CUDA fast mode was not enabled by default");
    okay &= Check(BlockOptionId(controller, HatBlockId(controller)) ==
                          QStringLiteral("basic-brute-force"),
                  "default search block was incorrect");
    okay &= Check(
            BlockField(controller, HatBlockId(controller),
                       QStringLiteral("autoPromoteBest")) ==
                    QStringLiteral("false"),
            "auto-promote search mode was unexpectedly enabled by default");
    okay &= Check(controller.evaluationTargetId() ==
                          QStringLiteral("velocity"),
                  "default evaluation block was incorrect");
    okay &= Check(
            HasPaletteBlock(controller.blockPalette(),
                            QStringLiteral("search/basic-brute-force")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("mutate/window")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("mutate/nudge-steering")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("mutate/set-steering")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("mutate/shift-events")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("mutate/flip-accelerate")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("mutate/flip-brake")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("mutate/smooth-steering")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("mutate/insert-steering-at")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("mutate/adjust-steering-by")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("mutate/press-accelerate")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("mutate/press-brake")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("mutate/delete-steering")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("mutate/delete-accelerate")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("mutate/delete-brake")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("mutate/reroll-steering")) &&
                    HasPaletteBlock(
                            controller.blockPalette(),
                            QStringLiteral("evaluate/finish-time")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("evaluate/box-entry-time")) &&
                    HasPaletteBlock(
                            controller.blockPalette(),
                            QStringLiteral("evaluate/prism-entry-time")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("evaluate/stunt-points")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("evaluate/speed")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("evaluate/speed-toward")) &&
                    HasPaletteBlock(
                            controller.blockPalette(),
                            QStringLiteral("evaluate/distance-to-point")) &&
                    HasPaletteBlock(
                            controller.blockPalette(),
                            QStringLiteral("evaluate/distance-to-pose")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("values/number")) &&
                    HasPaletteBlock(controller.blockPalette(),
                                    QStringLiteral("values/add")),
            "required palette blocks were not exposed");
    okay &= Check(MutatorBlockIds(controller.blockScript()).size() == 1 &&
                          BlockOptionId(
                                  controller,
                                  FirstWindowAtomId(
                                          controller.blockScript(), 0)) ==
                                  QStringLiteral("random-steering"),
                  "default modifier block was incorrect");

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
    okay &= Check(controller.simulationBackendId() == QStringLiteral("cuda"),
                  "CUDA backend was not selectable");
    if (controller.cudaAvailable()) {
        okay &= Check(controller.canStart(),
                      "available CUDA backend did not enable Start");
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
                      "CUDA fast preference was not selectable");
    } else {
        okay &= Check(!controller.canStart() &&
                              controller.validationMessage() ==
                                      controller.cudaStatusText(),
                      "unavailable CUDA backend did not expose its incompatibility");
    }
    controller.setSimulationBackendId(QStringLiteral("optimized-cpu"));
#endif
    controller.setSimulationBackendId(QStringLiteral("missing-backend"));
    okay &= Check(controller.simulationBackendId() ==
                          QStringLiteral("optimized-cpu"),
                  "invalid physics backend changed the selection");
    controller.setSimulationBackendId(QStringLiteral("reference"));


    controller.setBlockField(
            MutatorBlockId(controller, 0), QStringLiteral("seed"),
            QStringLiteral("4294967296"));
    okay &= Check(!controller.canStart(),
                  "modifier seed overflow enabled Start");
    controller.setBlockField(
            MutatorBlockId(controller, 0), QStringLiteral("seed"),
            QStringLiteral("123"));

    controller.setBlockField(
            MutatorBlockId(controller, 0), QStringLiteral("minTimeMs"),
            QStringLiteral("1001"));
    okay &= Check(!controller.canStart(),
                  "unaligned modifier time enabled Start");
    controller.setBlockField(
            MutatorBlockId(controller, 0), QStringLiteral("minTimeMs"),
            QStringLiteral("1000"));

    controller.setBlockField(
            EvaluatorBlockId(controller), QStringLiteral("minTimeMs"),
            QStringLiteral("1001"));
    okay &= Check(!controller.canStart(),
                  "unaligned evaluation time enabled Start");
    controller.setBlockField(
            EvaluatorBlockId(controller), QStringLiteral("minTimeMs"),
            QStringLiteral("1000"));

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
                    BlockField(controller, EvaluatorBlockId(controller),
                               QStringLiteral("targetTimeMs")) ==
                            QStringLiteral("6000"),
            "stunt target defaults did not validate");
    controller.setBlockField(
            EvaluatorBlockId(controller), QStringLiteral("targetTimeMs"),
            QStringLiteral("6001"));
    okay &= Check(!controller.canStart(),
                  "unaligned stunt target time enabled Start");
    controller.setBlockField(
            EvaluatorBlockId(controller), QStringLiteral("targetTimeMs"),
            QStringLiteral("4320"));
    okay &= Check(controller.canStart(),
                  "valid stunt target time did not enable Start");
    controller.setBlockField(
            EvaluatorBlockId(controller), QStringLiteral("targetTimeMs"),
            QStringLiteral("500"));
    okay &= Check(
            !controller.canStart() &&
                    controller.validationMessage().contains(
                            QStringLiteral("first modifier time")),
            "stunt target accepted a deadline before any mutation");
    controller.setBlockField(
            EvaluatorBlockId(controller), QStringLiteral("targetTimeMs"),
            QStringLiteral("4320"));
    okay &= Check(!controller.setEvaluatorBlock(
                          QStringLiteral("evaluate/missing-target")) &&
                          controller.evaluationTargetId() ==
                                  QStringLiteral("stunt-points"),
                  "unknown evaluation target changed the evaluator");
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

    controller.addBlock(QStringLiteral("mutate/window"));
    controller.addBlock(QStringLiteral("mutate/delete-steering"));
    bool okay = Check(MutatorBlockIds(controller.blockScript()).size() == 2,
                      "mutation window was not added");
    controller.setBlockField(
            FirstWindowAtomId(controller.blockScript(), 1),
            QStringLiteral("steerMaxCount"), QStringLiteral("4"));
    okay &= Check(BlockField(controller,
                             FirstWindowAtomId(controller.blockScript(), 1),
                             QStringLiteral("steerMaxCount")) ==
                          QStringLiteral("4"),
                  "block-owned setting was not changed");

    controller.moveBlock(MutatorBlockId(controller, 1), 0);
    okay &= Check(BlockOptionId(controller,
                                FirstWindowAtomId(
                                        controller.blockScript(), 0)) ==
                          QStringLiteral("input-deletion") &&
                          BlockOptionId(controller,
                                        FirstWindowAtomId(
                                                controller.blockScript(), 1)) ==
                                  QStringLiteral("random-steering"),
                  "mutation window order did not change");

    // Replacing a window's atoms resets their settings to the schema.
    controller.removeBlock(MutatorBlockId(controller, 1));
    controller.addBlock(QStringLiteral("mutate/window"));
    controller.addBlock(QStringLiteral("mutate/smooth-steering"));
    okay &= Check(MutatorBlockIds(controller.blockScript()).size() == 2 &&
                          BlockOptionId(controller,
                                        FirstWindowAtomId(
                                                controller.blockScript(), 1)) ==
                                  QStringLiteral("smooth-steering") &&
                          BlockField(controller,
                                     FirstWindowAtomId(
                                             controller.blockScript(), 1),
                                     QStringLiteral("radiusMs")) ==
                                  QStringLiteral("200"),
                  "modifier block type did not replace its settings");

    controller.removeBlock(MutatorBlockId(controller, 1));
    controller.removeBlock(MutatorBlockId(controller, 0));
    okay &= Check(MutatorBlockIds(controller.blockScript()).isEmpty(),
                  "mutation windows were not removed");
    okay &= Check(!controller.canStart(),
                  "empty modifier stack enabled Start");
    controller.addBlock(QStringLiteral("mutate/reroll-steering"));
    okay &= Check(controller.canStart(),
                  "restored modifier stack did not enable Start");
    return okay;
}

QVariantMap CanvasEntry(const SearchController &controller, int blockId) {
    for (const QVariant &value : controller.blockCanvas()) {
        const QVariantMap entry = value.toMap();
        if (entry.value(QStringLiteral("blockId")).toInt() == blockId) {
            return entry;
        }
    }
    return {};
}

bool CanvasHasEntry(const SearchController &controller, int blockId) {
    return !CanvasEntry(controller, blockId).isEmpty();
}

bool TestBlockCanvasEditing(const QString &packsDirectory,
                            const QString &replayPath) {
    QSettings().clear();
    SearchController controller;
    SetValidPaths(controller, packsDirectory, replayPath);

    QVariantList canvas = controller.blockCanvas();
    bool okay = Check(
            canvas.size() == 1 &&
                    canvas.front().toMap()
                            .value(QStringLiteral("isScript"))
                            .toBool() &&
                    canvas.front().toMap()
                                .value(QStringLiteral("blockId"))
                                .toInt() == HatBlockId(controller),
            "canvas did not open with the script hat as its only entry");

    // Structure is renderable from blockData alone.
    const QVariantMap hatData = controller.blockData(HatBlockId(controller));
    okay &= Check(
            hatData.value(QStringLiteral("evaluator")).toInt() ==
                            EvaluatorBlockId(controller) &&
                    !hatData.value(QStringLiteral("substack"))
                             .toList()
                             .isEmpty(),
            "hat blockData lacks the structural evaluator/substack ids");

    // A palette drop creates a loose block at the drop position.
    const int numberId =
            controller.addLooseBlock(QStringLiteral("values/number"), 40, 90);
    okay &= Check(numberId != 0 &&
                          CanvasHasEntry(controller, numberId),
                  "loose value block was not placed on the canvas");
    QVariantMap entry = CanvasEntry(controller, numberId);
    okay &= Check(
            !entry.value(QStringLiteral("isScript")).toBool() &&
                    entry.value(QStringLiteral("x")).toDouble() == 40.0 &&
                    entry.value(QStringLiteral("y")).toDouble() == 90.0 &&
                    controller.blockData(numberId)
                                .value(QStringLiteral("x"))
                                .toDouble() == 40.0,
            "loose block position was not stored or exposed");
    okay &= Check(!controller.attachBlock(HatBlockId(controller), 0,
                                          numberId),
                  "a reporter stacked under the hat was accepted");

    // Detaching a window carries its atoms; the script reacts.
    const int windowId = MutatorBlockId(controller, 0);
    const int atomId = FirstWindowAtomId(controller.blockScript(), 0);
    okay &= Check(!controller.detachBlockToCanvas(
                          HatBlockId(controller), 10, 10),
                  "the script hat was detached from itself");
    okay &= Check(controller.detachBlockToCanvas(windowId, 120, 80) &&
                          CanvasHasEntry(controller, windowId) &&
                          MutatorBlockIds(controller.blockScript())
                                  .isEmpty() &&
                          !controller.canStart(),
                  "window detach did not empty the script or block Start");
    {
        const QVariantMap windowData = controller.blockData(windowId);
        okay &= Check(windowData.value(QStringLiteral("substack"))
                              .toList()
                              .contains(atomId),
                      "detached window lost its atoms");
    }
    okay &= Check(controller.attachBlock(HatBlockId(controller), 0,
                                         windowId) &&
                          !CanvasHasEntry(controller, windowId) &&
                          MutatorBlockIds(controller.blockScript()).size() ==
                                  1 &&
                          FirstWindowAtomId(controller.blockScript(), 0) ==
                                  atomId &&
                          controller.canStart(),
                  "window re-attach did not restore the script");

    // Windows can be nested nowhere, atoms move between windows, and the
    // attach index counts the substack after the child is detached.
    controller.addBlock(QStringLiteral("mutate/window"));
    controller.addBlock(QStringLiteral("mutate/reroll-steering"));
    const int secondWindow = MutatorBlockId(controller, 1);
    const int rerollId =
            FirstWindowAtomId(controller.blockScript(), 1);
    okay &= Check(!controller.attachBlock(secondWindow, 0, windowId),
                  "a window nested inside a window was accepted");
    okay &= Check(!controller.attachBlock(HatBlockId(controller), 0,
                                          rerollId),
                  "an atom stacked directly under the hat was accepted");
    okay &= Check(controller.attachBlock(secondWindow, 0, atomId) &&
                          FirstWindowAtomId(controller.blockScript(), 1) ==
                                  atomId &&
                          !WindowAtomIds(controller.blockScript(), 0)
                                   .contains(atomId),
                  "atom did not move between windows");
    okay &= Check(controller.attachBlock(secondWindow, 2, atomId) &&
                          WindowAtomIds(controller.blockScript(), 1)
                                  .last()
                                  .toInt() == atomId &&
                          WindowAtomIds(controller.blockScript(), 1)
                                  .first()
                                  .toInt() == rerollId,
                  "attach index did not follow detach-then-insert semantics");

    // Loose reporters graft into number slots and leave the canvas.
    const int graftedId =
            controller.addLooseBlock(QStringLiteral("values/number"), 5, 5);
    controller.setBlockField(graftedId, QStringLiteral("value"),
                             QStringLiteral("1500"));
    okay &= Check(controller.graftReporterBlock(
                          windowId, QStringLiteral("minTimeMs"), graftedId,
                          0, 0) &&
                          !CanvasHasEntry(controller, graftedId),
                  "grafting a loose reporter did not remove it from the canvas");
    {
        bool chipFound = false;
        const QVariantList fields =
                controller.blockData(windowId)
                        .value(QStringLiteral("fields"))
                        .toList();
        for (const QVariant &value : fields) {
            const QVariantMap field = value.toMap();
            if (field.value(QStringLiteral("key")).toString() !=
                QStringLiteral("minTimeMs")) {
                continue;
            }
            chipFound = field.value(QStringLiteral("reporter"))
                                .toMap()
                                .value(QStringLiteral("blockId"))
                                .toInt() == graftedId;
        }
        okay &= Check(chipFound,
                      "grafted reporter is not exposed on its slot");
    }
    okay &= Check(!controller.graftReporterBlock(
                          graftedId, QStringLiteral("value"), graftedId,
                          0, 0),
                  "grafting a block into its own slot was accepted");
    const int parentOpId =
            controller.addLooseBlock(QStringLiteral("values/add"), 6, 6);
    const int nestedId = controller.attachReporter(
            parentOpId, QStringLiteral("left"),
            QStringLiteral("values/add"));
    okay &= Check(nestedId != 0 &&
                          !controller.graftReporterBlock(
                                  nestedId, QStringLiteral("right"),
                                  parentOpId, 0, 0),
                  "grafting a reporter into its own descendant was accepted");

    // Swapping chips parks the displaced reporter on the canvas.
    const int replacementId = controller.addLooseBlock(
            QStringLiteral("values/number"), 7, 7);
    okay &= Check(controller.graftReporterBlock(
                          windowId, QStringLiteral("minTimeMs"),
                          replacementId, 210, 190) &&
                          CanvasHasEntry(controller, graftedId) &&
                          CanvasEntry(controller, graftedId)
                                  .value(QStringLiteral("x"))
                                  .toDouble() == 210.0 &&
                          !CanvasHasEntry(controller, replacementId),
                  "displaced reporter was deleted instead of parked");

    // Detaching a chip pulls the reporter back onto the canvas.
    okay &= Check(controller.detachReporterToCanvas(
                          windowId, QStringLiteral("minTimeMs"), 150, 260) &&
                          CanvasHasEntry(controller, replacementId) &&
                          CanvasEntry(controller, replacementId)
                                  .value(QStringLiteral("y"))
                                  .toDouble() == 260.0,
                  "detaching a chip did not park the reporter on the canvas");

    // Evaluation reporters snap into the evaluator socket and back out.
    const int stuntId = controller.addLooseBlock(
            QStringLiteral("evaluate/stunt-points"), 10, 20);
    okay &= Check(stuntId != 0 &&
                          !controller.setEvaluatorBlockId(graftedId) &&
                          controller.setEvaluatorBlockId(stuntId) &&
                          EvaluatorBlockId(controller) == stuntId &&
                          controller.evaluationTargetId() ==
                                  QStringLiteral("stunt-points") &&
                          !CanvasHasEntry(controller, stuntId),
                  "evaluation reporter did not snap into the evaluator socket");
    okay &= Check(controller.detachBlockToCanvas(stuntId, 30, 50) &&
                          controller.evaluationTargetId().isEmpty() &&
                          !controller.canStart() &&
                          controller.setEvaluatorBlockId(stuntId) &&
                          EvaluatorBlockId(controller) == stuntId,
                  "evaluator detach or re-attach lost the block");

    // Canvas contents, positions, and wiring survive persistence.
    const QVariantList before = controller.blockCanvas();
    const int windowCount =
            MutatorBlockIds(controller.blockScript()).size();
    QSettings().sync();
    {
        SearchController restored;
        const QVariantList after = restored.blockCanvas();
        bool same = after.size() == before.size();
        for (const QVariant &value : before) {
            const QVariantMap expected = value.toMap();
            const QVariantMap found = CanvasEntry(
                    restored,
                    expected.value(QStringLiteral("blockId")).toInt());
            same = same && found == expected;
        }
        okay &= Check(same,
                      "canvas entries or positions were not persisted");
        okay &= Check(MutatorBlockIds(restored.blockScript()).size() ==
                              windowCount &&
                              FirstWindowAtomId(restored.blockScript(),
                                                1) == rerollId &&
                              WindowAtomIds(restored.blockScript(), 1)
                                      .last()
                                      .toInt() == atomId,
                      "script structure was not persisted alongside the canvas");
    }
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
        controller.setBlockField(
                MutatorBlockId(controller, 0), QStringLiteral("seed"),
                QStringLiteral("321"));
        controller.addBlock(QStringLiteral("mutate/window"));
        controller.addBlock(QStringLiteral("mutate/delete-steering"));
        controller.setBlockField(
                FirstWindowAtomId(controller.blockScript(), 1),
                QStringLiteral("steerMaxCount"), QStringLiteral("4"));
        controller.moveBlock(MutatorBlockId(controller, 1), 0);
        controller.setSimulationBackendId(QStringLiteral("optimized-cpu"));
        controller.setCpuWorkerCount(QStringLiteral("6"));
        controller.setCudaParallelSampleCount(QStringLiteral("384"));
        controller.setCudaCalibrationEnabled(true);
        controller.setCudaSessionSpecializationEnabled(false);
        controller.setDarkMode(true);
        controller.setBlockField(
                HatBlockId(controller), QStringLiteral("autoPromoteBest"),
                QStringLiteral("true"));
        controller.setEvaluationTargetId(QStringLiteral("point-target"));
        controller.setBlockField(
                EvaluatorBlockId(controller), QStringLiteral("x"),
                QStringLiteral("12.5"));
        controller.setEvaluationTargetId(QStringLiteral("stunt-points"));
        controller.setBlockField(
                EvaluatorBlockId(controller), QStringLiteral("targetTimeMs"),
                QStringLiteral("4320"));
        controller.setBaseInputScript(
                QStringLiteral("0.00 press up\n0.50 steer -16384"));
        QSettings().sync();
    }

    SearchController restored;
    bool okay = Check(
            BlockField(restored, HatBlockId(restored),
                       QStringLiteral("autoPromoteBest")) ==
                    QStringLiteral("true"),
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
    okay &= Check(MutatorBlockIds(restored.blockScript()).size() == 2,
                  "mutation window count was not persisted");
    okay &= Check(
            BlockOptionId(restored,
                          FirstWindowAtomId(restored.blockScript(), 0)) ==
                    QStringLiteral("input-deletion") &&
                    BlockField(restored,
                               FirstWindowAtomId(restored.blockScript(), 0),
                               QStringLiteral("steerMaxCount")) ==
                            QStringLiteral("4"),
            "first modifier window was not persisted");
    okay &= Check(
            BlockOptionId(restored,
                          FirstWindowAtomId(restored.blockScript(), 1)) ==
                    QStringLiteral("random-steering") &&
                    BlockField(restored, MutatorBlockId(restored, 1),
                               QStringLiteral("seed")) ==
                            QStringLiteral("321"),
            "second modifier window was not persisted");
    okay &= Check(restored.evaluationTargetId() ==
                          QStringLiteral("stunt-points") &&
                          BlockField(restored, EvaluatorBlockId(restored),
                                     QStringLiteral("targetTimeMs")) ==
                                  QStringLiteral("4320"),
                  "evaluation target configuration was not persisted");
    okay &= Check(QSettings().contains(QStringLiteral("blocks/program")),
                  "block program JSON was not persisted");
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
    QSignalSpy completionSpy(
            &controller, &SearchController::searchCompleted);

    QElapsedTimer elapsed;
    elapsed.start();
    controller.startSearch();
    controller.stopSearch();
    bool okay = Check(
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
        controller.setBlockField(
                EvaluatorBlockId(controller), QStringLiteral("x"),
                QStringLiteral("12.5"));
        controller.setBlockField(
                EvaluatorBlockId(controller), QStringLiteral("y"),
                QStringLiteral("-3.25"));
        controller.removeBlock(MutatorBlockId(controller, 0));
        controller.addBlock(QStringLiteral(
                "mutate/nudge-steering"));
        controller.setBlockField(
                FirstWindowAtomId(controller.blockScript(), 0),
                QStringLiteral("steerDeltaMin"), QStringLiteral("-0.25"));
        controller.setBlockField(
                FirstWindowAtomId(controller.blockScript(), 0),
                QStringLiteral("steerDeltaMax"), QStringLiteral("0.25"));

        bool okay = Check(
                controller.canStart(),
                "UI-entered dot decimals failed under comma LC_NUMERIC");
        controller.setBlockField(
                EvaluatorBlockId(controller), QStringLiteral("x"),
                QStringLiteral("12,5"));
        okay &= Check(!controller.canStart(),
                      "UI-entered comma decimal was accepted");
        controller.setBlockField(
                EvaluatorBlockId(controller), QStringLiteral("x"),
                QStringLiteral("12.5"));
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
                    BlockField(restored, EvaluatorBlockId(restored),
                               QStringLiteral("x")) ==
                            QStringLiteral("12.5") &&
                    BlockField(restored, EvaluatorBlockId(restored),
                               QStringLiteral("y")) ==
                            QStringLiteral("-3.25"),
            "persisted evaluation decimals changed representation");
    okay &= Check(
            BlockOptionId(restored,
                          FirstWindowAtomId(restored.blockScript(), 0)) ==
                            QStringLiteral("existing-event-perturbation") &&
                    BlockField(restored,
                               FirstWindowAtomId(restored.blockScript(), 0),
                               QStringLiteral("steerDeltaMin")) ==
                            QStringLiteral("-0.25") &&
                    BlockField(restored,
                               FirstWindowAtomId(restored.blockScript(), 0),
                               QStringLiteral("steerDeltaMax")) ==
                            QStringLiteral("0.25"),
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
    okay &= Check(
            MutatorBlockIds(controller.blockScript()).size() == 1 &&
                    BlockOptionId(
                            controller,
                            FirstWindowAtomId(controller.blockScript(), 0)) ==
                            QStringLiteral("random-steering"),
            "legacy mutation selection was not migrated");
    okay &= Check(
            BlockField(controller, MutatorBlockId(controller, 0),
                       QStringLiteral("minTimeMs")) ==
                    QStringLiteral("1200") &&
                    BlockField(controller, MutatorBlockId(controller, 0),
                               QStringLiteral("maxTimeMs")) ==
                            QStringLiteral("2400") &&
                    BlockField(controller, MutatorBlockId(controller, 0),
                               QStringLiteral("seed")) ==
                            QStringLiteral("987"),
            "legacy modifier settings were not migrated");
    okay &= Check(controller.evaluationTargetId() ==
                          QStringLiteral("velocity"),
                  "legacy evaluation target was canonicalized");
    okay &= Check(QSettings().contains(QStringLiteral("blocks/program")),
                  "migrated block program was not persisted");
    return okay;
}


bool TestIndefiniteSearchLifecycle(const QString &packsDirectory,
                                   const QString &replayPath) {
    QSettings().clear();
    SearchController controller;
    SetValidPaths(controller, packsDirectory, replayPath);
    controller.setSimulationBackendId(QStringLiteral("optimized-cpu"));
    controller.setBlockField(
            MutatorBlockId(controller, 0), QStringLiteral("minTimeMs"),
            QStringLiteral("0"));
    controller.setBlockField(
            MutatorBlockId(controller, 0), QStringLiteral("maxTimeMs"),
            QStringLiteral("20"));
    controller.setBlockField(
            EvaluatorBlockId(controller), QStringLiteral("minTimeMs"),
            QStringLiteral("0"));
    controller.setBlockField(
            EvaluatorBlockId(controller), QStringLiteral("maxTimeMs"),
            QStringLiteral("20"));
    const bool zeroOriginConfigured =
            BlockField(controller, MutatorBlockId(controller, 0),
                       QStringLiteral("minTimeMs")) ==
                    QStringLiteral("0") &&
            BlockField(controller, MutatorBlockId(controller, 0),
                       QStringLiteral("maxTimeMs")) ==
                    QStringLiteral("20") &&
            BlockField(controller, EvaluatorBlockId(controller),
                       QStringLiteral("minTimeMs")) ==
                    QStringLiteral("0") &&
            BlockField(controller, EvaluatorBlockId(controller),
                       QStringLiteral("maxTimeMs")) ==
                    QStringLiteral("20");
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
    const QString seedBeforeStart =
            BlockField(controller, MutatorBlockId(controller, 0),
                       QStringLiteral("seed"));
    controller.startSearch();
    bool okay = Check(controller.running() && !controller.canStart(),
                      "Start did not enter the running state");
    okay &= Check(BlockField(controller, MutatorBlockId(controller, 0),
                             QStringLiteral("seed")) != seedBeforeStart,
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
                          !controller.resultText().isEmpty() &&
                          !controller.bestInputsText().isEmpty(),
                  "completed search did not preserve the final best display");
    return okay;
}

bool TestMetricsWhenConditionExcludesBaseline(
        const QString &packsDirectory,
        const QString &replayPath) {
    QSettings().clear();
    SearchController controller;
    SetValidPaths(controller, packsDirectory, replayPath);
    controller.setSimulationBackendId(QStringLiteral("optimized-cpu"));
    controller.setBlockField(
            MutatorBlockId(controller, 0), QStringLiteral("minTimeMs"),
            QStringLiteral("0"));
    controller.setBlockField(
            MutatorBlockId(controller, 0), QStringLiteral("maxTimeMs"),
            QStringLiteral("20"));
    controller.setBlockField(
            EvaluatorBlockId(controller), QStringLiteral("minTimeMs"),
            QStringLiteral("0"));
    controller.setBlockField(
            EvaluatorBlockId(controller), QStringLiteral("maxTimeMs"),
            QStringLiteral("20"));
    controller.setConditionScript(
            QStringLiteral("iterations > 1000000000000"));
    if (!Check(controller.canStart(),
               "baseline-excluding condition did not enable search")) {
        return false;
    }

    controller.startSearch();
    bool okay = Check(
            WaitUntil(
                    [&controller]() {
                        return controller.running() &&
                                controller.statusText() ==
                                        QStringLiteral("Searching...") &&
                                controller.liveMetricsVisible() &&
                                !controller.iterationCountText().isEmpty() &&
                                controller.iterationCountText() !=
                                        QStringLiteral("0.00") &&
                                !controller.throughputText().isEmpty() &&
                                !controller.elapsedText().isEmpty() &&
                                controller.resultText().isEmpty() &&
                                controller.bestInputsText().isEmpty();
                    },
                    30000),
            "live metrics stayed hidden while the condition excluded all "
            "current candidates");

    controller.stopSearch();
    okay &= Check(
            WaitUntil([&controller]() { return !controller.running(); },
                      30000),
            "baseline-excluding metrics test did not stop cleanly");
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
            TestLayoutPersistence() &&
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
            TestBlockCanvasEditing(packsDirectory.path(), replayPath) &&
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
        const QString packs = QString::fromLocal8Bit(argv[2]);
        const QString replay = QString::fromLocal8Bit(argv[3]);
        okay = TestMetricsWhenConditionExcludesBaseline(packs, replay) &&
                TestIndefiniteSearchLifecycle(packs, replay);
    }
    QSettings().clear();
    return okay ? 0 : 1;
}
