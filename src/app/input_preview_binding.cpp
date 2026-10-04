#include "app/input_preview_binding.h"

#include "app/search_controller.h"
#include "viewer/race_viewer_controller.h"

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
    // The preview extent is the user's own value, independent of the search
    // end and of any session's horizon. An invalid entry keeps the last valid
    // extent while the field shows the error.
    const auto updatePreviewHorizon = [&controller, &viewer]() {
        bool ok = false;
        const qint64 extent = controller.previewExtentMs().toLongLong(&ok);
        if (!ok || extent < 10 || extent > kMaximumSimulationHorizonMs || extent % 10 != 0) return;
        viewer.setSimulationHorizonMs(extent);
    };
    updatePreviewHorizon();
    QObject::connect(&controller, &SearchController::previewExtentMsChanged,
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
