#include "app/input_preview_binding.h"

#include "app/search_controller.h"
#include "viewer/race_viewer_controller.h"

#include <algorithm>

namespace forevertas::app {

QMetaObject::Connection BindInputPreview(
        SearchController &controller,
        viewer::RaceViewerController &viewer) {
    viewer.setPreviewInputScript(controller.baseInputScript());
    QObject::connect(
            &controller, &SearchController::searchSessionReset,
            &viewer, [&controller, &viewer]() {
                viewer.setPreviewingHistory(false);
                viewer.clearSearchResults();
                viewer.setPreviewInputScript(controller.baseInputScript());
            });
    const auto updatePreviewHorizon = [&controller, &viewer]() {
        if (viewer.previewingHistory()) return;
        bool ok = false;
        qint64 horizon = controller.simulationHorizonMs().toLongLong(&ok);
        if (!ok || horizon < 10 || horizon > kMaximumSimulationHorizonMs)
            horizon = viewer.simulationHorizonMs();
        const auto includeWindow = [&horizon](const QVariantMap &settings) {
            for (const QString &key : {QStringLiteral("minTimeMs"),
                                       QStringLiteral("maxTimeMs"),
                                       QStringLiteral("targetTimeMs")}) {
                bool valid = false;
                const qint64 time = settings.value(key).toLongLong(&valid);
                if (valid && time >= 0 && time <= kMaximumSimulationHorizonMs)
                    horizon = std::max(horizon, time);
            }
        };
        for (const QVariant &pass : controller.modifierPasses())
            includeWindow(pass.toMap().value(QStringLiteral("settings")).toMap());
        includeWindow(controller.evaluationTargetSettings());
        // Keep edit handles available beyond an invalid search horizon without
        // changing the user's search configuration or making it valid to run.
        viewer.setSimulationHorizonMs((horizon + 9) / 10 * 10);
    };
    updatePreviewHorizon();
    QObject::connect(
            &controller,
            &SearchController::simulationHorizonMsChanged,
            &viewer, updatePreviewHorizon);
    QObject::connect(&controller, &SearchController::modifierPassesChanged,
                     &viewer, updatePreviewHorizon);
    QObject::connect(&controller, &SearchController::evaluationTargetSettingsChanged,
                     &viewer, updatePreviewHorizon);
    QObject::connect(&controller, &SearchController::basePreviewRequested,
                     &viewer, [&controller, &viewer, updatePreviewHorizon]() {
        viewer.setPreviewingHistory(false);
        updatePreviewHorizon();
        viewer.setPreviewInputScript(controller.baseInputScript());
        const bool hasPaths = !controller.packsDirectory().isEmpty() &&
                              !controller.replayPath().isEmpty();
        if (hasPaths && (!viewer.loaded() || viewer.loading() ||
                        viewer.loadedReplayPath() != controller.replayPath() ||
                        viewer.loadedPacksDirectory() != controller.packsDirectory())) {
            viewer.loadMap(controller.packsDirectory(), controller.replayPath());
        } else {
            viewer.refreshInputPreview();
        }
        viewer.focusInputPreview();
    });
    return QObject::connect(
            &controller,
            &SearchController::baseInputScriptChanged,
            &viewer,
            [&controller, &viewer]() {
                if (viewer.previewingHistory()) return;
                viewer.setPreviewInputScript(controller.baseInputScript());
            });
}

}  // namespace forevertas::app
