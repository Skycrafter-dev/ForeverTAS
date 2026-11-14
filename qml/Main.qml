import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml.Models
import QtQuick3D
import QtQuick.Effects
import QtCore
import "settings"
import ForeverTAS.Viewer 1.0

ApplicationWindow {
    id: window

    required property var controller
    required property var viewer
    property var updater: null
    property var shutdown: null
    // The session log (Help > Open log folder).
    property var logs: null
    // a GPU's own settings: its backend, or "GPU" with its card ticked
    readonly property bool cudaSettingsShown: controller.simulationBackendId === "cuda"
            || (controller.simulationBackendId === "gpus" && controller.cudaCardEnabled)
    readonly property bool hipSettingsShown: controller.simulationBackendId === "hip"
            || (controller.simulationBackendId === "gpus" && controller.hipCardEnabled)
    onClosing: close => {
        dockSpace.flushState()
        if (windowSaveTimer.running)
            window.saveWindow()
        if (lookSaveTimer.running)
            window.saveLook()
        if (shutdown && !shutdown.requestClose()) {
            close.accepted = false
            shutdownDialog.open()
        }
    }
    Connections {
        target: window.shutdown
        function onReadyToClose() {
            shutdownDialog.close()
            window.close()
        }
    }
    GlassDialog {
        id: shutdownDialog
        objectName: "shutdownDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(460, window.width - 32)
        title: qsTr("Closing ForeverTAS")
        modal: true
        closePolicy: Popup.NoAutoClose
        contentItem: ColumnLayout {
            spacing: 12
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: window.shutdown && window.shutdown.delayed
                    ? qsTr("Background work has not stopped yet. You can keep waiting or force exit. Force exit discards any unfinished result; already saved history is retained.")
                    : qsTr("Stopping background work and saving settings...")
            }
            ThemedButton {
                objectName: "forceExitButton"
                Layout.alignment: Qt.AlignRight
                visible: window.shutdown && window.shutdown.delayed
                text: qsTr("Force exit")
                onClicked: window.shutdown.forceExit()
            }
        }
    }

    // After a session that crashed or was closed by force: where its log is.
    GlassDialog {
        id: previousSessionDialog
        objectName: "previousSessionDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(480, window.width - 32)
        title: window.logs && window.logs.previousSessionCrashed
               ? qsTr("ForeverTAS crashed last time")
               : qsTr("ForeverTAS did not close normally last time")
        modal: true
        Component.onCompleted: {
            if (window.logs && window.logs.previousSessionLog.length > 0)
                open()
        }
        onClosed: if (window.logs) window.logs.dismissPreviousSession()
        contentItem: ColumnLayout {
            spacing: 12
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("If something went wrong, send its log to the developer: it shows what happened.\n%1")
                      .arg(window.logs ? window.logs.previousSessionLog : "")
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                ThemedButton {
                    objectName: "previousSessionOpenLogsButton"
                    text: qsTr("Open log folder")
                    onClicked: window.logs.openFolder()
                }
                ThemedButton {
                    objectName: "previousSessionCloseButton"
                    text: qsTr("Close")
                    onClicked: previousSessionDialog.close()
                }
            }
        }
    }

    // ---- the vanishing car ------------------------------------------------------
    // Once a second, while the car camera follows the selected car, what makes
    // the car visible is checked; the log gets each change in what fails.
    // Help > Check the car also tests the picture itself.
    function selectedCarProblems() {
        const problems = []
        const root = selectedRunCarRoot
        const style = root.layerStyle
        const p = viewer.carPosition
        const q = viewer.carRotation
        if (!style.visible)
            problems.push("its style is hidden in Graphics (car:" + viewer.selectedRunStyleId + ")")
        if (style.opacity < 0.05)
            problems.push("opacity " + style.opacity)
        if (!isFinite(p.x) || !isFinite(p.y) || !isFinite(p.z))
            problems.push("position not finite")
        const length = Math.sqrt(q.scalar * q.scalar + q.x * q.x + q.y * q.y + q.z * q.z)
        if (!isFinite(length) || Math.abs(length - 1) > 0.01)
            problems.push("rotation length " + length)
        if (viewer.carEllipsoids.length === 0)
            problems.push("no car shape")
        if (!viewer.selectedEllipsoidFilledGeometry)
            problems.push("no car geometry")
        if (!style.throughBlocks) {
            if (!root.visible)
                problems.push("car node hidden")
            for (let node = root.parent; node; node = node.parent) {
                if (node.visible === false) {
                    problems.push("a parent is hidden: " + (node.objectName || String(node)))
                    break
                }
            }
        }
        const c = viewer.carCameraPosition
        const distance = Math.sqrt((p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y)
                                   + (p.z - c.z) * (p.z - c.z))
        if (!(distance < 60))
            problems.push("camera " + distance.toFixed(1) + " from the car")
        const f = viewport.cameraForward
        if ((p.x - c.x) * f.x + (p.y - c.y) * f.y + (p.z - c.z) * f.z < 0)
            problems.push("car behind the camera")
        return problems
    }
    function selectedCarState() {
        const p = viewer.carPosition
        const q = viewer.carRotation
        const c = viewer.carCameraPosition
        return "render " + renderMode + ", camera preset " + viewer.cameraPreset
               + (viewport.carCameraActive ? " (car camera" + (viewer.hideSelectedCar ? ", internal)" : ")")
                                           : " (not the car camera)")
               + ", module car " + moduleCarShown + ", run " + viewer.selectedRunId
               + " style " + viewer.selectedRunStyleId
               + ", car (" + p.x.toFixed(2) + ", " + p.y.toFixed(2) + ", " + p.z.toFixed(2) + ")"
               + " rot (" + q.scalar.toFixed(3) + ", " + q.x.toFixed(3) + ", " + q.y.toFixed(3) + ", " + q.z.toFixed(3) + ")"
               + ", camera (" + c.x.toFixed(2) + ", " + c.y.toFixed(2) + ", " + c.z.toFixed(2) + ")"
               + ", time " + viewer.timeMs + " ms" + (viewer.playing ? " playing" : "")
    }
    readonly property bool selectedCarExpected: viewer.loaded && viewer.runCount > 0
                                                && viewport.carCameraActive && !viewer.hideSelectedCar
                                                && !rayTracingEnabled && !moduleCarShown
    property string carWatchState: "?"
    Timer {
        objectName: "carWatch"
        interval: 1000
        repeat: true
        running: window.logs !== null && window.viewer.loaded && window.viewer.runCount > 0
        onTriggered: {
            const state = window.selectedCarExpected ? window.selectedCarProblems().join("; ") : "-"
            if (state === window.carWatchState)
                return
            window.carWatchState = state
            if (state !== "-" && state !== "")
                window.logs.note("car check: the car cannot show: " + state + " [" + window.selectedCarState() + "]")
            else if (state === "")
                window.logs.note("car check: everything for the car to show is in place")
        }
    }
    // The car probe: every 2 s, while the car camera follows a car that should
    // show and every check above passes, the race view's picture (as drawn,
    // never drawn again or with the car hidden) is read around the car's
    // center for the car's color. Missing twice in a row is logged with the
    // viewer's state and the picture (at most 3 a session); and again when
    // the car is back.
    property int carProbeMisses: 0
    property real carProbeMissingSince: 0
    property int carProbePictures: 0
    property string carProbeNoted: ""
    function noteCarProbe(key, text) {
        if (carProbeNoted === key)
            return
        carProbeNoted = key
        logs.note(text)
    }
    Timer {
        objectName: "carProbe"
        interval: 2000
        repeat: true
        running: window.logs !== null && window.selectedCarExpected && window.visible
                 && rasterMapView.visible && rasterMapView.width > 0
        onTriggered: {
            if (window.carWatchState !== "")
                return   // the state checks already explain it
            const style = selectedRunCarRoot.layerStyle
            if (style.throughBlocks || style.opacity < 0.5)
                return
            const center = rasterMapView.mapFrom3DScene(window.viewer.carPosition)
            const c = window.viewer.carCameraPosition
            const p = window.viewer.carPosition
            const distance = Math.sqrt((p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y)
                                       + (p.z - c.z) * (p.z - c.z))
            if (!(distance > 0.5) || center.z < 0)
                return
            // a quarter-size picture; half a meter around the car's center
            const scale = 0.25
            const pixelsPerMeter = rasterMapView.height
                / (2 * distance * Math.tan(viewCamera.fieldOfView * Math.PI / 360))
            const radius = Math.max(2, 0.5 * pixelsPerMeter * scale)
            const size = Qt.size(Math.round(rasterMapView.width * scale),
                                 Math.round(rasterMapView.height * scale))
            const color = style.color
            rasterMapView.grabToImage(result => {
                const probe = window.logs.carPixels(result.image,
                                                    Qt.point(center.x * scale, center.y * scale),
                                                    radius, color)
                if (probe.skipped !== undefined) {
                    window.noteCarProbe("skipped", "car probe: not checking: " + probe.skipped)
                    return
                }
                if (probe.fraction >= 0.25) {
                    if (window.carProbeMisses >= 2)
                        window.logs.note("car probe: the car is back in the picture after "
                                         + ((Date.now() - window.carProbeMissingSince) / 1000).toFixed(1)
                                         + " s (" + Math.round(probe.fraction * 100) + "% car pixels)")
                    window.carProbeMisses = 0
                    window.noteCarProbe("seen", "car probe: the car is in the picture ("
                                        + Math.round(probe.fraction * 100) + "% car pixels)")
                    return
                }
                if (probe.fraction > 0.05)
                    return   // partly hidden: not a verdict
                if (++window.carProbeMisses === 1) {
                    window.carProbeMissingSince = Date.now()
                    return
                }
                if (window.carProbeMisses !== 2)
                    return
                const stats = rasterMapView.renderStats
                window.logs.note("car probe: THE CAR IS NOT IN THE PICTURE although everything for it to show"
                                 + " is in place: " + Math.round(probe.fraction * 100) + "% car pixels of "
                                 + probe.samples + " around (" + Math.round(center.x) + ", "
                                 + Math.round(center.y) + "), " + distance.toFixed(1) + " m from the camera;"
                                 + " car node visible " + selectedRunCarRoot.visible
                                 + " at " + selectedRunCarRoot.scenePosition
                                 + ", " + window.viewer.carEllipsoids.length + " parts"
                                 + ", style color " + color + " opacity " + style.opacity
                                 + "; view " + rasterMapView.width + "x" + rasterMapView.height
                                 + " " + stats.fps + " fps"
                                 + " [" + window.selectedCarState() + "]")
                if (window.carProbePictures < 3) {
                    ++window.carProbePictures
                    rasterMapView.grabToImage(full => {
                        const path = window.logs.savePicture(full.image,
                                                             "car-missing-" + window.carProbePictures)
                        window.logs.note("car probe: the picture is " + path)
                    })
                }
            }, size)
        }
    }

    // The In-game car: the module draws the game's car from its own run of the
    // inputs. Every 2 s while the car camera follows it: was it asked to draw
    // the car, is its car where ForeverTAS has the run at that frame's time,
    // is the camera near it, is the frame the time shown. Changes are logged.
    // (Its picture is checked by Help > Check the car: the game's skin has no
    // color to look for.)
    readonly property bool moduleCarExpected: moduleCarShown && viewer.loaded && viewer.runCount > 0
                                              && viewport.carCameraActive && !viewer.hideSelectedCar
    property string moduleCarState: "?"
    Timer {
        objectName: "moduleCarProbe"
        interval: 2000
        repeat: true
        running: window.logs !== null && window.moduleCarExpected && window.visible
        onTriggered: {
            const car = moduleRenderView.carState()
            const problems = []
            let gap = -1
            if (!car.available) {
                problems.push("the In-game view has not reported its car (older module or no frame yet)")
            } else {
                if (!car.shown)
                    problems.push("the In-game view was told not to draw the car")
                // the module's car against the run at the module frame's time
                const p = window.viewer.selectedRunPositionAt(car.raceTimeMs)
                const q = car.position
                gap = Math.sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y)
                                + (p.z - q.z) * (p.z - q.z))
                if (!(gap < 5))
                    problems.push("the In-game car is " + gap.toFixed(1)
                                  + " m from the run at the same time (module car " + q + ", tick "
                                  + car.tick + ", " + car.controls + " controls)")
                // a frame left behind shows the car where the camera no longer is
                const behind = Math.abs(car.raceTimeMs - window.viewer.presentationTimeMs)
                if (!window.viewer.playing && behind > 50)
                    problems.push("the In-game frame shows " + car.raceTimeMs.toFixed(0)
                                  + " ms, not the " + window.viewer.presentationTimeMs.toFixed(0)
                                  + " ms shown (" + car.frames + " frames drawn)")
                const c = car.cameraPosition
                const seen = Math.sqrt((q.x - c.x) * (q.x - c.x) + (q.y - c.y) * (q.y - c.y)
                                       + (q.z - c.z) * (q.z - c.z))
                if (!(seen < 60))
                    problems.push("the In-game camera is " + seen.toFixed(1) + " m from the In-game car")
            }
            const state = problems.join("; ")
            if (state !== window.moduleCarState) {
                window.moduleCarState = state
                window.logs.note(state === ""
                    ? "in-game car check: drawn, where the camera looks (" + gap.toFixed(2) + " m apart)"
                    : "in-game car check: the car cannot show: " + state + " [" + window.selectedCarState() + "]")
            }
        }
    }

    // The In-game picture of rect (item coordinates) as the module drew it:
    // then(image), or then(null) after 2 s without one.
    function moduleCapture(rect, then) {
        const serial = moduleRenderView.requestPicture(rect)
        const started = Date.now()
        const poll = () => {
            const picture = moduleRenderView.picture()
            if (picture.serial >= serial)
                then(picture.image)
            else if (Date.now() - started > 2000)
                then(null)
            else
                moduleCaptureTimer.later(poll)
        }
        moduleCaptureTimer.later(poll)
    }
    Timer {
        id: moduleCaptureTimer
        property var pending: []
        interval: 30
        function later(f) {
            pending.push(f)
            if (!running)
                start()
        }
        onTriggered: {
            const calls = pending
            pending = []
            for (const f of calls)
                f()
        }
    }

    // Calls then(true) once the In-game view has drawn a frame after frame
    // number "frames", then(false) after 3 s without one.
    function afterModuleFrame(frames, then) {
        moduleFrameWait.frames = frames
        moduleFrameWait.then = then
        moduleFrameWait.started = Date.now()
        moduleFrameWait.restart()
    }
    Timer {
        id: moduleFrameWait
        property int frames: 0
        property var then: null
        property real started: 0
        interval: 50
        repeat: true
        onTriggered: {
            const drawn = moduleRenderView.carState().frames > frames
            if (!drawn && Date.now() - started < 3000)
                return
            stop()
            // the frame is drawn; the next scene graph frame shows it
            const then = moduleFrameWait.then
            Qt.callLater(() => then(drawn))
        }
    }

    // Help > Check the car: the picture with and without the car.
    function checkCar() {
        // the two pictures must show the same moment: paused, settled
        if (viewer.playing) {
            viewer.pause()
            checkCarLater.restart()
            return
        }
        const problems = selectedCarExpected ? selectedCarProblems() : []
        logs.note("check the car: " + (problems.length ? problems.join("; ") : "no problem found")
                  + " [" + selectedCarState() + "]")
        if (moduleCarShown && viewer.loaded && viewer.runCount > 0) {
            // the In-game view drawn with and without its car
            const car = moduleRenderView.carState()
            logs.note("check the car (In-game): " + JSON.stringify(car) + " [" + selectedCarState() + "]")
            const failed = qsTr("The In-game view did not draw a new frame to compare with. What the viewer was doing is in the log: send it (Help > Open log folder) with what you did before.")
            const whole = Qt.rect(0, 0, moduleRenderView.width, moduleRenderView.height)
            moduleCapture(whole, withCar => {
                if (withCar === null) {
                    carCheckDialog.show(failed)
                    return
                }
                logs.keepPicture(withCar)
                moduleCarCheckHidden = true
                afterModuleFrame(moduleRenderView.carState().frames, drawn => {
                    if (!drawn) {
                        moduleCarCheckHidden = false
                        logs.note("check the car (In-game): no new frame without the car")
                        carCheckDialog.show(failed)
                        return
                    }
                    moduleCapture(whole, withoutCar => {
                        moduleCarCheckHidden = false
                        if (withoutCar === null) {
                            carCheckDialog.show(failed)
                            return
                        }
                        const pixels = logs.differingPixels(withoutCar)
                        logs.note("check the car (In-game): " + pixels + " pixels show the car")
                        if (pixels <= 30)
                            logs.note("check the car (In-game): the pictures are "
                                      + logs.savePicture(withoutCar, "check-in-game-without-car") + " and "
                                      + logs.saveKeptPicture("check-in-game-with-car"))
                        carCheckDialog.show(pixels > 30
                            ? qsTr("The car is in the picture (%1 pixels).").arg(pixels)
                            : qsTr("The car is not in the picture. What the viewer was doing is in the log: send it (Help > Open log folder) with what you did before."))
                    })
                })
            })
            return
        }
        if (rayTracingEnabled || !rasterMapView.visible || !viewer.loaded || viewer.runCount === 0) {
            carCheckDialog.show(qsTr("Open a replay and use a raster render mode (not ray traced) to check the car."))
            return
        }
        rasterMapView.grabToImage(withCar => {
            logs.keepPicture(withCar.image)
            selectedRunCarRoot.checkHidden = true
            rasterMapView.grabToImage(withoutCar => {
                selectedRunCarRoot.checkHidden = false
                const pixels = logs.differingPixels(withoutCar.image)
                logs.note("check the car: " + pixels + " pixels show the car")
                if (pixels <= 30)
                    logs.note("check the car: the pictures are "
                              + logs.savePicture(withoutCar.image, "check-without-car") + " and "
                              + logs.saveKeptPicture("check-with-car"))
                carCheckDialog.show(pixels > 30
                    ? qsTr("The car is in the picture (%1 pixels).").arg(pixels)
                    : qsTr("The car is not in the picture%1. What the viewer was doing is in the log: send it (Help > Open log folder) with what you did before.")
                          .arg(problems.length ? " (" + problems.join("; ") + ")" : ""))
            })
        })
    }
    Timer {
        id: checkCarLater
        interval: 500
        onTriggered: window.checkCar()
    }
    GlassDialog {
        id: carCheckDialog
        objectName: "carCheckDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(460, window.width - 32)
        title: qsTr("Check the car")
        modal: true
        property string message: ""
        function show(text) { message = text; open() }
        contentItem: ColumnLayout {
            spacing: 12
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: carCheckDialog.message
            }
            ThemedButton {
                Layout.alignment: Qt.AlignRight
                text: qsTr("Close")
                onClicked: carCheckDialog.close()
            }
        }
    }

    property string renderMode: "textured"
    VideoExportDialog {
        id: videoExportDialog
        objectName: "videoExportDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(440, window.width - 32)
        viewer: window.viewer
    }
    property bool codeEditorExpanded: false
    readonly property bool rayTracingEnabled:
        renderMode === "textured-rt"
    // The map's own textures; a render module's mode shows them too until
    // its first frame.
    readonly property bool texturedMap: renderMode === "textured"
                                        || renderMode === "module"
    readonly property bool moduleMapShown: renderMode === "module"
                                           && moduleRenderView.ready
    // The render module draws the selected run's car as the game does,
    // in place of ForeverTAS's.
    property bool moduleGameCar: String(window.controller.uiSetting(
                                     "moduleGameCar", true)) !== "false"
    readonly property bool moduleCarShown: moduleMapShown && moduleGameCar
    // Help > Check the car: one In-game frame without the car
    property bool moduleCarCheckHidden: false
    readonly property real measuredFps: rayTracingEnabled
        ? rayTracingTrajectoryOverlay.renderStats.fps : rasterMapView.renderStats.fps
    onRenderModeChanged: editHistory.noteChange()
    readonly property var settingsWheelRedirectorObject:
        settingsWheelRedirector

    EditHistory {
        id: editHistory
        objectName: "editHistory"
        controller: window.controller
        viewer: window.viewer
        renderMode: window.renderMode
        onRenderModeRequested: mode => window.renderMode = mode
    }

    Connections {
        target: window.controller
        function onPacksDirectoryChanged() { editHistory.noteChange() }
        function onReplayPathChanged() { editHistory.noteChange() }
        function onBaseInputScriptChanged() {
            if (editHistory.restoring || !baseInputScriptArea.activeFocus)
                baseInputScriptArea.text = window.controller.baseInputScript
            editHistory.noteChange()
        }
        function onSimulationBackendIdChanged() { editHistory.noteChange() }
        function onSimulationHorizonMsChanged() { editHistory.noteChange() }
        function onConditionScriptChanged() { editHistory.noteChange() }
        function onCpuWorkerCountChanged() { editHistory.noteChange() }
        function onCudaParallelSampleCountChanged() { editHistory.noteChange() }
        function onCudaCalibrationEnabledChanged() { editHistory.noteChange() }
        function onHipParallelSampleCountChanged() { editHistory.noteChange() }
        function onHipCalibrationEnabledChanged() { editHistory.noteChange() }
        function onRandomizeSeedsOnStartChanged() { editHistory.noteChange() }
        function onDrawTargetsThroughBlocksChanged() { editHistory.noteChange() }
        function onSearchAlgorithmIdChanged() { editHistory.noteChange() }
        function onSearchAlgorithmSettingsChanged() { editHistory.noteChange() }
        function onModifierPassesChanged() { editHistory.noteChange() }
        function onEvaluationTargetIdChanged() { editHistory.noteChange() }
        function onEvaluationTargetSettingsChanged() { editHistory.noteChange() }
    }

    Connections {
        target: window.controller.cuboidTargets
        function onTargetsChanged() { editHistory.noteChange() }
        function onSelectedTargetChanged() { editHistory.noteChange() }
    }
    Connections {
        target: window.controller.customVolumeTargets
        function onTargetsChanged() { editHistory.noteChange() }
        function onSelectedTargetChanged() { editHistory.noteChange() }
    }
    Connections {
        target: window.controller.poseTargets
        function onTargetsChanged() { editHistory.noteChange() }
        function onSelectedTargetChanged() { editHistory.noteChange() }
    }
    Connections {
        target: window.viewer
        function onVisualStylesChanged() { editHistory.noteChange() }
        function onTelemetryScriptChanged() { editHistory.noteChange() }
    }
    Connections {
        target: window.viewer.whiteboard
        function onItemsChanged() { editHistory.noteChange() }
        function onBoardsChanged() { editHistory.noteChange() }
        function onSelectionChanged() { editHistory.noteChange() }
        function onBoardSelectionChanged() { editHistory.noteChange() }
        function onActiveChanged() { editHistory.noteChange() }
        function onToolChanged() { editHistory.noteChange() }
        function onColorChanged() { editHistory.noteChange() }
        function onSizeChanged() { editHistory.noteChange() }
        function onMapKeyChanged() { editHistory.reset() }
    }

    function runColor(index) {
        const colors = ["#ff8a3d", "#3d8dff", "#63c77b", "#c57aeb",
                        "#e7c24f", "#54c7c1", "#f56f70", "#7186f1",
                        "#a8ca4d", "#e881bb", "#4eb1e8", "#b7a45b",
                        "#d95f57", "#936ddb", "#43a98b", "#cf6daf"]
        const normalized = Math.max(0, index) % colors.length
        return colors[normalized]
    }

    function visualStyle(id, defaults) {
        const revision = viewer.visualStyleRevision
        const saved = viewer.visualStyle(id)
        return {
            visible: saved.visible ?? true,
            throughBlocks: saved.throughBlocks ?? (defaults.throughBlocks ?? false),
            color: saved.color ?? defaults.color,
            width: saved.width ?? (defaults.width ?? 2),
            opacity: saved.opacity ?? (defaults.opacity ?? 1),
            linePattern: saved.linePattern ?? "solid"
        }
    }

    function overlayWindowRange(id) {
        if (id === "evaluation:window")
            return evaluationOverlay.windowRange("evaluation", -1)
        const index = Number(id.slice("modifier:".length))
        return evaluationOverlay.windowRange("modifier", index)
    }

    function hasThroughVisuals() {
        if (rayTracingEnabled)
            return true
        for (const path of viewer.trajectoryPaths) {
            const style = visualStyle(path.visualId,
                                      { color: path.color })
            if ((path.visible ?? true)
                && style.visible && style.throughBlocks)
                return true
        }
        for (const run of viewer.runOptions) {
            const style = visualStyle("car:" + run.id,
                                      { color: "#ffffff" })
            if (style.visible && style.throughBlocks)
                return true
        }
        if (viewer.improvementCarCount > 0) {
            const style = visualStyle("car:improvements", { color: "#ffffff" })
            if (style.visible && style.throughBlocks)
                return true
        }
        const targets = [
            ["cuboid", controller.cuboidTargets.targets],
            ["custom", controller.customVolumeTargets.targets],
            ["pose", controller.poseTargets.targets]
        ]
        for (const pair of targets) {
            const kind = pair[0]
            const entries = pair[1]
            for (const entry of entries) {
                const style = visualStyle("target:" + kind + ":" + entry.id,
                                          { color: "#ffffff",
                                            throughBlocks:
                                                controller.drawTargetsThroughBlocks })
                if (style.visible && style.throughBlocks)
                    return true
            }
        }
        return false
    }

    function commitBaseInputScript() {
        if (window.controller.baseInputScript !== baseInputScriptArea.text)
            window.controller.baseInputScript = baseInputScriptArea.text
    }

    function focusedTextEditor() {
        const item = window.activeFocusItem
        return item && typeof item.undo === "function" ? item : null
    }

    readonly property bool canUndoEdit:
        !window.controller.running
        && !window.viewer.manualDriving
        && editHistory.gestureDepth === 0
        && (editHistory.canUndo
            || (window.focusedTextEditor()?.canUndo ?? false))
    readonly property bool canRedoEdit:
        !window.controller.running
        && !window.viewer.manualDriving
        && editHistory.gestureDepth === 0
        && (editHistory.canRedo
            || (window.focusedTextEditor()?.canRedo ?? false))

    function undoEdit() {
        const editor = window.focusedTextEditor()
        if (editor?.canUndo)
            editor.undo()
        else
            editHistory.undo()
    }

    function redoEdit() {
        const editor = window.focusedTextEditor()
        if (editor?.canRedo)
            editor.redo()
        else
            editHistory.redo()
    }

    onActiveChanged: {
        if (!active) {
            window.viewer.releaseManualInputs()
            viewport.releaseFreeMovement()
        }
    }

    Binding {
        target: window.viewer.gamepad
        property: "active"
        value: window.active && window.controller.viewerVisible &&
               (!window.shutdown || !window.shutdown.closing) &&
               !preferencesDialog.visible &&
               (window.viewer.manualDriving ||
                (window.viewer.takeOverOnInput && window.viewer.playing))
    }

    Component.onCompleted: {
        // Some window systems place a new window themselves: put it back
        // once it is up too.
        window.placeSavedWindow()
        Qt.callLater(window.placeSavedWindow)
        Qt.callLater(function() {
            if (window.viewer.loaded)
                viewport.resetCameraFocus()
            else if (!window.viewer.loading && window.controller.packsDirectory.length
                     && window.controller.replayPath.length)
                window.viewer.loadMap(window.controller.packsDirectory,
                                      window.controller.replayPath)
        })
    }

    Connections {
        target: window.viewer

        function onSceneChanged() {
            viewport.resetCameraFocus()
        }
    }

    // The window opens as it was last closed: its size and place when not
    // maximized, and whether it was maximized or full screen.
    readonly property var savedWindow: {
        try {
            const saved = JSON.parse(String(window.controller.uiSetting("window", "")))
            return saved && saved.width > 0 && saved.height > 0 ? saved : null
        } catch (error) {
            return null
        }
    }
    property rect normalGeometry: savedWindow
        ? Qt.rect(savedWindow.x, savedWindow.y, savedWindow.width, savedWindow.height)
        : Qt.rect(0, 0, 1420, 820)
    property string windowState: savedWindow && savedWindow.state ? savedWindow.state : "normal"
    // What View > Full screen goes back to.
    property string stateBeforeFullScreen: savedWindow && savedWindow.before === "maximized"
                                           ? "maximized" : "normal"
    function toggleFullScreen() {
        if (visibility === Window.FullScreen) {
            visibility = stateBeforeFullScreen === "maximized" ? Window.Maximized : Window.Windowed
        } else {
            stateBeforeFullScreen = visibility === Window.Maximized ? "maximized" : "normal"
            visibility = Window.FullScreen
        }
    }
    width: normalGeometry.width
    height: normalGeometry.height
    minimumWidth: window.controller.viewerVisible ? 1240 : 390
    minimumHeight: 580
    visible: true
    visibility: windowState === "maximized" ? Window.Maximized
                : windowState === "fullscreen" ? Window.FullScreen
                : Window.Windowed
    // Puts the window back where it was, if a screen is still there (a
    // monitor may be gone; screens of unknown size count as there).
    function placeSavedWindow() {
        if (!savedWindow || window.visibility !== Window.Windowed && window.visible)
            return
        const r = savedWindow
        let known = false
        for (const screen of Qt.application.screens) {
            if (screen.width <= 0 || screen.height <= 0)
                continue
            known = true
            if (r.x + 40 < screen.virtualX + screen.width && r.x + r.width - 40 > screen.virtualX
                    && r.y >= screen.virtualY - 8 && r.y + 40 < screen.virtualY + screen.height) {
                known = false
                break
            }
        }
        if (known)
            return
        window.x = r.x
        window.y = r.y
    }
    onXChanged: windowSaveTimer.restart()
    onYChanged: windowSaveTimer.restart()
    onVisibilityChanged: windowSaveTimer.restart()
    // Saved once the window settles (while it maximizes, its size can
    // change before its state does), and each save rewrites the settings
    // file.
    Timer {
        id: windowSaveTimer
        interval: 500
        onTriggered: window.saveWindow()
    }
    function saveWindow() {
        windowSaveTimer.stop()
        if (visibility === Window.Windowed) {
            if (width < minimumWidth || height < minimumHeight)
                return
            normalGeometry = Qt.rect(x, y, width, height)
            windowState = "normal"
        } else if (visibility === Window.Maximized) {
            windowState = "maximized"
        } else if (visibility === Window.FullScreen) {
            windowState = "fullscreen"
        } else {
            return
        }
        const r = normalGeometry
        window.controller.setUiSetting("window", JSON.stringify({
            x: r.x, y: r.y, width: r.width, height: r.height, state: windowState,
            before: stateBeforeFullScreen
        }))
    }
    title: qsTr("ForeverTAS")
    color: AppTheme.window

    Connections {
        target: window.updater
        function onUpdateAvailableChanged() {
            if (window.updater.updateAvailable)
                updateDialog.open()
        }
    }

    GlassDialog {
        id: updateDialog
        objectName: "updateDialog"
        anchors.centerIn: parent
        modal: true
        width: Math.min(560, window.width - 48)
        title: qsTr("Update")
        closePolicy: window.updater && window.updater.downloading
                     ? Popup.NoAutoClose : Popup.CloseOnEscape

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: window.updater && window.updater.updateAvailable
                      ? qsTr("ForeverTAS %1 is available. You have %2.")
                            .arg(window.updater.latestVersion)
                            .arg(Qt.application.version)
                      : window.updater && window.updater.checking
                        ? qsTr("Checking for updates…")
                        : qsTr("ForeverTAS is up to date.")
                color: AppTheme.text
                wrapMode: Text.WordWrap
            }

            ProgressBar {
                Layout.fillWidth: true
                visible: window.updater && window.updater.downloading
                from: 0
                to: 100
                value: window.updater ? window.updater.downloadProgress : 0
            }

            Label {
                Layout.fillWidth: true
                visible: window.updater && window.updater.downloading
                text: window.updater
                      ? qsTr("Downloading %1%").arg(window.updater.downloadProgress)
                      : ""
                color: AppTheme.textMuted
            }

            Label {
                Layout.fillWidth: true
                visible: window.updater && window.updater.errorMessage.length > 0
                text: window.updater ? window.updater.errorMessage : ""
                color: AppTheme.error
                wrapMode: Text.WordWrap
            }
        }

        footer: DialogButtonBox {
            spacing: 8

            ThemedButton {
                text: qsTr("Release notes")
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: Qt.openUrlExternally(window.updater.releaseUrl)
            }

            ThemedButton {
                text: window.updater && window.updater.downloading
                      ? qsTr("Cancel download") : qsTr("Later")
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: {
                    if (window.updater && window.updater.downloading)
                        window.updater.cancelDownload()
                    else
                        updateDialog.close()
                }
            }

            ThemedButton {
                objectName: "installUpdateButton"
                text: qsTr("Install")
                highlighted: true
                enabled: window.updater && window.updater.updateAvailable
                         && !window.updater.downloading
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: window.updater.downloadAndInstall()
            }

            background: Rectangle { color: AppTheme.panel }
        }
    }

    GlassDialog {
        id: replaceBaseInputScriptDialog

        objectName: "replaceBaseInputScriptDialog"
        anchors.centerIn: parent
        modal: true
        title: qsTr("Replace base input script?")
        onAccepted: window.controller.extractReplayInputs()

        footer: DialogButtonBox {
            spacing: 8

            ThemedButton {
                objectName: "confirmReplaceBaseInputButton"
                text: qsTr("Yes")
                highlighted: true
                DialogButtonBox.buttonRole: DialogButtonBox.YesRole
            }

            ThemedButton {
                objectName: "cancelReplaceBaseInputButton"
                text: qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }

            onAccepted: replaceBaseInputScriptDialog.accept()
            onRejected: replaceBaseInputScriptDialog.reject()

            background: Rectangle {
                color: AppTheme.panel
            }
        }

        Label {
            width: 360
            text: qsTr("Extracting replay inputs will replace the current script.")
            wrapMode: Text.WordWrap
        }
    }

    GlassDialog {
        id: telemetryEditorDialog
        objectName: "telemetryEditorDialog"
        anchors.centerIn: parent
        modal: true
        width: Math.min(620, window.width - 48)
        title: qsTr("Scripted telemetry")
        onOpened: {
            telemetryScriptEditor.text = window.viewer.telemetryScript
            telemetryScriptEditor.forceActiveFocus()
        }
        onAccepted:
            window.viewer.telemetryScript = telemetryScriptEditor.text

        footer: DialogButtonBox {
            spacing: 8

            ThemedButton {
                objectName: "resetTelemetryScriptButton"
                text: qsTr("Reset")
                DialogButtonBox.buttonRole: DialogButtonBox.ResetRole
                onClicked:
                    telemetryScriptEditor.text =
                        window.viewer.defaultTelemetryScript
            }

            ThemedButton {
                objectName: "cancelTelemetryScriptButton"
                text: qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }

            ThemedButton {
                objectName: "applyTelemetryScriptButton"
                text: qsTr("Apply")
                highlighted: true
                enabled: telemetryScriptErrorLabel.text.length === 0
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }

            onAccepted: telemetryEditorDialog.accept()
            onRejected: telemetryEditorDialog.reject()

            background: Rectangle {
                color: AppTheme.panel
            }
        }

        contentItem: ColumnLayout {
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                StyledComboBox {
                    id: telemetryFieldCombo
                    objectName: "telemetryFieldCombo"
                    Layout.fillWidth: true
                    model: [
                        "{camera.x:2}", "{camera.y:2}", "{camera.z:2}",
                        "{car.x:2}", "{car.y:2}", "{car.z:2}",
                        "{car.velocity.x:2}", "{car.velocity.y:2}",
                        "{car.velocity.z:2}", "{car.speed:2}",
                        "{car.speedKph:2}", "{input.accelerate:2}",
                        "{input.brake:2}", "{input.steering:2}",
                        "{race.checkpoints:0}",
                        "{race.totalCheckpoints:0}", "{race.laps:0}",
                        "{race.totalLaps:0}", "{race.finished}",
                        "{time.ms:0}", "{time.s:2}", "{tick:0}",
                        "{run.name}", "{target.readout}",
                        "{target.name}", "{target.summary}",
                        "{target.now}", "{target.score:2}",
                        "{stunt.score:0}", "{stunt.deadline:0}",
                        "{conditions.acceptance}",
                        "{car.speedKph / 3.6:2}",
                        "{abs(car.x - camera.x):2}",
                        "{max(car.speed, 20):1}"
                    ]
                    Accessible.name: qsTr("Telemetry field")
                }

                ThemedButton {
                    objectName: "insertTelemetryFieldButton"
                    text: qsTr("Insert")
                    onClicked: {
                        const token = telemetryFieldCombo.currentText
                        telemetryScriptEditor.insert(
                                    telemetryScriptEditor.cursorPosition,
                                    token)
                        telemetryScriptEditor.forceActiveFocus()
                    }
                }
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 150
                clip: true

                TextArea {
                    id: telemetryScriptEditor
                    objectName: "telemetryScriptEditor"
                    color: AppTheme.text
                    selectionColor: AppTheme.accent
                    selectedTextColor: AppTheme.textOnAccent
                    font.family: "monospace"
                    font.pixelSize: 12
                    wrapMode: TextEdit.Wrap
                    background: Rectangle {
                        color: AppTheme.surface
                        border.width: telemetryScriptEditor.activeFocus ? 2 : 1
                        border.color: telemetryScriptEditor.activeFocus
                                      ? AppTheme.focus : AppTheme.borderStrong
                        radius: 5
                    }
                }
            }

            Label {
                id: telemetryScriptErrorLabel
                objectName: "telemetryScriptErrorLabel"
                Layout.fillWidth: true
                visible: text.length > 0
                text: window.viewer.telemetryScriptError(
                          telemetryScriptEditor.text)
                color: AppTheme.error
                wrapMode: Text.WordWrap
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.minimumHeight: 42
                Layout.preferredHeight: Math.min(
                                            120,
                                            telemetryScriptPreview
                                                .implicitHeight + 18)
                color: AppTheme.viewerOverlay
                border.width: 1
                border.color: AppTheme.viewerOverlayBorder
                radius: 6
                clip: true

                Label {
                    id: telemetryScriptPreview
                    objectName: "telemetryScriptPreview"
                    anchors.fill: parent
                    anchors.margins: 9
                    text: telemetryEditorDialog.visible
                          ? viewport.renderTelemetry(
                                telemetryScriptEditor.text) : ""
                    textFormat: Text.RichText
                    color: AppTheme.viewerOverlayText
                    font.family: "monospace"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    palette {
        window: AppTheme.window
        windowText: AppTheme.text
        base: AppTheme.surface
        alternateBase: AppTheme.surfaceAlternate
        text: AppTheme.text
        brightText: AppTheme.text
        button: AppTheme.control
        buttonText: AppTheme.text
        light: AppTheme.surfaceRaised
        midlight: AppTheme.border
        mid: AppTheme.borderStrong
        dark: AppTheme.textFaint
        shadow: AppTheme.scrim
        highlight: AppTheme.accent
        highlightedText: AppTheme.textOnAccent
        placeholderText: AppTheme.textFaint
        toolTipBase: AppTheme.tooltipBackground
        toolTipText: AppTheme.tooltipText
    }

    Overlay.modal: Rectangle {
        color: AppTheme.scrim
    }

    Overlay.modeless: Rectangle {
        color: "transparent"
    }

    Shortcut {
        objectName: "saveBaseInputScriptShortcut"
        sequences: [StandardKey.Save]
        context: Qt.ApplicationShortcut
        enabled: baseInputScriptArea.activeFocus
        onActivated: window.commitBaseInputScript()
    }

    Shortcut {
        objectName: "undoBaseInputScriptShortcut"
        sequences: [StandardKey.Undo]
        context: Qt.ApplicationShortcut
        enabled: window.canUndoEdit
        onActivated: window.undoEdit()
    }

    Shortcut {
        objectName: "redoEditShortcut"
        sequence: "Ctrl+Shift+Z"
        context: Qt.ApplicationShortcut
        enabled: window.canRedoEdit
        onActivated: window.redoEdit()
    }


    // ---- glass UI: menus, zoom, saved look and layout -------------------------

    readonly property real menuBarHeight: 30
    // The text field last typed in, for Edit > Cut/Copy/Paste/Select all
    // (opening the menu takes the focus away from it).
    property Item lastTextEditor: null
    onActiveFocusItemChanged: {
        const item = window.activeFocusItem
        if (item && typeof item.selectAll === "function"
                && typeof item.paste === "function")
            window.lastTextEditor = item
    }

    function editText(action) {
        const editor = window.lastTextEditor
        if (!editor || !editor.visible)
            return
        editor.forceActiveFocus()
        editor[action]()
    }

    function setZoom(value) {
        AppTheme.zoom = Math.round(Math.max(0.7, Math.min(1.6, value)) * 10) / 10
    }

    readonly property bool canOpenMap: !window.viewer.loading
                                       && !window.viewer.manualDriving
                                       && !window.controller.running
                                       && !window.controller.extractingReplayInputs
    readonly property bool canExtractInputs: window.controller.canExtractReplayInputs
                                             && !window.viewer.loading
                                             && !window.viewer.manualDriving

    function loadMapIfReady() {
        if (window.canOpenMap
                && window.controller.packsDirectory.length > 0
                && window.controller.replayPath.length > 0)
            window.viewer.loadMap(window.controller.packsDirectory,
                                  window.controller.replayPath)
    }

    function openMapOrReplay() {
        if (!window.controller.browseForReplay())
            return
        if (window.controller.packsDirectory.length === 0) {
            // Needed first; the map loads when the dialog closes.
            packsDialog.loadAfter = true
            packsDialog.open()
            return
        }
        window.loadMapIfReady()
    }

    function extractInputs() {
        if (window.controller.baseInputScript.trim().length > 0)
            replaceBaseInputScriptDialog.open()
        else
            window.controller.extractReplayInputs()
    }

    // Code runs the simulation debugger while its panel is on screen (it
    // used to run while the Code tab was selected).
    function updateSimulationDebugger() {
        if (codePanel.onTop) {
            if (window.viewer.loaded)
                window.viewer.startSimulationDebugger()
        } else {
            window.codeEditorExpanded = false
            window.viewer.stopSimulationDebugger()
        }
    }

    // First run and View > Reset layout: Inputs over Graphics on the left,
    // Bruteforce and Code as tabs on the right, the race viewer header above
    // the race view, and the old race view overlays floating where they were.
    function defaultLayout(w, h) {
        const m = AppTheme.margin, g = AppTheme.gap, hh = AppTheme.headerHeight
        const left = 300, right = 390
        const headerHeight = hh + 52
        const x0 = m + left + g, x1 = m + w - right - g
        const y0 = m + headerHeight + g, y1 = m + h
        const mid = (x0 + x1) / 2
        function tabs(id, panels) {
            return { type: "tabs", id: id, tabs: panels, current: panels[0] }
        }
        function floating(id, panel, x, y, fw, fh) {
            return { id: id, tabs: [panel], current: panel,
                     rect: { x: x, y: y, w: fw, h: fh }, collapsed: false }
        }
        // The whiteboard toolbar gets taller while drawing (two rows of
        // buttons when narrow); leave it room.
        const whiteboardWidth = Math.min(520, Math.max(330, (x1 - x0 - 24) * 0.6))
        const whiteboardHeight = hh + 46
        const whiteboardRoom = hh + 164
        const telemetryHeight = hh + 46
        const playbackHeight = hh + 58
        // Camera sits top right of the race view, next to the whiteboard
        // when there is room, under it otherwise.
        const room = x1 - x0 - 24 - whiteboardWidth - g
        const cameraWidth = Math.max(260, Math.min(480, room))
        // Below 440 px the camera buttons wrap onto two rows.
        const cameraHeight = hh + (cameraWidth < 440 ? 80 : 44)
        const cameraY = room >= 260 ? y0 + 12 : y0 + 12 + whiteboardRoom + g
        return {
            version: 1,
            nextId: 40,
            root: {
                type: "split", id: "g1", dir: "row", ratio: left / Math.max(1, w - g), size: left,
                a: { type: "split", id: "g2", dir: "column", ratio: 0.5,
                     a: tabs("g3", ["inputs", "attempts"]), b: tabs("g4", ["graphics"]) },
                b: { type: "split", id: "g5", dir: "row",
                     ratio: (w - left - right - 2 * g) / Math.max(1, w - left - 2 * g),
                     size: right,
                     a: { type: "split", id: "g6", dir: "column",
                          ratio: headerHeight / Math.max(1, h - g), size: headerHeight,
                          a: tabs("g7", ["race"]),
                          b: { type: "center", id: "center" } },
                     b: { type: "split", id: "g9", dir: "column", ratio: 0.58,
                          a: tabs("g8", ["base", "target", "passes", "search", "code"]),
                          b: tabs("g10", ["results"]) } }
            },
            floating: [
                floating("g20", "whiteboard", x0 + 12, y0 + 12, whiteboardWidth, whiteboardHeight),
                floating("g21", "drawings", x0 + 12, y0 + 12 + whiteboardRoom + g,
                         330, 420),
                floating("g22", "camera", x1 - 12 - cameraWidth, cameraY, cameraWidth, cameraHeight),
                floating("g23", "telemetry", x1 - 12 - 300, cameraY + cameraHeight + g,
                         300, telemetryHeight),
                floating("g24", "splits", x1 - 12 - 230,
                         cameraY + cameraHeight + g + telemetryHeight + g, 230, hh + 230),
                floating("g25", "playback", mid - 215, y1 - 12 - playbackHeight, 430, playbackHeight),
                floating("g26", "manualDrive", mid - 130,
                         y1 - 12 - playbackHeight - g - (hh + 36), 260, hh + 36)
            ],
            closed: []
        }
    }

    // Zoom, glass look: saved preferences.
    QtObject {
        Component.onCompleted: {
            window.setZoom(Number(window.controller.uiSetting("zoom", 1.0)))
            AppTheme.darkening = Number(window.controller.uiSetting("glassDarkening", AppTheme.defaultDarkening))
            AppTheme.blur = Number(window.controller.uiSetting("glassBlur", AppTheme.defaultBlur))
            AppTheme.edgeBend = Number(window.controller.uiSetting("glassEdgeBend", AppTheme.defaultEdgeBend))
            AppTheme.shadowStrength = Number(window.controller.uiSetting("glassShadowStrength", AppTheme.defaultShadowStrength))
            AppTheme.shadowSize = Number(window.controller.uiSetting("glassShadowSize", AppTheme.defaultShadowSize))
            AppTheme.cornerRadius = Number(window.controller.uiSetting("glassCornerRadius", AppTheme.defaultCornerRadius))
            // Popups and dialogs live in the overlay: zoom it too.
            const overlay = window.Overlay.overlay
            overlay.transformOrigin = Item.TopLeft
            overlay.scale = Qt.binding(function() { return AppTheme.zoom })
            window.fitOverlay()
        }
    }
    function fitOverlay() {
        const overlay = window.Overlay.overlay
        const w = window.width / AppTheme.zoom, h = window.height / AppTheme.zoom
        if (overlay.width !== w)
            overlay.width = w
        if (overlay.height !== h)
            overlay.height = h
    }
    Connections {
        target: window.Overlay.overlay
        function onWidthChanged() { Qt.callLater(window.fitOverlay) }
        function onHeightChanged() { Qt.callLater(window.fitOverlay) }
    }
    Connections {
        target: AppTheme
        function onZoomChanged() {
            lookSaveTimer.restart()
            window.fitOverlay()
        }
        function onDarkeningChanged() { lookSaveTimer.restart() }
        function onBlurChanged() { lookSaveTimer.restart() }
        function onEdgeBendChanged() { lookSaveTimer.restart() }
        function onShadowStrengthChanged() { lookSaveTimer.restart() }
        function onShadowSizeChanged() { lookSaveTimer.restart() }
        function onCornerRadiusChanged() { lookSaveTimer.restart() }
    }
    // Saved once a slider stops moving: each save rewrites the settings file.
    Timer {
        id: lookSaveTimer
        interval: 500
        onTriggered: window.saveLook()
    }
    function saveLook() {
        lookSaveTimer.stop()
        window.controller.setUiSetting("zoom", AppTheme.zoom)
        window.controller.setUiSetting("glassDarkening", AppTheme.darkening)
        window.controller.setUiSetting("glassBlur", AppTheme.blur)
        window.controller.setUiSetting("glassEdgeBend", AppTheme.edgeBend)
        window.controller.setUiSetting("glassShadowStrength", AppTheme.shadowStrength)
        window.controller.setUiSetting("glassShadowSize", AppTheme.shadowSize)
        window.controller.setUiSetting("glassCornerRadius", AppTheme.cornerRadius)
    }
    onWidthChanged: {
        Qt.callLater(window.fitOverlay)
        windowSaveTimer.restart()
    }
    onHeightChanged: {
        Qt.callLater(window.fitOverlay)
        windowSaveTimer.restart()
    }

    Shortcut {
        objectName: "openMapShortcut"
        sequences: [StandardKey.Open]
        context: Qt.ApplicationShortcut
        enabled: window.canOpenMap
        onActivated: window.openMapOrReplay()
    }

    Shortcut {
        objectName: "fullScreenShortcut"
        sequence: "F11"
        context: Qt.ApplicationShortcut
        onActivated: window.toggleFullScreen()
    }

    Shortcut {
        objectName: "quitShortcut"
        sequences: [StandardKey.Quit]
        context: Qt.ApplicationShortcut
        onActivated: window.close()
    }

    // Tab hides every panel while the race view has the keyboard; in forms
    // it keeps moving between fields.
    Shortcut {
        objectName: "hideAllPanelsShortcut"
        sequence: "Tab"
        context: Qt.WindowShortcut
        enabled: !window.activeFocusItem
                 || window.activeFocusItem === manualInputFocus
                 || window.activeFocusItem === window.contentItem
        onActivated: dockSpace.hideAll = !dockSpace.hideAll
    }

    Shortcut {
        objectName: "zoomInShortcut"
        sequences: [StandardKey.ZoomIn, "Ctrl+="]
        context: Qt.ApplicationShortcut
        onActivated: window.setZoom(AppTheme.zoom + 0.1)
    }

    Shortcut {
        objectName: "zoomOutShortcut"
        sequences: [StandardKey.ZoomOut]
        context: Qt.ApplicationShortcut
        onActivated: window.setZoom(AppTheme.zoom - 0.1)
    }

    Shortcut {
        objectName: "resetZoomShortcut"
        sequence: "Ctrl+0"
        context: Qt.ApplicationShortcut
        onActivated: window.setZoom(1.0)
    }

    component PanelMenuItem: ThemedMenuItem {
        property string panelId
        showMark: true
        marked: dockSpace.layout.revision >= 0 && dockSpace.layout.isOpen(panelId)
        onTriggered: dockSpace.setOpen(panelId, !dockSpace.layout.isOpen(panelId))
    }

    component LookSlider: RowLayout {
        property alias text: label.text
        property alias from: slider.from
        property alias to: slider.to
        property alias value: slider.value
        signal moved(real value)
        Layout.fillWidth: true
        spacing: 10
        Label {
            id: label
            Layout.preferredWidth: 90
            color: AppTheme.text
        }
        ThemedSlider {
            id: slider
            Layout.fillWidth: true
            onMoved: parent.moved(value)
        }
    }

    // The glass look, as a settings dialog in the middle of the window; it
    // does not darken the window, so changes show as they are made.
    // The gamepad's buttons run the race's actions while the window has the focus (Preferences takes them while it
    // changes a binding).
    Connections {
        target: window.viewer.gamepad
        function onButtonChanged(button, pressed) {
            if (preferencesDialog.visible || (pressed && (!window.active || !window.controller.viewerVisible)))
                return
            raceControls.handlePad(button, pressed)
        }
    }

    RaceControls {
        id: raceControls
        controller: window.controller
        viewer: window.viewer
    }

    PreferencesDialog {
        id: preferencesDialog
        objectName: "preferencesDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(560, window.width - 32)
        raceControls: raceControls
        gamepad: window.viewer.gamepad
    }

    // m:ss.cc
    function raceTimeText(ms) {
        const centis = Math.max(0, Math.round(ms / 10))
        const minutes = Math.floor(centis / 6000)
        const seconds = Math.floor(centis / 100) % 60
        return minutes + ":" + String(seconds).padStart(2, "0") + "." + String(centis % 100).padStart(2, "0")
    }

    GlassDialog {
        id: glassLookPopup
        objectName: "glassLookPopup"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(380, window.width - 32)
        title: qsTr("Glass look")
        modal: true
        dim: false
        closePolicy: Popup.CloseOnEscape

        contentItem: ColumnLayout {
            spacing: 8

            LookSlider {
                text: qsTr("Darkening")
                from: 0
                to: 0.85
                value: AppTheme.darkening
                onMoved: value => AppTheme.darkening = value
            }
            LookSlider {
                text: qsTr("Blur")
                from: 0
                to: 1.0
                value: AppTheme.blur
                onMoved: value => AppTheme.blur = value
            }
            LookSlider {
                text: qsTr("Edge bend")
                from: 0
                to: 32
                value: AppTheme.edgeBend
                onMoved: value => AppTheme.edgeBend = value
            }
            LookSlider {
                text: qsTr("Shadow")
                from: 0
                to: 1
                value: AppTheme.shadowStrength
                onMoved: value => AppTheme.shadowStrength = value
            }
            LookSlider {
                text: qsTr("Shadow size")
                from: 0
                to: 1.5
                value: AppTheme.shadowSize
                onMoved: value => AppTheme.shadowSize = value
            }
            LookSlider {
                text: qsTr("Corners")
                from: 0
                to: 32
                value: AppTheme.cornerRadius
                onMoved: value => AppTheme.cornerRadius = value
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: 6

                ThemedButton {
                    objectName: "glassLookReset"
                    text: qsTr("Reset")
                    onClicked: {
                        AppTheme.darkening = AppTheme.defaultDarkening
                        AppTheme.blur = AppTheme.defaultBlur
                        AppTheme.edgeBend = AppTheme.defaultEdgeBend
                        AppTheme.shadowStrength = AppTheme.defaultShadowStrength
                        AppTheme.shadowSize = AppTheme.defaultShadowSize
                        AppTheme.cornerRadius = AppTheme.defaultCornerRadius
                    }
                }
                Item { Layout.fillWidth: true }
                ThemedButton {
                    objectName: "glassLookClose"
                    text: qsTr("Close")
                    onClicked: glassLookPopup.close()
                }
            }
        }
    }

    // ---- race view, with the glass panels over it ---------------------------
    // The race view fills the window; docked and floating panels both sit
    // over it, so docking never moves or resizes the view. The ambient layer
    // only shows when the view is smaller than the window.
    readonly property rect raceRect: Qt.rect(0, 0, Math.max(1, window.width),
                                             Math.max(1, window.height))
    // The part of the race view the cameras frame, as the race view of
    // releases: between the docked side columns, over the window's full
    // height (its header and transport sat over the picture). The rest of
    // the window carries the same picture on.
    readonly property rect framedRect: {
        const zoom = AppTheme.zoom
        const c = dockSpace.layout.center
        const a = dockSpace.area
        const g = AppTheme.gap
        const ox = dockSpace.x * zoom
        const left = c.x <= a.x + 0.5 ? 0 : ox + (c.x - g) * zoom
        const right = c.x + c.width >= a.x + a.width - 0.5
                      ? window.width : ox + (c.x + c.width + g) * zoom
        return Qt.rect(left, 0, Math.max(1, right - left),
                       Math.max(1, window.height))
    }
    // The ray from the camera through (x, y) of a race view, as its camera's
    // projection draws it. Qt's pick(), pickAll() and mapTo3DScene() leave
    // out the framing of a custom projection (off-centre, between docked
    // panels), so they missed what is drawn: gizmos could not be grabbed.
    function framedRay(view, x, y) {
        const camera = view.camera
        const p = camera.projection
        const nx = 2 * x / Math.max(1, view.width) - 1
        const ny = 1 - 2 * y / Math.max(1, view.height)
        const local = Qt.vector3d((nx + p.m13) / p.m11, (ny + p.m23) / p.m22, -1)
        return { origin: camera.scenePosition,
                 direction: camera.sceneRotation.times(local).normalized() }
    }
    // What (x, y) of a race view shows, nearest first: pickAll() as drawn.
    function framedPickAll(view, x, y) {
        const ray = framedRay(view, x, y)
        return view.rayPickAll(ray.origin, ray.direction)
                   .sort((a, b) => a.distance - b.distance)
    }
    // The nearest of them (with objectHit null when there is none): pick() as drawn.
    function framedPick(view, x, y) {
        const hits = framedPickAll(view, x, y)
        return hits.length ? hits[0] : { objectHit: null, scenePosition: Qt.vector3d(0, 0, 0), distance: Infinity }
    }
    // A camera's projection over the whole race view that shows the framed
    // part as a camera of that size and vertical field of view would.
    function framedProjection(fieldOfView, near, far) {
        const width = Math.max(1, raceRect.width)
        const height = Math.max(1, raceRect.height)
        const r = framedRect
        const focal = r.height / 2 / Math.tan(fieldOfView * Math.PI / 360)
        const cx = r.x + r.width / 2
        const cy = r.y + r.height / 2
        return Qt.matrix4x4(2 * focal / width, 0, 1 - 2 * cx / width, 0,
                            0, 2 * focal / height, 2 * cy / height - 1, 0,
                            0, 0, -(far + near) / (far - near),
                            -2 * far * near / (far - near),
                            0, 0, -1, 0)
    }

    Item {
        id: raceLayer
        objectName: "raceLayer"
        anchors.fill: parent

        Binding {
            target: AppTheme
            property: "glassAvailable"
            value: raceLayer.GraphicsInfo.api !== GraphicsInfo.Software
        }

        Rectangle {
            anchors.fill: parent
            color: AppTheme.window
        }

        // Hidden behind the race view; its texture feeds the glass.
        MultiEffect {
            id: blurredRace
            x: raceBackdrop.x
            y: raceBackdrop.y
            width: raceBackdrop.width
            height: raceBackdrop.height
            visible: AppTheme.glassAvailable
            source: raceBackdrop
            autoPaddingEnabled: false
            blurEnabled: true
            blur: 1.0
            blurMax: 64
            layer.enabled: AppTheme.glassAvailable
            layer.textureSize: Qt.size(Math.max(1, width / 2), Math.max(1, height / 2))
        }

        // The panels, blurred, for the glass of popups (lists, menus,
        // dialogs) so the text under them does not show through. Drawn
        // under the race view; captured only while such a popup is open.
        ShaderEffectSource {
            id: panelCapture
            anchors.fill: parent
            visible: AppTheme.glassAvailable
            sourceItem: ui
            live: AppTheme.panelViewers > 0
            hideSource: false
            textureSize: Qt.size(Math.max(1, width / 2), Math.max(1, height / 2))
        }
        MultiEffect {
            id: panelsBlurred
            anchors.fill: parent
            visible: AppTheme.glassAvailable
            source: panelCapture
            autoPaddingEnabled: false
            blurEnabled: true
            blur: 0.8
            blurMax: 48
            layer.enabled: AppTheme.glassAvailable && AppTheme.panelViewers > 0
            layer.textureSize: Qt.size(Math.max(1, width / 2), Math.max(1, height / 2))
        }

        ShaderEffect {
            id: ambient
            anchors.fill: parent
            visible: AppTheme.glassAvailable
            property vector2d itemSize: Qt.vector2d(width, height)
            property vector2d viewOrigin: Qt.vector2d(raceBackdrop.x, raceBackdrop.y)
            property vector2d viewSize: Qt.vector2d(raceBackdrop.width, raceBackdrop.height)
            property real dim: 0.05
            property var blurSource: blurredRace
            fragmentShader: "qrc:/glass/shaders/ambient.frag.qsb"
        }

        Item {
            id: raceBackdrop
            objectName: "raceBackdrop"
            x: window.raceRect.x
            y: window.raceRect.y
            width: window.raceRect.width
            height: window.raceRect.height
            layer.enabled: AppTheme.glassAvailable

            Rectangle {
                anchors.fill: parent
                color: AppTheme.window
            }

            Item {
                id: viewport
                objectName: "raceViewport"
                anchors.fill: parent
                visible: window.controller.viewerVisible
                         && !window.codeEditorExpanded
                onVisibleChanged: {
                    if (!visible) releaseFreeMovement()
                }

                property real orbitYaw: 35
                property real orbitPitch: -20
                property real orbitDistance: 38
                property real cameraFieldOfView: 55
                property bool freeCamera: false
                property bool orbitalCamera: false
                property bool pointPlacementActive: false
                readonly property bool targetMouseEditingAllowed:
                    !window.controller.running && !window.controller.targetMouseEditingLocked
                onTargetMouseEditingAllowedChanged: {
                    if (!targetMouseEditingAllowed) {
                        endCuboidInteraction()
                        endCustomInteraction()
                        endPoseInteraction()
                        pointPlacementActive = false
                        cuboidPointerCaptured = false
                        editHistory.endGesture()
                    }
                }
                Connections {
                    target: window.controller
                    function onEvaluationTargetIdChanged() {
                        if (!evaluationOverlay.showsPoint)
                            viewport.pointPlacementActive = false
                    }
                    function onRunningChanged() {
                        if (window.controller.running)
                            viewport.pointPlacementActive = false
                    }
                }
                readonly property vector3d sceneCameraPosition:
                    viewCamera.scenePosition
                readonly property var sceneCameraRotation:
                    viewCamera.sceneRotation
                // A car turned as the camera looks: a camera looks along its
                // -Z with its +X to the right, a car drives along its +Z with
                // its +X to the left (half a turn about the up axis apart).
                readonly property var sceneCameraCarRotation:
                    viewCamera.sceneRotation.times(Qt.quaternion(0, 0, 1, 0))
                property vector3d freeCameraPosition:
                    Qt.vector3d(0, 0, 0)
                function renderTelemetry(script) {
                    // These explicit reads keep the binding live while the
                    // C++ formatter samples the selected run internally.
                    const timelineTime = window.viewer.timeMs
                    const selectedCarPosition = window.viewer.carPosition
                    const context = evaluationOverlay.telemetryContext
                    const frame = evaluationOverlay.projectionFrame
                    return window.viewer.renderTelemetryWithContext(
                                script, frame ? frame[0] : viewCamera.scenePosition,
                                context, true)
                }
                readonly property bool carCameraActive:
                    !freeCamera && !orbitalCamera && !cuboidFocused
                    && window.viewer.carCameraAvailable
                    && window.viewer.runCount > 0
                readonly property vector3d cameraForward: {
                    if (carCameraActive) {
                        const dx = window.viewer.carCameraTarget.x
                                 - window.viewer.carCameraPosition.x
                        const dy = window.viewer.carCameraTarget.y
                                 - window.viewer.carCameraPosition.y
                        const dz = window.viewer.carCameraTarget.z
                                 - window.viewer.carCameraPosition.z
                        const length = Math.sqrt(dx * dx + dy * dy
                                               + dz * dz)
                        if (length > 0.000001)
                            return Qt.vector3d(dx / length, dy / length,
                                               dz / length)
                    }
                    const yaw = orbitYaw * Math.PI / 180
                    const pitch = orbitPitch * Math.PI / 180
                    const pitchCos = Math.cos(pitch)
                    return Qt.vector3d(
                        -Math.sin(yaw) * pitchCos,
                        Math.sin(pitch),
                        -Math.cos(yaw) * pitchCos)
                }
                readonly property vector3d freeCameraTarget:
                    Qt.vector3d(
                        freeCameraPosition.x
                            + cameraForward.x * orbitDistance,
                        freeCameraPosition.y
                            + cameraForward.y * orbitDistance,
                        freeCameraPosition.z
                            + cameraForward.z * orbitDistance)
                property bool hasObjectFocus: false
                property bool cuboidFocused: false
                property int exactWhiteboardBoardIndex: -1
                property string exactWhiteboardBoardId: ""
                property int lastFocusedWhiteboardIndex: -1
                property string lastFocusedWhiteboardId: ""
                property vector3d cuboidFocusCenter:
                    Qt.vector3d(0, 0, 0)
                readonly property vector3d cameraTarget:
                    freeCamera ? freeCameraTarget
                    : cuboidFocused ? cuboidFocusCenter
                    : carCameraActive
                      ? window.viewer.carCameraTarget
                      : window.viewer.carPosition
                readonly property string cameraFocusMode:
                    freeCamera ? "free"
                    : cuboidFocused ? "object"
                    : orbitalCamera ? "orbital"
                    : carCameraActive ? "preset" : "car"
                property bool freeMoveForward: false
                property bool freeMoveBackward: false
                property bool freeMoveLeft: false
                property bool freeMoveRight: false
                property bool freeMoveUp: false
                property bool freeMoveDown: false
                property real freeMoveSpeed: 0
                property real freeMoveStartedAt: 0
                property real freeMoveLastStepAt: 0
                readonly property real freeMoveInitialSpeed: 9
                readonly property real freeMoveAcceleration: 30
                readonly property real freeMoveMaximumSpeed: 360
                property real viewRotationTargetYaw: orbitYaw
                property real viewRotationTargetPitch: orbitPitch
                property bool viewRotationSmoothing: false
                property bool cuboidDragActive: false
                property bool cuboidPointerCaptured: false
                property string cuboidDragKind: ""
                property string cuboidDragAxis: ""
                property real cuboidDragX: 0
                property real cuboidDragY: 0
                property bool customVertexDragActive: false
                property int customVertexDragIndex: -1
                property bool customDepthDragActive: false
                property bool poseDragActive: false
                property string poseDragKind: ""
                property string poseDragAxis: ""
                property bool exportingWhiteboardImage: false
                property url whiteboardImageExportUrl: ""

                function hasFreeMovement() {
                    return freeMoveForward || freeMoveBackward
                        || freeMoveLeft || freeMoveRight
                        || freeMoveUp || freeMoveDown
                }

                function releaseFreeMovement() {
                    freeMoveForward = false
                    freeMoveBackward = false
                    freeMoveLeft = false
                    freeMoveRight = false
                    freeMoveUp = false
                    freeMoveDown = false
                    freeMoveSpeed = 0
                    freeMoveStartedAt = 0
                    freeMoveLastStepAt = 0
                    freeCameraMovementTimer.stop()
                }

                function setFreeMovement(direction, active) {
                    const hadMovement = hasFreeMovement()
                    if (direction === "forward")
                        freeMoveForward = active
                    else if (direction === "backward")
                        freeMoveBackward = active
                    else if (direction === "left")
                        freeMoveLeft = active
                    else if (direction === "right")
                        freeMoveRight = active
                    else if (direction === "up")
                        freeMoveUp = active
                    else if (direction === "down")
                        freeMoveDown = active
                    else
                        return false

                    const hasMovement = hasFreeMovement()
                    if (!hadMovement && hasMovement) {
                        const now = Date.now()
                        freeMoveStartedAt = now
                        freeMoveLastStepAt = now
                        freeMoveSpeed = freeMoveInitialSpeed
                        freeCameraMovementTimer.start()
                    } else if (!hasMovement) {
                        releaseFreeMovement()
                    }
                    return true
                }

                function freeMovementForKey(key) {
                    if (key === Qt.Key_Up || key === Qt.Key_W
                            || key === Qt.Key_Z)
                        return "forward"
                    if (key === Qt.Key_Down || key === Qt.Key_S)
                        return "backward"
                    if (key === Qt.Key_Left || key === Qt.Key_A
                            || key === Qt.Key_Q)
                        return "left"
                    if (key === Qt.Key_Right || key === Qt.Key_D)
                        return "right"
                    if (key === Qt.Key_E)
                        return "up"
                    if (key === Qt.Key_C)
                        return "down"
                    return ""
                }

                function handleCameraPresetKey(event, active) {
                    let preset = 0
                    if (event.key === Qt.Key_1)
                        preset = 1
                    else if (event.key === Qt.Key_2)
                        preset = 2
                    else if (event.key === Qt.Key_3)
                        preset = 3
                    else if (event.key === Qt.Key_7) {
                        event.accepted = true
                        if (active && !event.isAutoRepeat)
                            enableFreeCamera()
                        return true
                    } else {
                        return false
                    }
                    event.accepted = true
                    if (active && !event.isAutoRepeat
                            && window.viewer.runCount > 0
                            && window.viewer.carCameras.includes(preset)) {
                        window.viewer.cameraPreset = preset
                        focusCurrentCar()
                    }
                    return true
                }

                function handleFreeCameraKey(event, active) {
                    if (!freeCamera)
                        return false
                    const direction = freeMovementForKey(event.key)
                    if (direction.length === 0)
                        return false
                    event.accepted = true
                    if (!event.isAutoRepeat)
                        setFreeMovement(direction, active)
                    return true
                }

                function stepFreeCameraMovement(timestamp) {
                    if (!freeCamera) {
                        releaseFreeMovement()
                        return false
                    }
                    if (!hasFreeMovement())
                        return false
                    const now = Number.isFinite(timestamp)
                              ? timestamp : Date.now()
                    if (freeMoveStartedAt <= 0)
                        freeMoveStartedAt = now
                    if (freeMoveLastStepAt <= 0)
                        freeMoveLastStepAt = now
                    const elapsed = Math.max(
                        0, (now - freeMoveStartedAt) / 1000)
                    const delta = Math.max(
                        0,
                        Math.min(0.05,
                                 (now - freeMoveLastStepAt) / 1000))
                    freeMoveLastStepAt = now
                    freeMoveSpeed = Math.min(
                        freeMoveMaximumSpeed,
                        freeMoveInitialSpeed
                            + elapsed * freeMoveAcceleration)
                    if (delta <= 0)
                        return true

                    const yaw = orbitYaw * Math.PI / 180
                    const pitch = orbitPitch * Math.PI / 180
                    const pitchCos = Math.cos(pitch)
                    const forward = Qt.vector3d(
                        -Math.sin(yaw) * pitchCos,
                        Math.sin(pitch),
                        -Math.cos(yaw) * pitchCos)
                    const right = Qt.vector3d(
                        Math.cos(yaw), 0, -Math.sin(yaw))
                    let x = 0
                    let y = 0
                    let z = 0
                    if (freeMoveForward) {
                        x += forward.x
                        y += forward.y
                        z += forward.z
                    }
                    if (freeMoveBackward) {
                        x -= forward.x
                        y -= forward.y
                        z -= forward.z
                    }
                    if (freeMoveRight) {
                        x += right.x
                        z += right.z
                    }
                    if (freeMoveLeft) {
                        x -= right.x
                        z -= right.z
                    }
                    if (freeMoveUp)
                        y += 1
                    if (freeMoveDown)
                        y -= 1
                    const length = Math.sqrt(x * x + y * y + z * z)
                    if (length <= 0.000001)
                        return true
                    const distance = freeMoveSpeed * delta / length
                    freeCameraPosition = Qt.vector3d(
                        freeCameraPosition.x + x * distance,
                        freeCameraPosition.y + y * distance,
                        freeCameraPosition.z + z * distance)
                    return true
                }

                function zoomOrbit(wheelDeltaY) {
                    if (carCameraActive || freeCamera)
                        return false
                    leaveExactWhiteboardView()
                    const factor = Math.exp(-wheelDeltaY / 1200)
                    orbitDistance = Math.max(
                        3, Math.min(1000, orbitDistance * factor))
                    return true
                }

                function beginViewRotation() {
                    viewRotationTargetYaw = orbitYaw
                    viewRotationTargetPitch = orbitPitch
                    viewRotationSmoothing = false
                }

                function updateViewRotation(deltaX, deltaY) {
                    if (carCameraActive)
                        return
                    if (!freeCamera) {
                        orbitYaw -= deltaX
                        orbitPitch = Math.max(
                            -85, Math.min(85, orbitPitch - deltaY))
                        viewRotationTargetYaw = orbitYaw
                        viewRotationTargetPitch = orbitPitch
                        viewRotationSmoothing = false
                        return
                    }
                    viewRotationTargetYaw -= deltaX * 0.5
                    viewRotationTargetPitch = Math.max(
                        -85,
                        Math.min(85,
                                 viewRotationTargetPitch - deltaY * 0.5))
                    viewRotationSmoothing = true
                }

                function stepViewRotation(frameTime) {
                    if (!viewRotationSmoothing)
                        return false
                    const seconds = Math.max(
                        0,
                        Math.min(0.05,
                                 Number.isFinite(frameTime)
                                 ? frameTime : 1 / 60))
                    const blend = 1 - Math.exp(-24 * seconds)
                    orbitYaw += (viewRotationTargetYaw - orbitYaw) * blend
                    orbitPitch += (viewRotationTargetPitch - orbitPitch)
                        * blend
                    if (Math.abs(viewRotationTargetYaw - orbitYaw) < 0.001
                            && Math.abs(viewRotationTargetPitch
                                        - orbitPitch) < 0.001) {
                        orbitYaw = viewRotationTargetYaw
                        orbitPitch = viewRotationTargetPitch
                        viewRotationSmoothing = false
                    }
                    return true
                }

                function enableFreeCamera() {
                    window.viewer.releaseManualInputs()
                    orbitalCamera = false
                    const wasCarCamera = carCameraActive
                    const forward = cameraForward
                    const position = freeCamera
                        ? freeCameraPosition
                        : wasCarCamera
                          ? window.viewer.carCameraPosition
                          : Qt.vector3d(
                                cameraTarget.x
                                    - forward.x * orbitDistance,
                                cameraTarget.y
                                    - forward.y * orbitDistance,
                                cameraTarget.z
                                    - forward.z * orbitDistance)
                    if (wasCarCamera) {
                        const dx = window.viewer.carCameraTarget.x
                                 - position.x
                        const dy = window.viewer.carCameraTarget.y
                                 - position.y
                        const dz = window.viewer.carCameraTarget.z
                                 - position.z
                        orbitDistance = Math.max(
                            0.1, Math.sqrt(dx * dx + dy * dy + dz * dz))
                        orbitPitch = Math.asin(
                            Math.max(-1, Math.min(1, forward.y)))
                            * 180 / Math.PI
                        orbitYaw = Math.atan2(-forward.x, -forward.z)
                            * 180 / Math.PI
                    }
                    beginViewRotation()
                    freeCameraPosition = Qt.vector3d(
                        position.x, position.y, position.z)
                    exactWhiteboardBoardIndex = -1
                    exactWhiteboardBoardId = ""
                    cameraFieldOfView = 55
                    freeCamera = true
                    cuboidFocused = false
                }

                function leaveExactWhiteboardView() {
                    if (exactWhiteboardBoardIndex < 0)
                        return
                    exactWhiteboardBoardIndex = -1
                    exactWhiteboardBoardId = ""
                    cameraFieldOfView = 55
                }

                function enableOrbitalCamera() {
                    window.viewer.releaseManualInputs()
                    releaseFreeMovement()
                    beginViewRotation()
                    exactWhiteboardBoardIndex = -1
                    exactWhiteboardBoardId = ""
                    cameraFieldOfView = 55
                    freeCamera = false
                    cuboidFocused = false
                    orbitalCamera = true
                }

                function focusCurrentCar() {
                    releaseFreeMovement()
                    beginViewRotation()
                    exactWhiteboardBoardIndex = -1
                    exactWhiteboardBoardId = ""
                    cameraFieldOfView = 55
                    freeCamera = false
                    cuboidFocused = false
                    orbitalCamera = false
                }

                function resetCameraFocus() {
                    focusCurrentCar()
                    hasObjectFocus = false
                    lastFocusedWhiteboardIndex = -1
                    lastFocusedWhiteboardId = ""
                    cuboidFocusCenter = Qt.vector3d(0, 0, 0)
                }

                function whiteboardIndexForId(id) {
                    if (id.length === 0)
                        return -1
                    const boards = window.viewer.whiteboard.boards
                    for (let index = 0; index < boards.length; ++index) {
                        if (boards[index].id === id)
                            return index
                    }
                    return -1
                }

                function synchronizeWhiteboardFocus() {
                    if (lastFocusedWhiteboardId.length === 0)
                        return
                    const index = whiteboardIndexForId(
                        lastFocusedWhiteboardId)
                    const board = index >= 0
                        ? window.viewer.whiteboard.boards[index] : null
                    if (!board || !board.isCurrentMap) {
                        enableFreeCamera()
                        lastFocusedWhiteboardIndex = -1
                        lastFocusedWhiteboardId = ""
                        hasObjectFocus = false
                        return
                    }
                    lastFocusedWhiteboardIndex = index
                    if (exactWhiteboardBoardId ===
                            lastFocusedWhiteboardId) {
                        exactWhiteboardBoardIndex = index
                    }
                }

                function focusLastObject() {
                    if (!hasObjectFocus)
                        return
                    const whiteboardIndex = whiteboardIndexForId(
                        lastFocusedWhiteboardId)
                    if (whiteboardIndex >= 0) {
                        const board = window.viewer.whiteboard.boards[
                            whiteboardIndex]
                        if (board && board.isCurrentMap) {
                            restoreWhiteboardView(board)
                            return
                        }
                    }
                    releaseFreeMovement()
                    beginViewRotation()
                    exactWhiteboardBoardIndex = -1
                    exactWhiteboardBoardId = ""
                    cameraFieldOfView = 55
                    freeCamera = false
                    cuboidFocused = true
                    orbitalCamera = false
                }

                function focusCuboid(center, size) {
                    releaseFreeMovement()
                    beginViewRotation()
                    exactWhiteboardBoardIndex = -1
                    exactWhiteboardBoardId = ""
                    lastFocusedWhiteboardIndex = -1
                    lastFocusedWhiteboardId = ""
                    cameraFieldOfView = 55
                    cuboidFocusCenter = center
                    hasObjectFocus = true
                    freeCamera = false
                    cuboidFocused = true
                    orbitalCamera = false
                    orbitDistance = Math.max(
                        3,
                        Math.min(1000,
                                 Math.max(size.x, size.y, size.z) * 2.4))
                }

                function captureWhiteboardView() {
                    const camera = viewCamera.scenePosition
                    const target = cameraTarget
                    const viewportWidth = Math.max(1, width)
                    const viewportHeight = Math.max(1, height)
                    const contentX = 0
                    const contentY = Math.min(
                        1, whiteboardOverlay.boardTop / viewportHeight)
                    const contentWidth = 1
                    const contentHeight = Math.max(
                        0.000001, 1 - contentY)
                    const planeDistance = Math.max(
                        viewCamera.clipNear * 2,
                        Math.min(
                            Math.max(0.05, orbitDistance - 0.01),
                            Math.max(0.05, orbitDistance * 0.32)))
                    const fullHeight = 2 * Math.tan(
                        cameraFieldOfView * Math.PI / 360)
                        * planeDistance
                    const fullWidth = fullHeight
                        * viewportWidth / viewportHeight
                    const planeWidth = fullWidth * contentWidth
                    const planeHeight = fullHeight * contentHeight
                    const contentCenterX =
                        contentX + contentWidth * 0.5
                    const contentCenterY =
                        contentY + contentHeight * 0.5
                    const rightOffset =
                        fullWidth * (contentCenterX - 0.5)
                    const upOffset =
                        fullHeight * (0.5 - contentCenterY)
                    const yaw = orbitYaw * Math.PI / 180
                    const pitch = orbitPitch * Math.PI / 180
                    const right = Qt.vector3d(
                        Math.cos(yaw), 0, -Math.sin(yaw))
                    const up = Qt.vector3d(
                        Math.sin(yaw) * Math.sin(pitch),
                        Math.cos(pitch),
                        Math.cos(yaw) * Math.sin(pitch))
                    return {
                        "projection": "perspective-vertical",
                        "fieldOfView": cameraFieldOfView,
                        "planeDistance": planeDistance,
                        "viewportWidth": viewportWidth,
                        "viewportHeight": viewportHeight,
                        "contentX": contentX,
                        "contentY": contentY,
                        "contentWidth": contentWidth,
                        "contentHeight": contentHeight,
                        "canvasWidth": Math.max(
                            1, whiteboardOverlay.width),
                        "canvasHeight": Math.max(
                            1,
                            whiteboardOverlay.height
                            - whiteboardOverlay.boardTop),
                        "targetX": target.x,
                        "targetY": target.y,
                        "targetZ": target.z,
                        "yaw": orbitYaw,
                        "pitch": orbitPitch,
                        "distance": orbitDistance,
                        "planeX": camera.x
                                  + cameraForward.x * planeDistance
                                  + right.x * rightOffset
                                  + up.x * upOffset,
                        "planeY": camera.y
                                  + cameraForward.y * planeDistance
                                  + right.y * rightOffset
                                  + up.y * upOffset,
                        "planeZ": camera.z
                                  + cameraForward.z * planeDistance
                                  + right.z * rightOffset
                                  + up.z * upOffset,
                        "planeWidth": planeWidth,
                        "planeHeight": planeHeight
                    }
                }

                function restoreWhiteboardView(board) {
                    releaseFreeMovement()
                    cuboidFocusCenter = Qt.vector3d(
                        board.targetX,
                        board.targetY,
                        board.targetZ)
                    hasObjectFocus = true
                    freeCamera = false
                    cuboidFocused = true
                    orbitalCamera = false
                    orbitYaw = board.yaw
                    orbitPitch = board.pitch
                    beginViewRotation()
                    orbitDistance = board.distance
                    cameraFieldOfView =
                        board.projectionVersion >= 1
                        ? board.fieldOfView : 55
                    exactWhiteboardBoardIndex =
                        board.projectionVersion >= 1
                        ? board.boardIndex : -1
                    exactWhiteboardBoardId =
                        board.projectionVersion >= 1
                        ? board.id : ""
                    lastFocusedWhiteboardIndex = board.boardIndex
                    lastFocusedWhiteboardId = board.id
                }

                function whiteboardPlaneFocusEnabled() {
                    return !window.viewer.whiteboard.active
                        && !freeCamera
                }

                Timer {
                    id: freeCameraMovementTimer
                    interval: 16
                    repeat: true
                    onTriggered:
                        viewport.stepFreeCameraMovement(Date.now())
                }

                FrameAnimation {
                    running: viewport.viewRotationSmoothing
                             && window.visible && viewport.visible
                    onTriggered:
                        viewport.stepViewRotation(frameTime)
                }

                function finishWhiteboardImageExport(success) {
                    whiteboardExportTimer.stop()
                    whiteboardExportWatchdog.stop()
                    whiteboardPlaneView.forcedBoardIndex = -1
                    whiteboardPlaneView.exportMode = false
                    exportingWhiteboardImage = false
                    whiteboardImageExportUrl = ""
                    window.viewer.whiteboard.finishBoardImageExport(
                                success, true)
                }

                function exportWhiteboardBackground(index, fileUrl) {
                    if (exportingWhiteboardImage
                            || index < 0
                            || index >= window.viewer.whiteboard.boardCount) {
                        return false
                    }
                    const board = window.viewer.whiteboard.boards[index]
                    const path =
                            window.viewer.whiteboard.imageExportPath(fileUrl)
                    if (!board || !board.isCurrentMap
                            || path.length === 0
                            || !window.viewer.whiteboard.selectBoard(index)) {
                        return false
                    }
                    restoreWhiteboardView(board)
                    whiteboardImageExportUrl = fileUrl
                    whiteboardPlaneView.forcedBoardIndex = index
                    whiteboardPlaneView.exportMode = true
                    exportingWhiteboardImage = true
                    whiteboardExportTimer.restart()
                    return true
                }

                Timer {
                    id: whiteboardExportTimer
                    interval: 120
                    repeat: false
                    onTriggered: {
                        const path =
                                window.viewer.whiteboard.imageExportPath(
                                    viewport.whiteboardImageExportUrl)
                        if (path.length === 0) {
                            viewport.finishWhiteboardImageExport(false)
                            return
                        }
                        const started = viewport.grabToImage(
                            function(result) {
                                const saved = result !== null
                                        && window.viewer.whiteboard
                                               .saveBoardBackgroundImage(
                                                   result.image,
                                                   viewport
                                                       .whiteboardImageExportUrl)
                                viewport.finishWhiteboardImageExport(saved)
                            })
                        if (started) {
                            whiteboardExportWatchdog.restart()
                        } else {
                            viewport.finishWhiteboardImageExport(false)
                        }
                    }
                }

                Timer {
                    id: whiteboardExportWatchdog
                    interval: 10000
                    repeat: false
                    onTriggered:
                        viewport.finishWhiteboardImageExport(false)
                }

                // What a press at (x, y) grabs: a gizmo handle wins over the
                // see-through body of a box, volume or pose around it (seen
                // from inside or behind it, the handles lie within its
                // outline), but not over the map in front of both.
                function editorPick(view, x, y) {
                    const bodies = ["select", "custom-select", "pose-select"]
                    let first = null
                    for (const result of window.framedPickAll(view, x, y)) {
                        const hit = result.objectHit
                        if (!hit)
                            continue
                        if (hit.targetIndex === undefined)
                            return first !== null ? first : hit   // in front of the rest
                        if (hit.editorKind && bodies.indexOf(hit.editorKind) < 0)
                            return hit
                        if (first === null)
                            first = hit
                    }
                    return first
                }

                // How far a drag of (dx, dy) pixels moves a target along a
                // world axis, in metres: the axis as the camera shows it at
                // origin (car, orbit or free camera alike); an axis pointing
                // at the camera takes the vertical drag.
                function axisDragAmount(origin, axis, dx, dy) {
                    const unit = axis === "x" ? Qt.vector3d(1, 0, 0)
                               : axis === "y" ? Qt.vector3d(0, 1, 0)
                               : Qt.vector3d(0, 0, 1)
                    const a = rasterMapView.mapFrom3DScene(origin)
                    const b = rasterMapView.mapFrom3DScene(origin.plus(unit))
                    let sx = b.x - a.x
                    let sy = b.y - a.y
                    let lengthSquared = sx * sx + sy * sy
                    if (!isFinite(lengthSquared) || lengthSquared < 1e-6) {
                        // no projection to go by (the 3D view not drawn):
                        // the orbit camera's angles, as before
                        const scale = orbitDistance / Math.max(200, Math.min(width, height))
                        const yaw = orbitYaw * Math.PI / 180
                        const pitch = orbitPitch * Math.PI / 180
                        if (axis === "x")
                            return (dx * Math.cos(yaw) - dy * Math.sin(yaw) * Math.sin(pitch)) * scale
                        if (axis === "y")
                            return -dy * Math.cos(pitch) * scale
                        return (-dx * Math.sin(yaw) - dy * Math.cos(yaw) * Math.sin(pitch)) * scale
                    }
                    if (!(lengthSquared >= 64)) {
                        // the axis seen nearly end-on: along its direction
                        // on screen if it has one (else the vertical drag),
                        // at the speed of a metre at that distance
                        const distance = viewCamera.scenePosition.minus(origin).length()
                        const perMetre = rasterMapView.height
                            / (2 * Math.max(0.5, distance)
                               * Math.tan(viewCamera.fieldOfView * Math.PI / 360))
                        const length = Math.sqrt(lengthSquared)
                        const ux = length > 1e-3 ? sx / length : 0
                        const uy = length > 1e-3 ? sy / length : -1
                        return (dx * ux + dy * uy) / Math.max(1, perMetre)
                    }
                    return (dx * sx + dy * sy) / lengthSquared
                }

                function beginCuboidInteraction(kind, axis, x, y) {
                    if (!targetMouseEditingAllowed)
                        return false
                    cuboidDragKind = kind
                    cuboidDragAxis = axis
                    cuboidDragX = x
                    cuboidDragY = y
                    cuboidDragActive = kind === "move"
                                       || kind === "resize"
                    return cuboidDragActive
                }

                function updateCuboidInteraction(x, y) {
                    if (!cuboidDragActive || !targetMouseEditingAllowed)
                        return
                    const dx = x - cuboidDragX
                    const dy = y - cuboidDragY
                    const target = window.controller.cuboidTargets.selectedTarget
                    const amount = axisDragAmount(target.center, cuboidDragAxis, dx, dy)
                    if (cuboidDragKind === "resize") {
                        window.controller.cuboidTargets.resizeSelected(
                            cuboidDragAxis, amount)
                    } else if (cuboidDragAxis === "x") {
                        window.controller.cuboidTargets.translateSelected(
                            amount, 0, 0)
                    } else if (cuboidDragAxis === "y") {
                        window.controller.cuboidTargets.translateSelected(
                            0, amount, 0)
                    } else {
                        window.controller.cuboidTargets.translateSelected(
                            0, 0, amount)
                    }
                    cuboidDragX = x
                    cuboidDragY = y
                }

                function endCuboidInteraction() {
                    cuboidDragActive = false
                    cuboidDragKind = ""
                    cuboidDragAxis = ""
                }

                function customPlanePoint(x, y) {
                    const target =
                        window.controller.customVolumeTargets.selectedTarget
                    const view = window.rayTracingEnabled
                               ? rayTracingTrajectoryOverlay
                               : rasterMapView
                    const ray = window.framedRay(view, x, y)
                    const nearPoint = ray.origin
                    const direction = ray.direction
                    let numerator = 0
                    let denominator = 0
                    if (target.plane === "xy") {
                        numerator = target.origin.z - nearPoint.z
                        denominator = direction.z
                    } else if (target.plane === "yz") {
                        numerator = target.origin.x - nearPoint.x
                        denominator = direction.x
                    } else {
                        numerator = target.origin.y - nearPoint.y
                        denominator = direction.y
                    }
                    if (Math.abs(denominator) < 0.000001)
                        return target.origin
                    const amount = numerator / denominator
                    return nearPoint.plus(direction.times(amount))
                }

                function beginCustomInteraction(kind, index, x, y) {
                    if (!targetMouseEditingAllowed
                        || window.controller.customVolumeDrawing)
                        return false
                    cuboidDragX = x
                    cuboidDragY = y
                    customVertexDragIndex = index
                    customVertexDragActive = kind === "custom-vertex"
                    customDepthDragActive = kind === "custom-depth"
                    return customVertexDragActive
                           || customDepthDragActive
                }

                function updateCustomInteraction(x, y) {
                    if (!targetMouseEditingAllowed) return
                    if (customVertexDragActive) {
                        const point = customPlanePoint(x, y)
                        window.controller.customVolumeTargets.setVertexWorld(
                            customVertexDragIndex,
                            point.x,
                            point.y,
                            point.z)
                    } else if (customDepthDragActive) {
                        const target =
                            window.controller.customVolumeTargets.selectedTarget
                        const dx = x - cuboidDragX
                        const dy = y - cuboidDragY
                        // the depth runs along the axis across the volume's plane
                        const axis = target.plane === "xy" ? "z"
                                   : target.plane === "yz" ? "x" : "y"
                        const amount = axisDragAmount(target.origin, axis, dx, dy)
                        window.controller.customVolumeTargets
                              .resizeDepthSelected(amount)
                    }
                    cuboidDragX = x
                    cuboidDragY = y
                }

                function endCustomInteraction() {
                    customVertexDragActive = false
                    customDepthDragActive = false
                    customVertexDragIndex = -1
                }

                function beginPoseInteraction(kind, axis, x, y) {
                    if (!targetMouseEditingAllowed)
                        return false
                    poseDragKind = kind
                    poseDragAxis = axis
                    cuboidDragX = x
                    cuboidDragY = y
                    poseDragActive = kind === "pose-move"
                                     || kind === "pose-rotate"
                    return poseDragActive
                }

                function updatePoseInteraction(x, y) {
                    if (!poseDragActive || !targetMouseEditingAllowed)
                        return
                    const dx = x - cuboidDragX
                    const dy = y - cuboidDragY
                    if (poseDragKind === "pose-rotate") {
                        let degrees = 0
                        if (poseDragAxis === "yaw")
                            degrees = dx * 0.5
                        else if (poseDragAxis === "pitch")
                            degrees = -dy * 0.5
                        else
                            degrees = (dx - dy) * 0.35
                        window.controller.poseTargets.rotateSelected(
                            poseDragAxis, degrees)
                    } else {
                        const target = window.controller.poseTargets.selectedTarget
                        const amount = axisDragAmount(target.position, poseDragAxis, dx, dy)
                        window.controller.poseTargets.translateSelected(
                            poseDragAxis === "x" ? amount : 0,
                            poseDragAxis === "y" ? amount : 0,
                            poseDragAxis === "z" ? amount : 0)
                    }
                    cuboidDragX = x
                    cuboidDragY = y
                }

                function endPoseInteraction() {
                    poseDragActive = false
                    poseDragKind = ""
                    poseDragAxis = ""
                }

                component CuboidEditorScene: Node {
                    id: cuboidScene
                    property bool interactive: false
                    property bool throughPass: false

                    Repeater3D {
                        model: window.controller.cuboidTargets

                        delegate: Node {
                            id: cuboidRoot
                            objectName: "cuboidTargetRoot"

                            required property int index
                            required property string targetId
                            required property vector3d targetCenter
                            required property vector3d targetSize
                            required property bool targetSelected
                            readonly property int targetIndex: index
                            readonly property var layerStyle:
                                window.visualStyle("target:cuboid:" + targetId,
                                                   { color: "#55a7d8",
                                                     opacity: 0.13,
                                                     throughBlocks:
                                                         window.controller.drawTargetsThroughBlocks })
                            readonly property bool targetActive:
                                targetSelected
                                && window.controller.evaluationTargetId
                                   === "volume-entry-time"
                            position: targetCenter
                            visible: layerStyle.visible
                                     && (cuboidScene.throughPass
                                         ? window.rayTracingEnabled
                                           || layerStyle.throughBlocks
                                         : !layerStyle.throughBlocks)

                            Model {
                                objectName: "cuboidTargetModel"
                                property int targetIndex:
                                    cuboidRoot.targetIndex
                                property string editorKind: "select"
                                property string editorAxis: ""
                                source: "#Cube"
                                scale: Qt.vector3d(
                                    cuboidRoot.targetSize.x / 100,
                                    cuboidRoot.targetSize.y / 100,
                                    cuboidRoot.targetSize.z / 100)
                                pickable: cuboidScene.interactive
                                castsShadows: false
                                receivesShadows: false
                                materials: DefaultMaterial {
                                    lighting: DefaultMaterial.NoLighting
                                    diffuseColor: cuboidRoot.targetActive
                                        ? Qt.lighter(cuboidRoot.layerStyle.color, 1.25)
                                        : cuboidRoot.layerStyle.color
                                    opacity: cuboidRoot.targetActive
                                        ? Math.min(1, cuboidRoot.layerStyle.opacity * 2.08)
                                        : cuboidRoot.layerStyle.opacity
                                    cullMode: Material.NoCulling
                                }
                            }

                            Node {
                                visible: cuboidRoot.targetActive
                                         && !window.controller.running
                                readonly property real barLength:
                                    Math.max(1.6,
                                             viewport.orbitDistance * 0.055)
                                readonly property real thickness:
                                    Math.max(0.12,
                                             viewport.orbitDistance * 0.005)
                                readonly property real handleSize:
                                    Math.max(0.34,
                                             viewport.orbitDistance * 0.014)

                                Model {
                                    property int targetIndex:
                                        cuboidRoot.targetIndex
                                    property string editorKind: "move"
                                    property string editorAxis: "x"
                                    source: "#Cube"
                                    x: cuboidRoot.targetSize.x / 2
                                       + parent.barLength / 2
                                    scale: Qt.vector3d(
                                        parent.barLength / 100,
                                        parent.thickness / 100,
                                        parent.thickness / 100)
                                    pickable: cuboidScene.interactive
                                    materials: DefaultMaterial {
                                        lighting:
                                            DefaultMaterial.NoLighting
                                        diffuseColor: "#e55353"
                                    }
                                }
                                Model {
                                    property int targetIndex:
                                        cuboidRoot.targetIndex
                                    property string editorKind: "move"
                                    property string editorAxis: "y"
                                    source: "#Cube"
                                    y: cuboidRoot.targetSize.y / 2
                                       + parent.barLength / 2
                                    scale: Qt.vector3d(
                                        parent.thickness / 100,
                                        parent.barLength / 100,
                                        parent.thickness / 100)
                                    pickable: cuboidScene.interactive
                                    materials: DefaultMaterial {
                                        lighting:
                                            DefaultMaterial.NoLighting
                                        diffuseColor: "#62c96b"
                                    }
                                }
                                Model {
                                    property int targetIndex:
                                        cuboidRoot.targetIndex
                                    property string editorKind: "move"
                                    property string editorAxis: "z"
                                    source: "#Cube"
                                    z: cuboidRoot.targetSize.z / 2
                                       + parent.barLength / 2
                                    scale: Qt.vector3d(
                                        parent.thickness / 100,
                                        parent.thickness / 100,
                                        parent.barLength / 100)
                                    pickable: cuboidScene.interactive
                                    materials: DefaultMaterial {
                                        lighting:
                                            DefaultMaterial.NoLighting
                                        diffuseColor: "#4c86e8"
                                    }
                                }

                                Model {
                                    objectName: "cuboidResizeHandleX"
                                    property int targetIndex:
                                        cuboidRoot.targetIndex
                                    property string editorKind: "resize"
                                    property string editorAxis: "x"
                                    source: "#Cube"
                                    x: cuboidRoot.targetSize.x / 2
                                       + parent.barLength
                                    scale: Qt.vector3d(
                                        parent.handleSize / 100,
                                        parent.handleSize / 100,
                                        parent.handleSize / 100)
                                    pickable: cuboidScene.interactive
                                    materials: DefaultMaterial {
                                        lighting:
                                            DefaultMaterial.NoLighting
                                        diffuseColor: "#ff7770"
                                    }
                                }
                                Model {
                                    objectName: "cuboidResizeHandleY"
                                    property int targetIndex:
                                        cuboidRoot.targetIndex
                                    property string editorKind: "resize"
                                    property string editorAxis: "y"
                                    source: "#Cube"
                                    y: cuboidRoot.targetSize.y / 2
                                       + parent.barLength
                                    scale: Qt.vector3d(
                                        parent.handleSize / 100,
                                        parent.handleSize / 100,
                                        parent.handleSize / 100)
                                    pickable: cuboidScene.interactive
                                    materials: DefaultMaterial {
                                        lighting:
                                            DefaultMaterial.NoLighting
                                        diffuseColor: "#87e58e"
                                    }
                                }
                                Model {
                                    objectName: "cuboidResizeHandleZ"
                                    property int targetIndex:
                                        cuboidRoot.targetIndex
                                    property string editorKind: "resize"
                                    property string editorAxis: "z"
                                    source: "#Cube"
                                    z: cuboidRoot.targetSize.z / 2
                                       + parent.barLength
                                    scale: Qt.vector3d(
                                        parent.handleSize / 100,
                                        parent.handleSize / 100,
                                        parent.handleSize / 100)
                                    pickable: cuboidScene.interactive
                                    materials: DefaultMaterial {
                                        lighting:
                                            DefaultMaterial.NoLighting
                                        diffuseColor: "#78a6ff"
                                    }
                                }
                            }
                        }
                    }
                }

                component CustomVolumeEditorScene: Node {
                    id: customVolumeScene
                    property bool interactive: false
                    property bool throughPass: false

                    Repeater3D {
                        model:
                            window.controller.customVolumeTargets.targets

                        delegate: Node {
                            id: customVolumeRoot

                            required property int index
                            required property var modelData
                            readonly property int targetIndex: index
                            readonly property var layerStyle:
                                window.visualStyle("target:custom:" + modelData.id,
                                                   { color: "#ca7ccc",
                                                     opacity: 0.14,
                                                     throughBlocks:
                                                         window.controller.drawTargetsThroughBlocks })
                            readonly property bool targetSelected:
                                modelData.selected
                            readonly property bool targetActive:
                                targetSelected
                                && window.controller.evaluationTargetId
                                   === "custom-volume-entry-time"
                            visible: layerStyle.visible
                                     && (customVolumeScene.throughPass
                                         ? window.rayTracingEnabled
                                           || layerStyle.throughBlocks
                                         : !layerStyle.throughBlocks)

                            Model {
                                objectName: "customVolumeTargetModel"
                                property int targetIndex:
                                    customVolumeRoot.targetIndex
                                property string editorKind:
                                    "custom-select"
                                property int vertexIndex: -1
                                visible: window.viewer.loaded
                                geometry: customVolumeRoot.modelData.geometry
                                pickable: customVolumeScene.interactive
                                castsShadows: false
                                receivesShadows: false
                                materials: DefaultMaterial {
                                    lighting: DefaultMaterial.NoLighting
                                    diffuseColor: customVolumeRoot.targetActive
                                        ? Qt.lighter(customVolumeRoot.layerStyle.color, 1.25)
                                        : customVolumeRoot.layerStyle.color
                                    opacity: customVolumeRoot.targetActive
                                        ? Math.min(1, customVolumeRoot.layerStyle.opacity * 2.14)
                                        : customVolumeRoot.layerStyle.opacity
                                    cullMode: Material.NoCulling
                                }
                            }

                            Model {
                                objectName: "customVolumeDrawingPlane"
                                visible:
                                    window.viewer.loaded
                                    && customVolumeRoot.targetActive
                                    && window.controller.customVolumeDrawing
                                position: customVolumeRoot.modelData.origin
                                source: "#Cube"
                                scale:
                                    customVolumeRoot.modelData.plane === "xy"
                                    ? Qt.vector3d(0.5, 0.5, 0.002)
                                    : customVolumeRoot.modelData.plane
                                      === "yz"
                                      ? Qt.vector3d(0.002, 0.5, 0.5)
                                      : Qt.vector3d(0.5, 0.002, 0.5)
                                materials: DefaultMaterial {
                                    lighting: DefaultMaterial.NoLighting
                                    diffuseColor: "#e6b75d"
                                    opacity: 0.1
                                    cullMode: Material.NoCulling
                                }
                            }

                            Node {
                                visible:
                                    window.viewer.loaded
                                    && customVolumeRoot.targetSelected
                                    && window.controller.evaluationTargetId
                                       === "custom-volume-entry-time"
                                    && !window.controller
                                              .customVolumeDrawing

                                Model {
                                    objectName:
                                        "customVolumePlaneChoiceXY"
                                    property int targetIndex:
                                        customVolumeRoot.targetIndex
                                    property string editorKind:
                                        "custom-plane"
                                    property string planeChoice: "xy"
                                    property int vertexIndex: -1
                                    position:
                                        customVolumeRoot.modelData.origin
                                        .plus(Qt.vector3d(-6, 3, 0))
                                    source: "#Cube"
                                    scale: Qt.vector3d(
                                        0.04, 0.04, 0.002)
                                    pickable:
                                        customVolumeScene.interactive
                                    materials: DefaultMaterial {
                                        lighting:
                                            DefaultMaterial.NoLighting
                                        diffuseColor: "#e36d6d"
                                        opacity: 0.22
                                    }
                                }
                                Model {
                                    objectName:
                                        "customVolumePlaneChoiceXZ"
                                    property int targetIndex:
                                        customVolumeRoot.targetIndex
                                    property string editorKind:
                                        "custom-plane"
                                    property string planeChoice: "xz"
                                    property int vertexIndex: -1
                                    position:
                                        customVolumeRoot.modelData.origin
                                        .plus(Qt.vector3d(0, 0, 0))
                                    source: "#Cube"
                                    scale: Qt.vector3d(
                                        0.04, 0.002, 0.04)
                                    pickable:
                                        customVolumeScene.interactive
                                    materials: DefaultMaterial {
                                        lighting:
                                            DefaultMaterial.NoLighting
                                        diffuseColor: "#6ed47b"
                                        opacity: 0.22
                                    }
                                }
                                Model {
                                    objectName:
                                        "customVolumePlaneChoiceYZ"
                                    property int targetIndex:
                                        customVolumeRoot.targetIndex
                                    property string editorKind:
                                        "custom-plane"
                                    property string planeChoice: "yz"
                                    property int vertexIndex: -1
                                    position:
                                        customVolumeRoot.modelData.origin
                                        .plus(Qt.vector3d(6, 3, 0))
                                    source: "#Cube"
                                    scale: Qt.vector3d(
                                        0.002, 0.04, 0.04)
                                    pickable:
                                        customVolumeScene.interactive
                                    materials: DefaultMaterial {
                                        lighting:
                                            DefaultMaterial.NoLighting
                                        diffuseColor: "#669cf0"
                                        opacity: 0.22
                                    }
                                }
                            }

                            Repeater3D {
                                model:
                                    customVolumeRoot.modelData.vertices

                                delegate: Model {
                                    required property var modelData
                                    property int targetIndex:
                                        customVolumeRoot.targetIndex
                                    property string editorKind:
                                        "custom-vertex"
                                    property int vertexIndex:
                                        modelData.index
                                    visible:
                                        window.viewer.loaded
                                        && customVolumeRoot.targetActive
                                    position: modelData.world
                                    source: "#Sphere"
                                    scale: Qt.vector3d(
                                        Math.max(
                                            0.004,
                                            viewport.orbitDistance
                                            * 0.00016),
                                        Math.max(
                                            0.004,
                                            viewport.orbitDistance
                                            * 0.00016),
                                        Math.max(
                                            0.004,
                                            viewport.orbitDistance
                                            * 0.00016))
                                    pickable:
                                        customVolumeScene.interactive
                                    materials: DefaultMaterial {
                                        lighting:
                                            DefaultMaterial.NoLighting
                                        diffuseColor: "#ffd06a"
                                    }
                                }
                            }

                            Model {
                                objectName: "customVolumeDepthHandle"
                                property int targetIndex:
                                    customVolumeRoot.targetIndex
                                property string editorKind: "custom-depth"
                                property int vertexIndex: -1
                                visible:
                                    window.viewer.loaded
                                    && customVolumeRoot.targetActive
                                    && !window.controller.customVolumeDrawing
                                position:
                                    customVolumeRoot.modelData.depthHandle
                                source: "#Cube"
                                scale: Qt.vector3d(
                                    Math.max(
                                        0.005,
                                        viewport.orbitDistance
                                        * 0.0002),
                                    Math.max(
                                        0.005,
                                        viewport.orbitDistance
                                        * 0.0002),
                                    Math.max(
                                        0.005,
                                        viewport.orbitDistance
                                        * 0.0002))
                                pickable: customVolumeScene.interactive
                                materials: DefaultMaterial {
                                    lighting: DefaultMaterial.NoLighting
                                    diffuseColor: "#fff0a6"
                                }
                            }
                        }
                    }
                }

                // Every restart's latest improvement car, drawn with one
                // instanced call per render mode however many restarts.
                component ImprovementCars: Node {
                    id: improvementCarsNode
                    readonly property var layerStyle:
                        window.visualStyle("car:improvements",
                                           { color: "#ffffff", opacity: 1 })

                    Model {
                        objectName: "improvementCarsFilledModel"
                        visible: window.renderMode !== "wireframe"
                        geometry: window.viewer.ellipsoidFilledGeometry
                        instancing: window.viewer.improvementCars
                        castsShadows: false
                        receivesShadows: false
                        materials: DefaultMaterial {
                            lighting: DefaultMaterial.NoLighting
                            vertexColorsEnabled: true
                            diffuseColor: improvementCarsNode.layerStyle.color
                            opacity: improvementCarsNode.layerStyle.opacity
                            cullMode: Material.BackFaceCulling
                        }
                    }

                    Model {
                        objectName: "improvementCarsWireModel"
                        visible: window.renderMode === "wireframe"
                        geometry: window.viewer.ellipsoidWireGeometry
                        instancing: window.viewer.improvementCars
                        castsShadows: false
                        receivesShadows: false
                        materials: DefaultMaterial {
                            lighting: DefaultMaterial.NoLighting
                            vertexColorsEnabled: true
                            diffuseColor: improvementCarsNode.layerStyle.color
                            opacity: improvementCarsNode.layerStyle.opacity
                            cullMode: Material.NoCulling
                        }
                    }
                }

                component PoseTargetEditorScene: Node {
                    id: poseScene
                    property bool interactive: false
                    property bool throughPass: false

                    Repeater3D {
                        model: window.controller.poseTargets.targets

                        delegate: Node {
                            id: poseRoot

                            required property int index
                            required property var modelData
                            readonly property int targetIndex: index
                            readonly property var layerStyle:
                                window.visualStyle("target:pose:" + modelData.id,
                                                   { color: "#65a7d8",
                                                     opacity: 0.36,
                                                     throughBlocks:
                                                         window.controller.drawTargetsThroughBlocks })
                            readonly property bool targetActive:
                                modelData.selected
                                && window.controller.evaluationTargetId
                                   === "pose-target"
                            position: modelData.position
                            visible: window.viewer.loaded
                                     && layerStyle.visible
                                     && (poseScene.throughPass
                                         ? window.rayTracingEnabled
                                           || layerStyle.throughBlocks
                                         : !layerStyle.throughBlocks)

                            Node {
                                rotation: poseRoot.modelData.rotation

                                Repeater3D {
                                    model: window.viewer.carEllipsoids.length

                                    delegate: Node {
                                        required property int index
                                        readonly property var ellipsoid:
                                            window.viewer.carEllipsoids[index]

                                        position: ellipsoid.position
                                        rotation: ellipsoid.rotation
                                        scale: ellipsoid.radii

                                        Model {
                                            objectName:
                                                "poseTargetCarModel"
                                            property int targetIndex:
                                                poseRoot.targetIndex
                                            property string editorKind:
                                                "pose-select"
                                            property string editorAxis: ""
                                            geometry:
                                                window.viewer
                                                      .ellipsoidFilledGeometry
                                            pickable:
                                                poseScene.interactive
                                            castsShadows: false
                                            receivesShadows: false
                                            materials: DefaultMaterial {
                                                lighting:
                                                    DefaultMaterial
                                                    .NoLighting
                                                diffuseColor: poseRoot.targetActive
                                                    ? Qt.lighter(poseRoot.layerStyle.color, 1.25)
                                                    : poseRoot.layerStyle.color
                                                opacity: poseRoot.targetActive
                                                    ? Math.min(1, poseRoot.layerStyle.opacity * 2.28)
                                                    : poseRoot.layerStyle.opacity
                                                cullMode:
                                                    Material.NoCulling
                                            }
                                        }
                                    }
                                }
                            }

                            Node {
                                id: poseHandleRoot

                                visible: poseRoot.targetActive
                                         && !window.controller.running
                                readonly property real barLength:
                                    Math.max(
                                        1.8,
                                        viewport.orbitDistance * 0.05)
                                readonly property real thickness:
                                    Math.max(
                                        0.12,
                                        viewport.orbitDistance * 0.004)
                                readonly property real rotationHandleSize:
                                    Math.max(
                                        0.5,
                                        viewport.orbitDistance * 0.018)

                                Repeater3D {
                                    model: [
                                        {
                                            "axis": "x",
                                            "position":
                                                Qt.vector3d(
                                                    1.6
                                                    + poseHandleRoot
                                                          .barLength
                                                      / 2,
                                                    0,
                                                    0),
                                            "scale": Qt.vector3d(
                                                poseHandleRoot.barLength
                                                / 100,
                                                poseHandleRoot.thickness
                                                / 100,
                                                poseHandleRoot.thickness
                                                / 100),
                                            "color": "#e36d6d"
                                        },
                                        {
                                            "axis": "y",
                                            "position":
                                                Qt.vector3d(
                                                    0,
                                                    1.1
                                                    + poseHandleRoot
                                                          .barLength
                                                      / 2,
                                                    0),
                                            "scale": Qt.vector3d(
                                                poseHandleRoot.thickness
                                                / 100,
                                                poseHandleRoot.barLength
                                                / 100,
                                                poseHandleRoot.thickness
                                                / 100),
                                            "color": "#6ed47b"
                                        },
                                        {
                                            "axis": "z",
                                            "position":
                                                Qt.vector3d(
                                                    0,
                                                    0,
                                                    3.2
                                                    + poseHandleRoot
                                                          .barLength
                                                      / 2),
                                            "scale": Qt.vector3d(
                                                poseHandleRoot.thickness
                                                / 100,
                                                poseHandleRoot.thickness
                                                / 100,
                                                poseHandleRoot.barLength
                                                / 100),
                                            "color": "#669cf0"
                                        }
                                    ]

                                    delegate: Model {
                                        required property var modelData
                                        objectName:
                                            "poseTargetMoveHandle"
                                        property int targetIndex:
                                            poseRoot.targetIndex
                                        property string editorKind:
                                            "pose-move"
                                        property string editorAxis:
                                            modelData.axis
                                        position: modelData.position
                                        scale: modelData.scale
                                        source: "#Cube"
                                        pickable:
                                            poseScene.interactive
                                        materials: DefaultMaterial {
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            diffuseColor: modelData.color
                                        }
                                    }
                                }

                                Repeater3D {
                                    model: [
                                        {
                                            "axis": "pitch",
                                            "position":
                                                Qt.vector3d(-2.3, 0, 0),
                                            "color": "#ffadad"
                                        },
                                        {
                                            "axis": "yaw",
                                            "position":
                                                Qt.vector3d(0, -1.8, 0),
                                            "color": "#a9efb2"
                                        },
                                        {
                                            "axis": "roll",
                                            "position":
                                                Qt.vector3d(0, 0, -3.9),
                                            "color": "#a8c7ff"
                                        }
                                    ]

                                    delegate: Model {
                                        required property var modelData
                                        objectName:
                                            "poseTargetRotationHandle"
                                        property int targetIndex:
                                            poseRoot.targetIndex
                                        property string editorKind:
                                            "pose-rotate"
                                        property string editorAxis:
                                            modelData.axis
                                        position: modelData.position
                                        scale: Qt.vector3d(
                                            poseHandleRoot
                                                  .rotationHandleSize
                                            / 100,
                                            poseHandleRoot
                                                  .rotationHandleSize
                                            / 100,
                                            poseHandleRoot
                                                  .rotationHandleSize
                                            / 100)
                                        source: "#Sphere"
                                        pickable:
                                            poseScene.interactive
                                        materials: DefaultMaterial {
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            diffuseColor: modelData.color
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // The race view as the render module draws it, behind the
                // 3D view (whose map meshes show the same image).
                RenderModuleView {
                    id: moduleRenderView
                    objectName: "moduleRenderView"
                    anchors.fill: parent
                    z: -1
                    // Shows nothing until its first frame.
                    visible: window.renderMode === "module"
                    active: window.renderMode === "module" && viewport.visible
                            && window.viewer.loaded
                    packsDirectory: window.viewer.loadedPacksDirectory
                    mapPath: window.viewer.loadedReplayPath
                    viewer: window.viewer
                    raceTimeMs: window.viewer.presentationTimeMs
                    showCar: window.moduleGameCar && !window.moduleCarCheckHidden
                    // The 3D view's map meshes show this frame: draw them
                    // again with it.
                    onFrameChanged: rasterMapView.update()
                    cameraPosition: viewCamera.scenePosition
                    cameraTarget: viewCamera.scenePosition.plus(viewCamera.forward)
                    cameraUp: viewCamera.up
                    fieldOfView: viewCamera.fieldOfView
                    clipNear: viewCamera.clipNear
                    clipFar: viewCamera.clipFar
                    framing: window.framedRect
                }

                View3D {
                    id: rasterMapView
                    objectName: "rasterMapView"
                    anchors.fill: parent
                    visible: !window.rayTracingEnabled
                    camera: viewCamera

                    environment: SceneEnvironment {
                        objectName: "mapEnvironment"
                        // The module's image shows behind the map.
                        backgroundMode: window.moduleMapShown
                                        ? SceneEnvironment.Transparent
                                        : SceneEnvironment.SkyBox
                        antialiasingMode: SceneEnvironment.MSAA
                        antialiasingQuality: SceneEnvironment.Medium
                        tonemapMode: SceneEnvironment.TonemapModeAces
                        probeExposure: 1.0
                        skyboxBlurAmount: 0.0
                        specularAAEnabled: true

                        lightProbe: Texture {
                            objectName: "daySkyTexture"
                            source: "qrc:/environment/day_sky.png"
                            mappingMode: Texture.LightProbe
                            generateMipmaps: true
                        }
                    }

                    Node {
                        position: viewport.carCameraActive
                                  ? window.viewer.carCameraPosition
                                  : viewport.freeCamera
                                    ? viewport.freeCameraPosition
                                    : viewport.cameraTarget
                        rotation: viewport.carCameraActive
                                  ? window.viewer.carCameraRotation
                                  : Qt.quaternion(1, 0, 0, 0)

                        Node {
                            eulerRotation.x: viewport.carCameraActive
                                             ? 0 : viewport.orbitPitch
                            eulerRotation.y: viewport.carCameraActive
                                             ? 0 : viewport.orbitYaw

                            CustomCamera {
                                id: viewCamera
                                objectName: "viewCamera"
                                readonly property real clipDistance:
                                    viewport.carCameraActive
                                    ? window.viewer.carCameraPosition
                                            .minus(window.viewer
                                                   .carCameraTarget)
                                            .length()
                                    : viewport.freeCamera ? 0 : viewport.orbitDistance
                                readonly property var dynamicClipPlanes:
                                    window.viewer.cameraClipPlanes(
                                        scenePosition, clipDistance)

                                z: viewport.carCameraActive
                                   || viewport.freeCamera
                                   ? 0 : viewport.orbitDistance
                                property real clipNear: dynamicClipPlanes.x
                                property real clipFar: dynamicClipPlanes.y
                                // Vertical, over the framed part of the view.
                                property real fieldOfView: viewport.carCameraActive
                                    ? window.viewer.carCameraFieldOfView
                                    : viewport.cameraFieldOfView
                                projection: window.framedProjection(
                                    fieldOfView, clipNear, clipFar)
                            }
                        }
                    }

                    DirectionalLight {
                        objectName: "mainMapLight"
                        eulerRotation.x: -52
                        eulerRotation.y: -32
                        brightness: 1.2
                        color: "#fff3d7"
                        castsShadow: false
                    }

                    DirectionalLight {
                        objectName: "fillMapLight"
                        eulerRotation.x: -20
                        eulerRotation.y: 145
                        brightness: 0.35
                        color: "#b9dbf2"
                        castsShadow: false
                    }

                    Instantiator {
                        id: visualMaterialCache
                        model: window.viewer.visualMaterials

                        delegate: PrincipledMaterial {
                            required property var modelData

                            objectName: "trackVisualMaterial"
                            Texture {
                                id: replacementBaseMap
                                objectName: "trackVisualBaseTexture"
                                source: modelData.baseTexture
                                tilingModeHorizontal: Texture.Repeat
                                tilingModeVertical: Texture.Repeat
                                generateMipmaps: true
                                minFilter: Texture.Linear
                                magFilter: Texture.Linear
                                mipFilter: Texture.Linear
                            }

                            lighting:
                                PrincipledMaterial.FragmentLighting
                            baseColor: window.renderMode ===
                                       "neutral"
                                       ? "#aeb3af"
                                       : (window.renderMode ===
                                          "material-debug"
                                          ? modelData.debugColor
                                          : "#ffffff")
                            baseColorMap: window.texturedMap
                                          ? replacementBaseMap
                                          : null
                            roughness: window.renderMode ===
                                       "neutral"
                                       ? 0.74
                                       : modelData.roughness
                            metalness: window.renderMode ===
                                       "neutral"
                                       ? 0
                                       : modelData.metalness
                            cullMode: Material.NoCulling
                            vertexColorsEnabled:
                                modelData.vertexColors
                                && window.texturedMap
                            emissiveMap: window.texturedMap
                                         && modelData.emissiveStrength > 0
                                         ? replacementBaseMap
                                         : null
                            emissiveFactor: window.texturedMap
                                            ? Qt.vector3d(
                                                  modelData.emissiveStrength,
                                                  modelData.emissiveStrength,
                                                  modelData.emissiveStrength)
                                            : Qt.vector3d(0, 0, 0)
                        }
                    }

                    CustomMaterial {
                        id: moduleScreenMaterial
                        objectName: "moduleScreenMaterial"
                        shadingMode: CustomMaterial.Unshaded
                        cullMode: Material.NoCulling
                        fragmentShader: "qrc:/materials/module_screen.frag"
                        property TextureInput moduleImage: TextureInput {
                            texture: Texture {
                                sourceItem: moduleRenderView
                            }
                        }
                        property size viewSize: moduleRenderView.frameSize
                        // Where the view sits in the module's image.
                        property vector2d moduleImageSize: Qt.vector2d(
                            moduleRenderView.imageSize.width,
                            moduleRenderView.imageSize.height)
                        property vector2d moduleImageOrigin: Qt.vector2d(
                            moduleRenderView.imageOrigin.x,
                            moduleRenderView.imageOrigin.y)
                    }

                    Repeater3D {
                        model: window.viewer.visualBatches

                        delegate: Model {
                            required property var modelData
                            readonly property int materialBindingIndex:
                                modelData.materialBindingIndex
                            readonly property var sharedMaterial: {
                                const cacheSize = visualMaterialCache.count
                                return cacheSize > 0
                                       ? visualMaterialCache.objectAt(
                                             materialBindingIndex)
                                       : null
                            }

                            objectName: "trackVisualModel"
                            visible: window.viewer.loaded
                                     && window.texturedMap
                                     && modelData.defaultVisible
                            geometry: modelData.geometry
                            castsShadows: false

                            // The render module's image, pixel for pixel,
                            // so the map hides what is behind it.
                            materials: window.moduleMapShown
                                       ? [moduleScreenMaterial]
                                       : sharedMaterial
                                         ? [sharedMaterial] : []
                        }
                    }

                    CuboidEditorScene {
                        objectName: "rasterCuboidEditorScene"
                        interactive: viewport.targetMouseEditingAllowed
                    }

                    CustomVolumeEditorScene {
                        objectName: "rasterCustomVolumeEditorScene"
                        interactive: viewport.targetMouseEditingAllowed
                    }

                    PoseTargetEditorScene {
                        objectName: "rasterPoseTargetEditorScene"
                        interactive: viewport.targetMouseEditingAllowed
                    }

                    Model {
                        objectName: "trackFilledModel"
                        visible: window.viewer.loaded
                                 && (window.renderMode === "neutral"
                                     || window.renderMode === "collision"
                                     || window.renderMode ===
                                        "material-debug")
                        geometry: window.viewer.loaded
                                  ? window.viewer.trackFilledGeometry
                                  : null
                        materials: DefaultMaterial {
                            lighting: DefaultMaterial.NoLighting
                            vertexColorsEnabled: true
                            diffuseColor: window.renderMode === "neutral"
                                          ? "#aeb3af" : "white"
                            cullMode: Material.BackFaceCulling
                        }
                    }

                    Model {
                        objectName: "trackWireModel"
                        visible: window.viewer.loaded
                                 && window.renderMode === "wireframe"
                        geometry: window.viewer.loaded
                                  ? window.viewer.trackWireGeometry
                                  : null
                        materials: DefaultMaterial {
                            lighting: DefaultMaterial.NoLighting
                            diffuseColor: "#b8d9c7"
                            cullMode: Material.NoCulling
                        }
                    }

                    Repeater3D {
                        model: window.viewer.trajectoryPaths

                        delegate: Model {
                            required property var modelData
                            readonly property var layerStyle:
                                window.visualStyle(modelData.visualId,
                                                   { color: modelData.color,
                                                     opacity: modelData.opacity,
                                                     width: 3 })

                            objectName: "trajectoryPathModel"
                            visible: window.viewer.loaded
                                     && (modelData.visible ?? true)
                                     && layerStyle.visible
                                     && !layerStyle.throughBlocks
                            geometry: modelData.geometry
                            castsShadows: false
                            receivesShadows: false
                            materials: DefaultMaterial {
                                lighting: DefaultMaterial.NoLighting
                                vertexColorsEnabled: modelData.vertexColors ?? false
                                diffuseColor: layerStyle.color
                                opacity: layerStyle.opacity
                                lineWidth: layerStyle.width
                                cullMode: Material.NoCulling
                            }
                        }
                    }

                    Repeater3D {
                        model: graphicsInspector.windowRows.filter(
                            row => !row.group && row.kind === "window")
                        delegate: Model {
                            required property var modelData
                            readonly property var layerStyle:
                                window.visualStyle(modelData.id,
                                                   { color: modelData.color,
                                                     width: modelData.width,
                                                     opacity: modelData.opacity,
                                                     throughBlocks: true })
                            readonly property var runs: window.viewer.runOptions
                            readonly property var timeRange:
                                window.overlayWindowRange(modelData.id)
                            objectName: "occludedTimeWindowModel"
                            visible: layerStyle.visible
                                     && !layerStyle.throughBlocks
                            geometry: runs.length > 0
                                ? window.viewer.timeRangeGeometry(
                                    modelData.id, modelData.runId,
                                    timeRange[0], timeRange[1],
                                    layerStyle.linePattern === "dash")
                                : null
                            castsShadows: false
                            receivesShadows: false
                            materials: DefaultMaterial {
                                lighting: DefaultMaterial.NoLighting
                                diffuseColor: layerStyle.color
                                opacity: layerStyle.opacity
                                lineWidth: layerStyle.width
                            }
                        }
                    }

                    Repeater3D {
                        model: graphicsInspector.windowRows.filter(
                            row => !row.group && row.kind === "window")
                        delegate: Node {
                            id: windowEndpoints
                            required property var modelData
                            readonly property var layerStyle:
                                window.visualStyle(modelData.id,
                                                   { color: modelData.color,
                                                     opacity: modelData.opacity,
                                                     throughBlocks: true })
                            readonly property var timeRange:
                                window.overlayWindowRange(modelData.id)
                            visible: layerStyle.visible
                                     && !layerStyle.throughBlocks
                            Repeater3D {
                                model: windowEndpoints.timeRange
                                delegate: Model {
                                    required property var modelData
                                    readonly property var sample:
                                        evaluationOverlay.sampleAt(
                                            Number(modelData))
                                    visible: sample !== null
                                    position: sample ? sample.position
                                                     : Qt.vector3d(0, 0, 0)
                                    source: "#Sphere"
                                    scale: Qt.vector3d(0.004, 0.004, 0.004)
                                    materials: DefaultMaterial {
                                        lighting: DefaultMaterial.NoLighting
                                        diffuseColor:
                                            windowEndpoints.layerStyle.color
                                        opacity:
                                            windowEndpoints.layerStyle.opacity
                                    }
                                }
                            }
                        }
                    }

                    Model {
                        objectName: "occludedHorizonMarker"
                        readonly property var sample:
                            evaluationOverlay.sampleAt(
                                evaluationOverlay.horizonTime())
                        readonly property var layerStyle:
                            evaluationOverlay.horizonStyle
                        visible: window.viewer.loaded && sample
                                 && layerStyle.visible
                                 && !layerStyle.throughBlocks
                        position: sample ? sample.position
                                         : Qt.vector3d(0, 0, 0)
                        source: "#Sphere"
                        scale: Qt.vector3d(0.005, 0.005, 0.005)
                        materials: DefaultMaterial {
                            lighting: DefaultMaterial.NoLighting
                            diffuseColor: parent.layerStyle.color
                            opacity: parent.layerStyle.opacity
                        }
                    }

                    Model {
                        objectName: "occludedEvaluationEventMarker"
                        readonly property var event:
                            window.controller.evaluationTargetId === "stunt-points"
                            ? evaluationOverlay.sampleAt(
                                evaluationOverlay.stuntTime())
                            : evaluationOverlay.summary.event
                        readonly property var layerStyle:
                            evaluationOverlay.eventStyle
                        visible: window.viewer.loaded && event
                                 && layerStyle.visible
                                 && !layerStyle.throughBlocks
                        position: event ? event.position
                                        : Qt.vector3d(0, 0, 0)
                        source: "#Sphere"
                        scale: Qt.vector3d(0.006, 0.006, 0.006)
                        materials: DefaultMaterial {
                            lighting: DefaultMaterial.NoLighting
                            diffuseColor: parent.layerStyle.color
                            opacity: parent.layerStyle.opacity
                        }
                    }

                    Model {
                        objectName: "occludedPointTargetMarker"
                        readonly property var layerStyle:
                            evaluationOverlay.pointStyle
                        visible: window.viewer.loaded
                                 && evaluationOverlay.showsPoint
                                 && layerStyle.visible
                                 && !layerStyle.throughBlocks
                        position: evaluationOverlay.pointTarget()
                        source: "#Sphere"
                        scale: Qt.vector3d(0.007, 0.007, 0.007)
                        materials: DefaultMaterial {
                            lighting: DefaultMaterial.NoLighting
                            diffuseColor: parent.layerStyle.color
                            opacity: parent.layerStyle.opacity
                        }
                    }

                    Model {
                        objectName: "occludedPointTargetLine"
                        readonly property var layerStyle:
                            evaluationOverlay.pointStyle
                        visible: window.viewer.loaded
                                 && window.controller.evaluationTargetId
                                    === "point-target"
                                 && evaluationOverlay.summary.event
                                 && layerStyle.visible
                                 && !layerStyle.throughBlocks
                        geometry: visible
                            ? window.viewer.visualLineGeometry(
                                "target:point",
                                [evaluationOverlay.summary.event.position,
                                 evaluationOverlay.pointTarget()], false)
                            : null
                        materials: DefaultMaterial {
                            lighting: DefaultMaterial.NoLighting
                            diffuseColor: parent.layerStyle.color
                            opacity: parent.layerStyle.opacity
                            lineWidth: parent.layerStyle.width
                        }
                    }

                    Model {
                        objectName: "occludedVelocityDirection"
                        readonly property var layerStyle:
                            evaluationOverlay.velocityStyle
                        readonly property var direction:
                            evaluationOverlay.directionFrom(start)
                        readonly property real length:
                            Math.max(4, window.viewer.carVelocity.length())
                        readonly property var start:
                            window.viewer.carPosition
                        readonly property var end:
                            Qt.vector3d(start.x + direction.x * length,
                                        start.y + direction.y * length,
                                        start.z + direction.z * length)
                        visible: window.viewer.loaded
                                 && window.controller.evaluationTargetId
                                    === "velocity"
                                 && layerStyle.visible
                                 && !layerStyle.throughBlocks
                        geometry: visible
                            ? window.viewer.visualLineGeometry(
                                "target:velocity", [start, end], false)
                            : null
                        materials: DefaultMaterial {
                            lighting: DefaultMaterial.NoLighting
                            diffuseColor: parent.layerStyle.color
                            opacity: parent.layerStyle.opacity
                            lineWidth: parent.layerStyle.width
                        }
                    }

                    Model {
                        objectName: "occludedConditionPlane"
                        readonly property var layerStyle:
                            evaluationOverlay.conditionStyle
                        readonly property var corners:
                            evaluationOverlay.conditionPlaneCorners()
                        visible: window.viewer.loaded
                                 && corners.length === 4
                                 && layerStyle.visible
                                 && !layerStyle.throughBlocks
                        geometry: visible
                            ? window.viewer.visualLineGeometry(
                                "condition:threshold", corners, true)
                            : null
                        materials: DefaultMaterial {
                            lighting: DefaultMaterial.NoLighting
                            diffuseColor: parent.layerStyle.color
                            opacity: parent.layerStyle.opacity
                            lineWidth: parent.layerStyle.width
                        }
                    }

                    Repeater3D {
                        model: window.viewer.runCount

                        delegate: Node {
                            id: runCarRoot

                            required property int index
                            readonly property var runPose:
                                window.viewer.runPoses[index]
                            readonly property int runIndex: index
                            readonly property var layerStyle:
                                window.visualStyle("car:" + (runPose?.styleId ?? ""),
                                                   { color: window.runColor(runPose?.colorIndex ?? index),
                                                     opacity: 1 })

                            objectName: "runCarRoot"
                            // runPose is briefly missing while runs change.
                            visible: !!runPose
                                     && !runPose.selected
                                     && !runPose.hidden
                                     && layerStyle.visible
                                     && !layerStyle.throughBlocks
                            position: runPose ? runPose.position : Qt.vector3d(0, 0, 0)
                            rotation: runPose ? runPose.rotation : Qt.quaternion(1, 0, 0, 0)

                            Repeater3D {
                                model: window.viewer.carEllipsoids.length

                                delegate: Node {
                                    required property int index
                                    readonly property var ellipsoid:
                                        window.viewer.carEllipsoids[
                                            index]

                                    objectName: "runCarEllipsoidNode"
                                    position: ellipsoid.position
                                    rotation: ellipsoid.rotation
                                    scale: ellipsoid.radii

                                    Model {
                                        objectName: "runCarFilledModel"
                                        visible: window.renderMode !==
                                                 "wireframe"
                                        geometry: window.viewer
                                            .ellipsoidFilledGeometries[
                                                (runCarRoot.runPose?.colorIndex ?? runCarRoot.runIndex) %
                                                window.viewer
                                                    .ellipsoidFilledGeometries
                                                    .length]
                                        materials: DefaultMaterial {
                                            objectName: "runCarFilledMaterial"
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            vertexColorsEnabled: true
                                            diffuseColor: runCarRoot.layerStyle.color
                                            opacity: runCarRoot.layerStyle.opacity
                                            cullMode:
                                                Material.BackFaceCulling
                                        }
                                    }

                                    Model {
                                        objectName: "runCarWireModel"
                                        visible: window.renderMode ===
                                                 "wireframe"
                                        geometry:
                                            window.viewer.ellipsoidWireGeometry
                                        materials: DefaultMaterial {
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            diffuseColor: runCarRoot.layerStyle.color
                                            opacity: runCarRoot.layerStyle.opacity
                                            cullMode: Material.NoCulling
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Node {
                        id: selectedRunCarRoot
                        readonly property var layerStyle:
                            window.visualStyle(
                                "car:" + window.viewer.selectedRunStyleId,
                                { color: window.runColor(
                                    window.viewer.selectedRunColorIndex),
                                  opacity: 1 })

                        objectName: "selectedRunCarRoot"
                        // Help > Check the car: one frame without it
                        property bool checkHidden: false
                        visible: !checkHidden && window.viewer.loaded
                                 && window.viewer.runCount > 0
                                 && layerStyle.visible
                                 && !layerStyle.throughBlocks
                                 && !(viewport.carCameraActive
                                      && window.viewer.hideSelectedCar)
                                 && !window.moduleCarShown
                        position: window.viewer.carPosition
                        rotation: window.viewer.carRotation

                        Repeater3D {
                            model: window.viewer.carEllipsoids.length

                            delegate: Node {
                                required property int index
                                readonly property var ellipsoid:
                                    window.viewer.carEllipsoids[index]

                                objectName: "selectedRunCarEllipsoidNode"
                                position: ellipsoid.position
                                rotation: ellipsoid.rotation
                                scale: ellipsoid.radii

                                Model {
                                    objectName: "selectedRunCarFilledModel"
                                    visible: window.renderMode !==
                                             "wireframe"
                                    geometry: window.viewer
                                        .selectedEllipsoidFilledGeometry
                                    materials: DefaultMaterial {
                                        lighting:
                                            DefaultMaterial.NoLighting
                                        vertexColorsEnabled: true
                                        diffuseColor: selectedRunCarRoot.layerStyle.color
                                        opacity: selectedRunCarRoot.layerStyle.opacity
                                        cullMode:
                                            Material.BackFaceCulling
                                    }
                                }

                                Model {
                                    objectName: "selectedRunCarWireModel"
                                    visible: window.renderMode ===
                                             "wireframe"
                                    geometry:
                                        window.viewer
                                            .ellipsoidWireGeometry
                                    materials: DefaultMaterial {
                                        lighting:
                                            DefaultMaterial.NoLighting
                                        diffuseColor: selectedRunCarRoot.layerStyle.color
                                        opacity: selectedRunCarRoot.layerStyle.opacity
                                        cullMode: Material.NoCulling
                                    }
                                }
                            }
                        }
                    }
                    ImprovementCars {
                        objectName: "improvementCarsRoot"
                        visible: window.viewer.loaded
                                 && window.viewer.improvementCarCount > 0
                                 && layerStyle.visible
                                 && !layerStyle.throughBlocks
                    }
                }

                GpuRayTracingView {
                    id: gpuRayTracingView
                    objectName: "gpuRayTracingView"
                    anchors.fill: parent
                    z: 1
                    visible: window.rayTracingEnabled
                    active: window.rayTracingEnabled && viewport.visible &&
                            (!window.shutdown || !window.shutdown.closing)
                            && window.viewer.loaded
                    viewer: window.viewer
                    cameraPosition: viewCamera.scenePosition
                    cameraTarget: viewport.cameraTarget
                    cameraUp: viewCamera.up
                    fieldOfView: viewCamera.fieldOfView
                    framing: window.framedRect
                }

                View3D {
                    id: rayTracingTrajectoryOverlay
                    objectName: "rayTracingTrajectoryOverlay"
                    anchors.fill: parent
                    z: 1.5
                    camera: rayTracingOverlayCamera
                    visible: window.viewer.loaded
                             && window.hasThroughVisuals()

                    environment: SceneEnvironment {
                        backgroundMode: SceneEnvironment.Transparent
                        antialiasingMode: SceneEnvironment.MSAA
                        antialiasingQuality: SceneEnvironment.Medium
                    }

                    Node {
                        position: viewport.carCameraActive
                                  ? window.viewer.carCameraPosition
                                  : viewport.freeCamera
                                    ? viewport.freeCameraPosition
                                    : viewport.cameraTarget
                        rotation: viewport.carCameraActive
                                  ? window.viewer.carCameraRotation
                                  : Qt.quaternion(1, 0, 0, 0)

                        Node {
                            eulerRotation.x: viewport.carCameraActive
                                             ? 0 : viewport.orbitPitch
                            eulerRotation.y: viewport.carCameraActive
                                             ? 0 : viewport.orbitYaw

                            CustomCamera {
                                id: rayTracingOverlayCamera
                                objectName: "rayTracingOverlayCamera"
                                readonly property var dynamicClipPlanes:
                                    window.viewer.cameraClipPlanes(
                                        scenePosition,
                                        viewCamera.clipDistance)

                                z: viewport.carCameraActive
                                   || viewport.freeCamera
                                   ? 0 : viewport.orbitDistance
                                projection: window.framedProjection(
                                    viewCamera.fieldOfView, dynamicClipPlanes.x,
                                    dynamicClipPlanes.y)
                            }
                        }
                    }

                    Repeater3D {
                        model: window.viewer.trajectoryPaths

                        delegate: Model {
                            required property var modelData
                            readonly property var layerStyle:
                                window.visualStyle(modelData.visualId,
                                                   { color: modelData.color,
                                                     opacity: modelData.opacity,
                                                     width: 3 })

                            objectName:
                                "rayTracingTrajectoryPathModel"
                            visible: (window.rayTracingEnabled
                                      || layerStyle.throughBlocks)
                                     && layerStyle.visible
                                     && (modelData.visible ?? true)
                            geometry: modelData.geometry
                            castsShadows: false
                            receivesShadows: false
                            materials: DefaultMaterial {
                                lighting: DefaultMaterial.NoLighting
                                vertexColorsEnabled: modelData.vertexColors ?? false
                                diffuseColor: layerStyle.color
                                opacity: layerStyle.opacity
                                lineWidth: layerStyle.width
                                cullMode: Material.NoCulling
                            }
                        }
                    }

                    // Keyed by count, like the raster cars: a pose list
                    // model would rebuild every car on each pose update.
                    Repeater3D {
                        model: window.viewer.runCount

                        delegate: Node {
                            id: throughCar
                            required property int index
                            readonly property var modelData:
                                window.viewer.runPoses[index]
                            readonly property var layerStyle:
                                window.visualStyle("car:" + (modelData?.styleId ?? ""),
                                                   { color: window.runColor(modelData?.colorIndex ?? index),
                                                     opacity: 1 })
                            objectName: "throughRunCarRoot"
                            visible: layerStyle.visible
                                     && (window.rayTracingEnabled
                                         || layerStyle.throughBlocks)
                                     && !!modelData
                                     && !(modelData.selected
                                          && viewport.carCameraActive
                                          && window.viewer.hideSelectedCar)
                                     && !(modelData.selected
                                          && window.moduleCarShown)
                            position: !modelData ? Qt.vector3d(0, 0, 0)
                                : modelData.selected
                                ? window.viewer.carPosition
                                : modelData.position
                            rotation: !modelData ? Qt.quaternion(1, 0, 0, 0)
                                : modelData.selected
                                ? window.viewer.carRotation
                                : modelData.rotation

                            Repeater3D {
                                model: window.viewer.carEllipsoids.length
                                delegate: Node {
                                    required property int index
                                    readonly property var ellipsoid:
                                        window.viewer.carEllipsoids[index]
                                    position: ellipsoid.position
                                    rotation: ellipsoid.rotation
                                    scale: ellipsoid.radii
                                    Model {
                                        geometry: window.viewer.ellipsoidFilledGeometry
                                        visible: window.renderMode !== "wireframe"
                                        materials: DefaultMaterial {
                                            lighting: DefaultMaterial.NoLighting
                                            vertexColorsEnabled: true
                                            diffuseColor: throughCar.layerStyle.color
                                            opacity: throughCar.layerStyle.opacity
                                            cullMode: Material.NoCulling
                                        }
                                    }
                                    Model {
                                        geometry: window.viewer.ellipsoidWireGeometry
                                        visible: window.renderMode === "wireframe"
                                        materials: DefaultMaterial {
                                            lighting: DefaultMaterial.NoLighting
                                            diffuseColor: throughCar.layerStyle.color
                                            opacity: throughCar.layerStyle.opacity
                                            lineWidth: 1.5
                                            cullMode: Material.NoCulling
                                        }
                                    }
                                }
                            }
                        }
                    }

                    ImprovementCars {
                        objectName: "throughImprovementCarsRoot"
                        visible: window.viewer.improvementCarCount > 0
                                 && layerStyle.visible
                                 && (window.rayTracingEnabled
                                     || layerStyle.throughBlocks)
                    }

                    CuboidEditorScene {
                        objectName: "rayTracingCuboidEditorScene"
                        throughPass: true
                        interactive: viewport.targetMouseEditingAllowed
                    }

                    CustomVolumeEditorScene {
                        objectName: "rayTracingCustomVolumeEditorScene"
                        throughPass: true
                        interactive: viewport.targetMouseEditingAllowed
                    }

                    PoseTargetEditorScene {
                        objectName: "rayTracingPoseTargetEditorScene"
                        throughPass: true
                        interactive: viewport.targetMouseEditingAllowed
                    }
                }

                WhiteboardPlanes {
                    id: whiteboardPlaneView
                    anchors.fill: parent
                    z: 1.75
                    model: window.viewer.whiteboard
                    cameraTarget: viewport.cameraTarget
                    cameraPosition: viewCamera.scenePosition
                    freeCamera: viewport.freeCamera
                    orbitYaw: viewport.orbitYaw
                    orbitPitch: viewport.orbitPitch
                    orbitDistance: viewport.orbitDistance
                    fieldOfView: viewCamera.fieldOfView
                    exactBoardIndex:
                        viewport.exactWhiteboardBoardIndex
                    contentTop: whiteboardOverlay.boardTop
                }

                Connections {
                    target: window.viewer.whiteboard

                    function onBoardsChanged() {
                        viewport.synchronizeWhiteboardFocus()
                    }
                }

                Connections {
                    target: window.controller

                    function onCuboidFocusRequested(center, size) {
                        viewport.focusCuboid(center, size)
                    }
                    function onCustomVolumeFocusRequested(center, size) {
                        viewport.focusCuboid(center, size)
                    }
                    function onPoseTargetFocusRequested(center, size) {
                        viewport.focusCuboid(center, size)
                    }
                }

                MouseArea {
                    id: manualInputFocus
                    objectName: "manualInputFocus"
                    anchors.fill: parent
                    z: 2
                    focus: false
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Canvas
                    Accessible.name: qsTr("Race preview")
                    Accessible.focusable: true
                    acceptedButtons: Qt.LeftButton
                    hoverEnabled: true
                    property real previousX: 0
                    property real previousY: 0

                    Keys.priority: Keys.BeforeItem
                    Keys.onPressed: event => {
                        viewport.handleCameraPresetKey(event, true)
                        if (!event.accepted)
                            viewport.handleFreeCameraKey(event, true)
                        if (!event.accepted)
                            raceControls.handle(event, true)
                    }
                    Keys.onReleased: event => {
                        viewport.handleCameraPresetKey(event, false)
                        if (!event.accepted)
                            viewport.handleFreeCameraKey(event, false)
                        if (!event.accepted)
                            raceControls.handle(event, false)
                    }
                    onActiveFocusChanged: {
                        if (!activeFocus
                            && window.viewer.manualDriving) {
                            window.viewer.releaseManualInputs()
                        }
                        if (!activeFocus)
                            viewport.releaseFreeMovement()
                    }

                    onPressed: mouse => {
                        editHistory.beginGesture()
                        manualInputFocus.forceActiveFocus()
                        previousX = mouse.x
                        previousY = mouse.y
                        viewport.beginViewRotation()
                        if (window.controller.customVolumeDrawing
                            && viewport.targetMouseEditingAllowed) {
                            const point = viewport.customPlanePoint(
                                mouse.x, mouse.y)
                            window.controller.customVolumeTargets
                                  .addVertexWorld(
                                      point.x, point.y, point.z)
                            viewport.cuboidPointerCaptured = true
                            return
                        }
                        if (viewport.whiteboardPlaneFocusEnabled()) {
                            const boardIndex =
                                whiteboardPlaneView.pickBoard(
                                    mouse.x, mouse.y)
                            if (boardIndex >= 0
                                && window.viewer.whiteboard
                                         .selectBoard(boardIndex)) {
                                viewport.restoreWhiteboardView(
                                    window.viewer.whiteboard
                                          .boards[boardIndex])
                                viewport.cuboidPointerCaptured = true
                                return
                            }
                        }
                        const view = window.rayTracingEnabled
                                     || window.controller
                                              .drawTargetsThroughBlocks
                                     ? rayTracingTrajectoryOverlay
                                     : rasterMapView
                        if (viewport.pointPlacementActive
                            && viewport.targetMouseEditingAllowed) {
                            const picked = window.framedPick(
                                rasterMapView, mouse.x, mouse.y)
                            const point = picked.objectHit
                                          ? picked.scenePosition
                                          : viewport.customPlanePoint(
                                                mouse.x, mouse.y)
                            window.controller.setEvaluationTargetSetting(
                                "x", String(point.x))
                            window.controller.setEvaluationTargetSetting(
                                "y", String(point.y))
                            window.controller.setEvaluationTargetSetting(
                                "z", String(point.z))
                            viewport.pointPlacementActive = false
                            viewport.cuboidPointerCaptured = true
                            return
                        }
                        const hit = viewport.editorPick(view, mouse.x, mouse.y)
                        if (hit && hit.targetIndex !== undefined
                            && viewport.targetMouseEditingAllowed) {
                            viewport.cuboidPointerCaptured = true
                            if (hit.editorKind
                                && hit.editorKind.indexOf("pose-")
                                   === 0) {
                                window.controller.poseTargets
                                      .selectTarget(hit.targetIndex)
                                window.controller.evaluationTargetId =
                                    "pose-target"
                                viewport.beginPoseInteraction(
                                    hit.editorKind,
                                    hit.editorAxis,
                                    mouse.x,
                                    mouse.y)
                            } else if (hit.editorKind
                                && hit.editorKind.indexOf("custom-")
                                   === 0) {
                                window.controller.customVolumeTargets
                                      .selectTarget(hit.targetIndex)
                                window.controller.evaluationTargetId =
                                    "custom-volume-entry-time"
                                if (hit.editorKind === "custom-plane") {
                                    window.controller.customVolumeTargets
                                          .setPlane(
                                              hit.targetIndex,
                                              hit.planeChoice)
                                }
                                viewport.beginCustomInteraction(
                                    hit.editorKind,
                                    hit.vertexIndex,
                                    mouse.x,
                                    mouse.y)
                            } else {
                                window.controller.cuboidTargets
                                      .selectTarget(hit.targetIndex)
                                window.controller.evaluationTargetId =
                                    "volume-entry-time"
                            }
                            if (hit.editorKind === "move"
                                || hit.editorKind === "resize") {
                                viewport.beginCuboidInteraction(
                                    hit.editorKind,
                                    hit.editorAxis,
                                    mouse.x,
                                    mouse.y)
                            }
                            return
                        }
                    }
                    // A drag moves gizmos and the camera once per frame, to
                    // where the pointer is then: a fast mouse sends many
                    // moves a frame, each worth a whole update.
                    property var pendingMove: null
                    FrameAnimation {
                        running: manualInputFocus.pendingMove !== null
                        onTriggered: manualInputFocus.applyPendingMove()
                    }
                    function applyPendingMove() {
                        const move = pendingMove
                        pendingMove = null
                        if (move)
                            moveTo(move.x, move.y)
                    }
                    function moveTo(x, y) {
                        if (viewport.cuboidDragActive) {
                            viewport.updateCuboidInteraction(x, y)
                            previousX = x
                            previousY = y
                            return
                        }
                        if (viewport.customVertexDragActive
                            || viewport.customDepthDragActive) {
                            viewport.updateCustomInteraction(x, y)
                            previousX = x
                            previousY = y
                            return
                        }
                        if (viewport.poseDragActive) {
                            viewport.updatePoseInteraction(x, y)
                            previousX = x
                            previousY = y
                            return
                        }
                        if (viewport.cuboidPointerCaptured)
                            return
                        viewport.leaveExactWhiteboardView()
                        viewport.updateViewRotation(x - previousX, y - previousY)
                        previousX = x
                        previousY = y
                    }
                    onPositionChanged: mouse => {
                        if (mouse.buttons & Qt.LeftButton)
                            pendingMove = Qt.point(mouse.x, mouse.y)
                    }
                    onReleased: {
                        applyPendingMove()
                        viewport.endCuboidInteraction()
                        viewport.endCustomInteraction()
                        viewport.endPoseInteraction()
                        viewport.cuboidPointerCaptured = false
                        if (window.viewer.manualDriving
                            || window.viewer.takeOverOnInput
                            || viewport.freeCamera)
                            manualInputFocus.forceActiveFocus()
                        editHistory.endGesture()
                    }
                    onCanceled: {
                        pendingMove = null
                        viewport.endCuboidInteraction()
                        viewport.endCustomInteraction()
                        viewport.endPoseInteraction()
                        viewport.cuboidPointerCaptured = false
                        editHistory.endGesture()
                    }
                    onWheel: wheel => {
                        if (viewport.zoomOrbit(wheel.angleDelta.y))
                            wheel.accepted = true
                    }
                }

                WhiteboardOverlay {
                    id: whiteboardOverlay
                    objectName: "whiteboardOverlay"
                    onEditGestureStarted: editHistory.beginGesture()
                    onEditGestureFinished: editHistory.endGesture()
                    anchors.fill: parent
                    z: 2.5
                    model: window.viewer.whiteboard
                    available: window.viewer.loaded
                    captureViewpoint: function() {
                        return viewport.captureWhiteboardView()
                    }
                    restoreViewpoint: function(board) {
                        viewport.restoreWhiteboardView(board)
                    }
                    afterPickUp: function() {
                        viewport.enableFreeCamera()
                    }
                    imageExportInProgress:
                        viewport.exportingWhiteboardImage
                    exportBackgroundImage: function(index, fileUrl) {
                        return viewport.exportWhiteboardBackground(
                                    index, fileUrl)
                    }
                    toolbarMaximumWidth: whiteboardToolbarHost.width
                    toolbarHost: whiteboardToolbarHost
                    drawingListHost: whiteboardDrawingListHost
                    visible: !viewport.exportingWhiteboardImage
                }

                EvaluationOverlay {
                    id: evaluationOverlay
                    anchors.fill: parent
                    controller: window.controller
                    viewer: window.viewer
                    projector: rasterMapView
                    pickAt: (x, y) => window.framedPick(rasterMapView, x, y)
                    suppressed: viewport.exportingWhiteboardImage
                }

                Column {
                    anchors.centerIn: parent
                    width: Math.min(parent.width / AppTheme.zoom - 60, 430)
                    scale: AppTheme.zoom
                    spacing: 12
                    visible: !window.viewer.loaded
                             && !viewport.exportingWhiteboardImage

                    BusyIndicator {
                        anchors.horizontalCenter: parent.horizontalCenter
                        running: window.viewer.loading
                        visible: running
                    }

                    Label {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: window.viewer.loading
                              ? window.viewer.statusText
                              : qsTr("Open a replay or challenge with File > Open map or replay.")
                        color: AppTheme.viewerOverlayText
                        wrapMode: Text.WordWrap
                        font.pixelSize: 16
                    }

                    Label {
                        width: parent.width
                        visible: !window.viewer.loading
                                 && window.viewer.statusText
                                    !== qsTr("No map loaded")
                                 && !mapFailure.visible
                        horizontalAlignment: Text.AlignHCenter
                        text: window.viewer.statusText
                        color: "#f0a19e"
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                    }

                    FailureNotice {
                        id: mapFailure
                        objectName: "viewerFailure"
                        width: parent.width
                        visible: !window.viewer.loading
                                 && reason.length > 0
                        failure: window.viewer.statusFailure
                        textColor: AppTheme.viewerOverlayText
                        detailsColor: AppTheme.viewerOverlayMuted
                    }

                }
            }
        }

        Component.onCompleted: {
            AppTheme.backdrop = raceBackdrop
            AppTheme.blurred = blurredRace
            AppTheme.panelsBlurred = panelsBlurred
            AppTheme.panelMap = Qt.binding(function() {
                return Qt.vector4d(ui.x - raceBackdrop.x, ui.y - raceBackdrop.y,
                                   window.width, window.height)
            })
        }
    }

    // ---- panels and menus (scaled by View > Zoom) ------------------------------
    Item {
        id: ui
        objectName: "uiLayer"
        width: window.width / AppTheme.zoom
        height: window.height / AppTheme.zoom
        scale: AppTheme.zoom
        transformOrigin: Item.TopLeft

        DockSpace {
            id: dockSpace
            objectName: "dockSpace"
            y: window.menuBarHeight
            width: parent.width
            height: parent.height - y
            backdropOrigin: Qt.point(dockSpace.x * AppTheme.zoom - raceBackdrop.x,
                                     dockSpace.y * AppTheme.zoom - raceBackdrop.y)
            maximizedPanel: window.codeEditorExpanded ? "code" : ""
            onMaximizeEnded: window.codeEditorExpanded = false
            savedState: String(window.controller.uiSetting("layout", ""))
            onStateSaved: state => window.controller.setUiSetting("layout", state)
            defaultLayout: window.defaultLayout

            DockPanel {
                id: inputsPanel
                objectName: "inputsPanel"
                panelId: "inputs"
                title: qsTr("Inputs")

                Rectangle {
                    id: timelinePanel
                    objectName: "timelinePanel"
                    anchors.fill: parent
                    color: AppTheme.panel

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 62
                            color: AppTheme.panelAlternate

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 14
                                anchors.rightMargin: 14
                                anchors.topMargin: 8
                                anchors.bottomMargin: 8
                                spacing: 2

                                Label {
                                    objectName: "timelineTimeLabel"
                                    text: window.viewer.timeText.replace(" / ", "\n/ ")
                                    color: AppTheme.text
                                    font.family: "monospace"
                                    font.pixelSize: 12
                                    font.weight: Font.Medium
                                }

                                Label {
                                    text: window.viewer.runCount > 0
                                          ? qsTr("Tick %1 / %2 · 100 Hz")
                                                .arg(window.viewer.currentTick)
                                                .arg(Math.max(0,
                                                              window.viewer.tickCount - 1))
                                          : window.viewer.loaded
                                            ? qsTr("Map loaded · no search run")
                                            : qsTr("100 physics ticks / second")
                                    color: AppTheme.textMuted
                                    font.pixelSize: 10
                                }
                            }
                            ThemedIconButton {
                                objectName: "timelineRotateButton"
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                icon.source: "qrc:/icons/rotate-cw.svg"
                                checkable: true
                                checked: timelinePreferences.horizontal
                                ToolTip.visible: hovered
                                ToolTip.text: checked ? qsTr("Time down") : qsTr("Time across")
                                onClicked: timelinePreferences.horizontal = checked
                            }
                        }

                        RaceTimeline {
                            id: raceTimeline
                            objectName: "raceTimeline"
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            viewer: window.viewer
                            enabled: window.viewer.runCount > 0
                                     && !window.viewer.manualDriving
                            pixelsPerTick: 3
                            horizontal: timelinePreferences.horizontal

                            QtObject {
                                id: timelinePreferences
                                property bool horizontal: String(AppSettings.value(
                                    "timeline/horizontal", false)) === "true"
                                onHorizontalChanged: AppSettings.setValue(
                                    "timeline/horizontal", horizontal)
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 50
                            color: AppTheme.panelAlternate

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                spacing: 10

                                Rectangle {
                                    Layout.preferredWidth: 10
                                    Layout.preferredHeight: 10
                                    radius: 2
                                    color: AppTheme.info
                                }
                                Label {
                                    text: qsTr("Steer")
                                    color: AppTheme.viewerOverlayMuted
                                    font.pixelSize: 10
                                }
                                Rectangle {
                                    Layout.preferredWidth: 10
                                    Layout.preferredHeight: 10
                                    radius: 2
                                    color: AppTheme.accent
                                }
                                Label {
                                    text: qsTr("Gas")
                                    color: AppTheme.viewerOverlayMuted
                                    font.pixelSize: 10
                                }
                                Rectangle {
                                    Layout.preferredWidth: 10
                                    Layout.preferredHeight: 10
                                    radius: 2
                                    color: AppTheme.error
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Brake")
                                    color: AppTheme.viewerOverlayMuted
                                    font.pixelSize: 10
                                }
                            }
                        }
                    }
                }
            }

            DockPanel {
                id: graphicsPanel
                objectName: "graphicsPanel"
                panelId: "graphics"
                title: qsTr("Graphics")

                GraphicsInspector {
                    id: graphicsInspector
                    anchors.fill: parent
                    viewer: window.viewer
                    controller: window.controller
                    editHistory: editHistory
                    renderMode: window.renderMode
                    rayTracingSupported: gpuRayTracingView.supported
                    moduleRenderLabel: moduleRenderView.available
                                       ? moduleRenderView.label : ""
                    moduleRenderStatus: moduleRenderView.status
                    moduleGameCar: window.moduleGameCar
                    onModuleGameCarRequested: show => {
                        window.moduleGameCar = show
                        window.controller.setUiSetting("moduleGameCar", show)
                    }
                    rayTracingStatus: gpuRayTracingView.status
                    onRenderModeRequested: mode => window.renderMode = mode
                }
            }

            DockPanel {
                id: racePanel
                objectName: "racePanel"
                panelId: "race"
                title: qsTr("Race viewer")
                available: window.controller.viewerVisible

                Rectangle {
                    id: raceViewerHeader
                    objectName: "raceViewerHeader"
                    anchors.fill: parent
                    color: "transparent"

                    RowLayout {
                        id: headerControlsRow
                        objectName: "headerControlsRow"
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        spacing: 10

                        ColumnLayout {
                            id: raceViewerTitleBlock
                            objectName: "raceViewerTitleBlock"
                            visible: raceViewerHeader.width >= 470
                            Layout.fillWidth: true
                            Layout.minimumWidth:
                                visible ? (raceViewerHeader.width < 650
                                           ? 100 : 150) : 0
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 0

                            Label {
                                text: qsTr("Race Viewer")
                                color: AppTheme.viewerOverlayText
                                font.pixelSize: 17
                                font.weight: Font.DemiBold
                            }

                            Label {
                                Layout.fillWidth: true
                                text: window.viewer.loaded
                                      ? qsTr("%1 triangles · %2 batches · %3 FPS")
                                            .arg(window.controller
                                                     .formatCompactNumber(
                                                         window.viewer
                                                           .visualTriangleCount))
                                            .arg(window.controller
                                                     .formatCompactNumber(
                                                         window.viewer
                                                           .visualBatchCount))
                                            .arg(Math.round(
                                                     window.measuredFps))
                                      : window.viewer.statusText
                                color: AppTheme.viewerOverlayMuted
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }

                        StyledComboBox {
                            id: runSelector

                            objectName: "runSelector"
                            Layout.preferredWidth:
                                raceViewerHeader.width < 650 ? 118 : 160
                            Layout.alignment: Qt.AlignVCenter
                            model: window.viewer.runOptions
                            textRole: "name"
                            valueRole: "id"
                            enabled: count > 0
                                     && !window.viewer.manualDriving

                            function synchronizeSelection() {
                                const selected = indexOfValue(
                                    window.viewer.selectedRunId)
                                if (selected >= 0
                                    && currentIndex !== selected) {
                                    currentIndex = selected
                                }
                            }

                            Component.onCompleted:
                                synchronizeSelection()
                            onModelChanged:
                                Qt.callLater(synchronizeSelection)
                            onActivated: selectedIndex =>
                                window.viewer.selectedRunId =
                                    valueAt(selectedIndex)

                            Connections {
                                target: window.viewer

                                function onRunsChanged() {
                                    Qt.callLater(
                                        runSelector.synchronizeSelection)
                                }
                                function onSelectedRunChanged() {
                                    runSelector.synchronizeSelection()
                                }
                            }
                        }


                        ThemedButton {
                            objectName: "resetViewButton"
                            Layout.alignment: Qt.AlignVCenter
                            Layout.preferredWidth:
                                raceViewerHeader.width < 650 ? 86 : 100
                            text: qsTr("Reset view")
                            enabled: window.viewer.loaded
                            onClicked: {
                                viewport.orbitYaw = 35
                                viewport.orbitPitch = -20
                                viewport.orbitDistance = 38
                                viewport.focusCurrentCar()
                            }
                        }
                    }
                }
            }

            DockPanel {
                id: cameraPanel
                objectName: "cameraPanel"
                panelId: "camera"
                title: qsTr("Camera")
                available: window.controller.viewerVisible && window.viewer.loaded

                Rectangle {
                    id: cameraFocusToolbar
                    objectName: "cameraFocusToolbar"
                    anchors.fill: parent
                    color: "transparent"

                    GridLayout {
                        id: cameraFocusControls
                        anchors.centerIn: parent
                        columns: cameraFocusToolbar.width < 440 ? 3 : 6
                        columnSpacing: 4
                        rowSpacing: 4

                        ThemedButton {
                            objectName: "freeCameraButton"
                            Layout.preferredWidth: Math.max(58, implicitWidth)
                            Layout.preferredHeight: 32
                            text: qsTr("Free")
                            highlighted: viewport.freeCamera
                            Accessible.name: qsTr("Free camera")
                            onClicked: {
                                viewport.enableFreeCamera()
                                manualInputFocus.forceActiveFocus()
                            }
                            ToolTip.visible: hovered
                            ToolTip.delay: 350
                            ToolTip.text: qsTr("Free camera")
                        }

                        ThemedButton {
                            objectName: "orbitalCameraButton"
                            Layout.preferredWidth: Math.max(70, implicitWidth)
                            Layout.preferredHeight: 32
                            text: qsTr("Orbital")
                            enabled: window.viewer.runCount > 0
                            highlighted: enabled
                                         && viewport.orbitalCamera
                            Accessible.name: qsTr("Orbital camera")
                            onClicked: {
                                viewport.enableOrbitalCamera()
                                manualInputFocus.forceActiveFocus()
                            }
                            ToolTip.visible: hovered
                            ToolTip.delay: 350
                            ToolTip.text: qsTr(
                                "Orbit around the car with drag and wheel")
                        }

                        ThemedButton {
                            objectName: "focusCarButton"
                            Layout.preferredWidth: Math.max(52, implicitWidth)
                            Layout.preferredHeight: 32
                            text: qsTr("Far")
                            enabled: window.viewer.runCount > 0
                                     && window.viewer.carCameras.includes(1)
                            highlighted: enabled
                                && viewport.carCameraActive
                                && window.viewer.cameraPreset === 1
                            Accessible.name: qsTr("Far car camera")
                            onClicked: {
                                window.viewer.cameraPreset = 1
                                viewport.focusCurrentCar()
                                manualInputFocus.forceActiveFocus()
                            }
                            ToolTip.visible: hovered
                            ToolTip.delay: 350
                            ToolTip.text: qsTr("Far car camera (1)")
                        }

                        ThemedButton {
                            objectName: "nearCameraButton"
                            Layout.preferredWidth: Math.max(60, implicitWidth)
                            Layout.preferredHeight: 32
                            text: qsTr("Near")
                            enabled: window.viewer.runCount > 0
                                     && window.viewer.carCameras.includes(2)
                            highlighted: enabled
                                && viewport.carCameraActive
                                && window.viewer.cameraPreset === 2
                            Accessible.name: qsTr("Near car camera")
                            onClicked: {
                                window.viewer.cameraPreset = 2
                                viewport.focusCurrentCar()
                                manualInputFocus.forceActiveFocus()
                            }
                            ToolTip.visible: hovered
                            ToolTip.delay: 350
                            ToolTip.text: qsTr("Near car camera (2)")
                        }

                        ThemedButton {
                            objectName: "internalCameraButton"
                            Layout.preferredWidth: Math.max(82, implicitWidth)
                            Layout.preferredHeight: 32
                            text: qsTr("Internal")
                            enabled: window.viewer.runCount > 0
                                     && window.viewer.carCameras.includes(3)
                            highlighted: enabled
                                && viewport.carCameraActive
                                && window.viewer.cameraPreset === 3
                            Accessible.name: qsTr("Internal car camera")
                            onClicked: {
                                window.viewer.cameraPreset = 3
                                viewport.focusCurrentCar()
                                manualInputFocus.forceActiveFocus()
                            }
                            ToolTip.visible: hovered
                            ToolTip.delay: 350
                            ToolTip.text: qsTr("Internal car camera (3)")
                        }

                        ThemedButton {
                            objectName: "focusObjectButton"
                            Layout.preferredWidth: 64
                            Layout.preferredHeight: 32
                            text: qsTr("Target")
                            enabled: viewport.hasObjectFocus
                            highlighted: !viewport.freeCamera
                                         && viewport.cuboidFocused
                            Accessible.name: qsTr("Focus selected target")
                            onClicked: {
                                viewport.focusLastObject()
                                manualInputFocus.forceActiveFocus()
                            }
                            ToolTip.visible: hovered
                            ToolTip.delay: 350
                            ToolTip.text: qsTr("Focus selected target")
                        }
                    }
                }
            }

            DockPanel {
                id: telemetryPanel
                objectName: "telemetryPanel"
                panelId: "telemetry"
                title: qsTr("Scripted telemetry")
                available: window.controller.viewerVisible && window.viewer.loaded

                Rectangle {
                    id: scriptedTelemetry
                    objectName: "scriptedTelemetry"
                    anchors.fill: parent
                    color: "transparent"

                    RowLayout {
                        id: scriptedTelemetryContent
                        anchors.fill: parent
                        anchors.margins: 6
                        spacing: 4

                        Label {
                            id: scriptedTelemetryText
                            objectName: "scriptedTelemetryText"
                            Layout.fillWidth: true
                            Layout.maximumWidth: 430
                            text: viewport.renderTelemetry(
                                      window.viewer.telemetryScript)
                            textFormat: Text.RichText
                            color: AppTheme.viewerOverlayText
                            font.pixelSize: 12
                            font.family: "monospace"
                            wrapMode: Text.Wrap
                            Accessible.name: qsTr("Scripted telemetry")
                            Accessible.description: text
                        }

                        ThemedToolButton {
                            id: editTelemetryButton
                            objectName: "editTelemetryButton"
                            Layout.preferredWidth: 26
                            Layout.preferredHeight: 26
                            text: "\u270e"
                            font.pixelSize: 15
                            Accessible.name: qsTr("Edit scripted telemetry")
                            onClicked: telemetryEditorDialog.open()
                            ToolTip.visible: hovered
                            ToolTip.delay: 350
                            ToolTip.text: qsTr("Edit scripted telemetry")
                        }
                    }
                }
            }

            DockPanel {
                id: splitsPanel
                objectName: "splitsPanel"
                panelId: "splits"
                title: qsTr("Splits")
                available: window.controller.viewerVisible
                           && window.viewer.checkpointSplits.length > 0

                Rectangle {
                    id: checkpointSplitOverlay
                    objectName: "checkpointSplitOverlay"
                    anchors.fill: parent
                    color: "transparent"
                    clip: true

                    Rectangle {
                        id: checkpointSplitHeader
                        objectName: "checkpointSplitHeader"
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 38
                        color: AppTheme.viewerOverlayControl

                        Label {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Splits")
                            color: AppTheme.viewerOverlayText
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                        }
                    }

                    ListView {
                        id: checkpointSplitList
                        objectName: "checkpointSplitList"
                        anchors.top: checkpointSplitHeader.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        model: window.viewer.checkpointSplits
                        boundsBehavior: Flickable.StopAtBounds
                        clip: true

                        onCountChanged: {
                            if (count > 0)
                                positionViewAtEnd()
                        }

                        ScrollBar.vertical: ScrollBar {
                            policy: checkpointSplitList.contentHeight
                                    > checkpointSplitList.height
                                    ? ScrollBar.AsNeeded
                                    : ScrollBar.AlwaysOff
                        }

                        delegate: Rectangle {
                            required property int index
                            required property var modelData
                            objectName: "checkpointSplitRow_" + index
                            width: checkpointSplitList.width
                            height: 36
                            color: modelData.isFinish
                                   ? "#244b34"
                                   : "transparent"

                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 1
                                color: AppTheme.viewerOverlayBorder
                                opacity: 0.5
                            }

                            RowLayout {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 8

                                Label {
                                    objectName:
                                        "checkpointSplitLabel_" + index
                                    Layout.fillWidth: true
                                    text: modelData.label
                                    color: modelData.isFinish
                                           ? "#8ee1ac"
                                           : AppTheme.viewerOverlayText
                                    elide: Text.ElideRight
                                    font.pixelSize: 11
                                    font.weight: modelData.isFinish
                                                 ? Font.DemiBold
                                                 : Font.Normal
                                }

                                Label {
                                    objectName:
                                        "checkpointSplitTime_" + index
                                    Layout.alignment: Qt.AlignRight
                                    text: modelData.time
                                    color: modelData.isFinish
                                           ? "#8ee1ac"
                                           : AppTheme.viewerOverlayMuted
                                    font.family: "monospace"
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                }
                            }
                        }
                    }
                }
            }

            DockPanel {
                id: playbackPanel
                objectName: "playbackPanel"
                panelId: "playback"
                title: qsTr("Playback")
                available: window.controller.viewerVisible

                Rectangle {
                    id: playbackDock
                    objectName: "playbackDock"
                    readonly property bool compact: width < 400
                    anchors.fill: parent
                    color: "transparent"

                    GridLayout {
                        anchors.centerIn: parent
                        columns: playbackDock.compact ? 4 : 5
                        columnSpacing: 8
                        rowSpacing: 4

                        ThemedToolButton {
                            id: jumpStartButton
                            objectName: "jumpStartButton"
                            Layout.preferredWidth: 42
                            Layout.preferredHeight: 42
                            implicitWidth: 42
                            implicitHeight: 42
                            text: ""
                            enabled: window.viewer.runCount > 0
                            palette.buttonText: AppTheme.viewerOverlayText
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Go to start and focus car")
                            onClicked: {
                                window.viewer.jumpToStart()
                                viewport.resetCameraFocus()
                            }

                            contentItem: Item {
                                Item {
                                    objectName: "jumpStartTransportIcon"
                                    anchors.centerIn: parent
                                    width: 18
                                    height: 18

                                    Rectangle {
                                        x: 2
                                        y: 2
                                        width: 3
                                        height: 14
                                        radius: 1
                                        color:
                                            jumpStartButton.effectiveTextColor
                                    }

                                    Canvas {
                                        anchors.fill: parent
                                        property color iconColor:
                                            jumpStartButton.effectiveTextColor
                                        antialiasing: true
                                        onIconColorChanged: requestPaint()
                                        onPaint: {
                                            const context = getContext("2d")
                                            context.clearRect(0, 0,
                                                              width, height)
                                            context.fillStyle = iconColor
                                            context.beginPath()
                                            context.moveTo(15, 2)
                                            context.lineTo(6, 9)
                                            context.lineTo(15, 16)
                                            context.closePath()
                                            context.fill()
                                        }
                                    }
                                }
                            }
                        }

                        ThemedToolButton {
                            id: playPauseButton
                            objectName: "playPauseButton"
                            Layout.preferredWidth: 42
                            Layout.preferredHeight: 42
                            implicitWidth: 42
                            implicitHeight: 42
                            text: ""
                            enabled: window.viewer.runCount > 0
                                     && !window.viewer.manualDriving
                            palette.buttonText:
                                AppTheme.viewerOverlayText
                            ToolTip.visible: hovered
                            ToolTip.text: window.viewer.playing
                                          ? qsTr("Pause")
                                          : qsTr("Play")
                            onClicked: window.viewer.togglePlayback()

                            contentItem: Item {
                                Canvas {
                                    objectName: "playTransportIcon"
                                    anchors.centerIn: parent
                                    width: 18
                                    height: 18
                                    visible: !window.viewer.playing
                                    property color iconColor:
                                        playPauseButton.effectiveTextColor
                                    antialiasing: true
                                    onIconColorChanged: requestPaint()
                                    onPaint: {
                                        const context = getContext("2d")
                                        context.clearRect(0, 0,
                                                          width, height)
                                        context.fillStyle = iconColor
                                        context.beginPath()
                                        context.moveTo(4, 2)
                                        context.lineTo(16, 9)
                                        context.lineTo(4, 16)
                                        context.closePath()
                                        context.fill()
                                    }
                                }

                                Item {
                                    objectName: "pauseTransportIcon"
                                    anchors.centerIn: parent
                                    width: 18
                                    height: 18
                                    visible: window.viewer.playing

                                    Rectangle {
                                        x: 3
                                        y: 2
                                        width: 4
                                        height: 14
                                        radius: 1
                                        color:
                                            playPauseButton.effectiveTextColor
                                    }

                                    Rectangle {
                                        x: 11
                                        y: 2
                                        width: 4
                                        height: 14
                                        radius: 1
                                        color:
                                            playPauseButton.effectiveTextColor
                                    }
                                }
                            }
                        }

                        ThemedToolButton {
                            id: jumpEndButton
                            objectName: "jumpEndButton"
                            Layout.preferredWidth: 42
                            Layout.preferredHeight: 42
                            implicitWidth: 42
                            implicitHeight: 42
                            text: ""
                            enabled: window.viewer.runCount > 0
                                     && !window.viewer.manualDriving
                            palette.buttonText: AppTheme.viewerOverlayText
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Go to end")
                            onClicked: window.viewer.jumpToEnd()

                            contentItem: Item {
                                Item {
                                    objectName: "jumpEndTransportIcon"
                                    anchors.centerIn: parent
                                    width: 18
                                    height: 18

                                    Canvas {
                                        anchors.fill: parent
                                        property color iconColor:
                                            jumpEndButton.effectiveTextColor
                                        antialiasing: true
                                        onIconColorChanged: requestPaint()
                                        onPaint: {
                                            const context = getContext("2d")
                                            context.clearRect(0, 0,
                                                              width, height)
                                            context.fillStyle = iconColor
                                            context.beginPath()
                                            context.moveTo(3, 2)
                                            context.lineTo(12, 9)
                                            context.lineTo(3, 16)
                                            context.closePath()
                                            context.fill()
                                        }
                                    }

                                    Rectangle {
                                        x: 13
                                        y: 2
                                        width: 3
                                        height: 14
                                        radius: 1
                                        color:
                                            jumpEndButton.effectiveTextColor
                                    }
                                }
                            }
                        }

                        // Held: the race runs backwards.
                        ThemedToolButton {
                            id: rewindButton
                            objectName: "rewindButton"
                            Layout.preferredWidth: 42
                            Layout.preferredHeight: 42
                            implicitWidth: 42
                            implicitHeight: 42
                            text: ""
                            enabled: window.viewer.runCount > 0
                            down: window.viewer.rewinding
                            palette.buttonText: AppTheme.viewerOverlayText
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Rewind while held (%1)").arg(raceControls.keyText("rewind"))
                            onPressed: window.viewer.beginRewind()
                            onReleased: window.viewer.endRewind()
                            onCanceled: window.viewer.endRewind()

                            contentItem: Item {
                                Canvas {
                                    objectName: "rewindTransportIcon"
                                    anchors.centerIn: parent
                                    width: 18
                                    height: 18
                                    property color iconColor:
                                        rewindButton.effectiveTextColor
                                    antialiasing: true
                                    onIconColorChanged: requestPaint()
                                    onPaint: {
                                        const context = getContext("2d")
                                        context.clearRect(0, 0, width, height)
                                        context.fillStyle = iconColor
                                        for (const x of [1, 9]) {
                                            context.beginPath()
                                            context.moveTo(x, 9)
                                            context.lineTo(x + 8, 3)
                                            context.lineTo(x + 8, 15)
                                            context.closePath()
                                            context.fill()
                                        }
                                    }
                                }
                            }
                        }

                        RowLayout {
                            objectName: "speedControls"
                            spacing: 2
                            Layout.columnSpan: playbackDock.compact ? 4 : 1
                            Layout.alignment: Qt.AlignHCenter

                            ThemedIconButton {
                                objectName: "slowerButton"
                                icon.source: "qrc:/icons/minus.svg"
                                enabled: window.viewer.playbackSpeed > 0.011
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Slower (%1)").arg(raceControls.keyText("slower"))
                                onClicked: window.viewer.slower()
                            }
                            Label {
                                objectName: "speedLabel"
                                Layout.preferredWidth: 46
                                horizontalAlignment: Text.AlignHCenter
                                readonly property real speed: window.viewer.playbackSpeed
                                text: speed >= 1 ? speed + "×" : "1/" + Math.round(1 / speed) + "×"
                                color: speed === 1 ? AppTheme.viewerOverlayText : AppTheme.accent
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                ToolTip.visible: speedMouse.containsMouse
                                ToolTip.text: qsTr("Race speed; click for real time")
                                MouseArea {
                                    id: speedMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: window.viewer.playbackSpeed = 1
                                }
                            }
                            ThemedIconButton {
                                objectName: "fasterButton"
                                icon.source: "qrc:/icons/plus.svg"
                                enabled: window.viewer.playbackSpeed < 49.9
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Faster (%1)").arg(raceControls.keyText("faster"))
                                onClicked: window.viewer.faster()
                            }
                        }
                    }
                }
            }

            DockPanel {
                id: manualDrivePanel
                objectName: "manualDrivePanel"
                panelId: "manualDrive"
                title: qsTr("Manual driving")
                available: window.controller.viewerVisible
                           && window.viewer.manualDriving

                Rectangle {
                    id: manualDriveStatus
                    objectName: "manualDriveStatus"
                    anchors.fill: parent
                    color: "transparent"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 8
                        spacing: 6

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Manual")
                            color: AppTheme.viewerOverlayText
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }

                        Repeater {
                            model: [
                                { "symbol": "←",
                                  "active": window.viewer.manualLeft },
                                { "symbol": "↑",
                                  "active":
                                      window.viewer.manualAccelerate },
                                { "symbol": "↓",
                                  "active": window.viewer.manualBrake },
                                { "symbol": "→",
                                  "active": window.viewer.manualRight }
                            ]

                            delegate: Rectangle {
                                required property string symbol
                                required property bool active
                                Layout.preferredWidth: 26
                                Layout.preferredHeight: 24
                                radius: 4
                                color: active ? AppTheme.accent
                                              : AppTheme.viewerOverlayControl
                                border.width: 1
                                border.color:
                                    active ? AppTheme.accentBorder
                                           : AppTheme.viewerOverlayBorder

                                Label {
                                    anchors.centerIn: parent
                                    text: symbol
                                    color: active ? AppTheme.textOnAccent
                                                  : AppTheme.viewerOverlayMuted
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                }
                            }
                        }
                    }
                }
            }

            DockPanel {
                id: attemptsPanel
                objectName: "attemptsPanel"
                panelId: "attempts"
                title: qsTr("Attempts")
                available: window.controller.viewerVisible
                // The base inputs before the last Keep, for Undo.
                property string inputsBeforeKeep: ""
                property bool canUndoKeep: false
                readonly property var shown: {
                    const list = window.viewer.attempts
                    for (const item of list) {
                        if (item.id === window.viewer.selectedRunId)
                            return item
                    }
                    return null
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 6

                    Label {
                        Layout.fillWidth: true
                        visible: attemptList.count === 0
                        text: qsTr("Play the inputs (%1) and press a driving key to take over. Every race you drive is kept here; Keep makes one the base inputs.")
                              .arg(raceControls.keyText("pause"))
                        wrapMode: Text.WordWrap
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                    }

                    ListView {
                        id: attemptList
                        objectName: "attemptList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 2
                        model: window.viewer.attempts
                        delegate: ThemedItemDelegate {
                            required property var modelData
                            width: ListView.view.width
                            highlighted: modelData.id === window.viewer.selectedRunId
                            text: modelData.name
                                  + " · " + qsTr("from %1").arg(window.raceTimeText(modelData.takeoverMs))
                                  + " · " + (modelData.finished && modelData.finishTimeMs >= 0
                                             ? qsTr("finished %1").arg(window.raceTimeText(modelData.finishTimeMs))
                                             : window.raceTimeText(modelData.live ? window.viewer.durationMs : modelData.endMs))
                                  + " · CP " + modelData.checkpoints
                            font.pixelSize: 12
                            ToolTip.visible: hovered && !modelData.live
                            ToolTip.text: qsTr("Watch it from 2 s before its takeover")
                            onClicked: {
                                if (!modelData.live) {
                                    window.viewer.watchAttempt(modelData.id)
                                    manualInputFocus.forceActiveFocus()
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        ThemedButton {
                            objectName: "keepAttemptButton"
                            text: qsTr("Keep")
                            enabled: attemptsPanel.shown !== null
                                     && !window.controller.running
                                     && !window.controller.extractingReplayInputs
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Make the selected attempt the base inputs")
                            onClicked: {
                                attemptsPanel.inputsBeforeKeep = window.controller.baseInputScript
                                window.controller.baseInputScript =
                                    window.viewer.attemptScript(attemptsPanel.shown.id)
                                attemptsPanel.canUndoKeep = true
                            }
                        }
                        ThemedButton {
                            objectName: "undoKeepButton"
                            text: qsTr("Undo keep")
                            enabled: attemptsPanel.canUndoKeep
                                     && !window.controller.running
                            onClicked: {
                                window.controller.baseInputScript = attemptsPanel.inputsBeforeKeep
                                attemptsPanel.canUndoKeep = false
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            DockPanel {
                id: whiteboardPanel
                objectName: "whiteboardPanel"
                panelId: "whiteboard"
                title: qsTr("Whiteboard")
                available: window.controller.viewerVisible && window.viewer.loaded

                Item {
                    id: whiteboardToolbarHost
                    objectName: "whiteboardToolbarHost"
                    anchors.fill: parent
                }
            }

            DockPanel {
                id: drawingsPanel
                objectName: "drawingsPanel"
                panelId: "drawings"
                title: qsTr("Drawings")
                available: window.controller.viewerVisible
                           && whiteboardOverlay.drawingListOpen
                closeHandler: function() { whiteboardOverlay.drawingListOpen = false }

                Item {
                    id: whiteboardDrawingListHost
                    objectName: "whiteboardDrawingListHost"
                    anchors.fill: parent
                }
            }

            // Base, Target, Passes and Search are panels of their own (each can be
            // docked or floated on its own); Results holds what the search does.

            DockPanel {
                id: basePanel
                objectName: "basePanel"
                panelId: "base"
                title: qsTr("Base")

                Rectangle {
                    objectName: "settingsPanel"
                    anchors.fill: parent
                    color: AppTheme.panel

                    ScrollView {
                        id: baseScroll
                        objectName: "baseScroll"
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        clip: true
                        contentWidth: availableWidth
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        PanelWheelRedirector {
                            objectName: "baseWheelRedirector"
                            parent: baseScroll.parent
                            anchors.fill: parent
                            flickable: baseScroll.contentItem
                        }

                        ColumnLayout {
                            width: baseScroll.availableWidth
                            spacing: 14

                            Item {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 12
                            }

                            ColumnLayout {
                                id: replaySection

                                objectName: "replayInputStatusSection"
                                visible: (!!window.controller.replayInputFailure.reason
                                             || window.controller.replayInputStatusText.length > 0)
                                Layout.fillWidth: true
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                spacing: 6

                                FailureNotice {
                                    id: replayInputFailureNotice
                                    objectName: "replayInputFailure"
                                    Layout.fillWidth: true
                                    title: qsTr("Input extraction failed")
                                    failure: window.controller.replayInputFailure
                                }

                                Label {
                                    id: replayInputStatusLabel
                                    objectName: "replayInputStatusLabel"
                                    Layout.fillWidth: true
                                    visible: text.length > 0
                                             && !(window.controller.replayInputFailure.reason)
                                    text: window.controller.replayInputStatusText
                                    color: text.indexOf(qsTr("failed")) >= 0
                                           || text.indexOf(qsTr("discarded")) >= 0
                                           ? AppTheme.error
                                           : AppTheme.success
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 11
                                }
                            }

                            ConfigurationSection {
                                objectName: "baseInputScriptSection"
                                Layout.fillWidth: true
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                title: qsTr("Base input script")

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Item {
                                        Layout.fillWidth: true
                                    }

                                    ThemedButton {
                                        id: copyCurrentRaceInputsButton
                                        objectName: "copyCurrentRaceInputsButton"
                                        text: qsTr("Copy current race")
                                        enabled: window.viewer.canCopyCurrentInputs
                                                 && !window.controller.running
                                                 && !window.controller.extractingReplayInputs
                                        onClicked: {
                                            window.controller.baseInputScript =
                                                window.viewer.currentInputScript()
                                            baseInputScriptArea.forceActiveFocus()
                                        }
                                        ToolTip.visible: hovered
                                        ToolTip.text: qsTr(
                                            "Replace the base input with the selected race through its current time")
                                    }
                                }

                                ScrollView {
                                    id: baseInputScriptScroll

                                    objectName: "baseInputScriptScrollView"
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 220
                                    clip: true
                                    ScrollBar.horizontal.policy:
                                        ScrollBar.AsNeeded
                                    ScrollBar.vertical.policy:
                                        ScrollBar.AsNeeded

                                    TextArea {
                                        id: baseInputScriptArea

                                        objectName: "baseInputScriptTextArea"
                                        width: Math.max(
                                            baseInputScriptScroll.availableWidth,
                                            contentWidth + leftPadding + rightPadding)
                                        text: window.controller.baseInputScript
                                        enabled: !window.controller.running
                                                 && !window.controller.extractingReplayInputs
                                        selectByMouse: true
                                        wrapMode: TextEdit.NoWrap
                                        textFormat: TextEdit.PlainText
                                        font.family: "monospace"
                                        font.pixelSize: 12
                                        color: enabled ? AppTheme.text
                                                       : AppTheme.disabledText
                                        placeholderText: qsTr("0.00 press up")
                                        onActiveFocusChanged: {
                                            if (!activeFocus)
                                                window.commitBaseInputScript()
                                        }
                                        background: Rectangle {
                                            color: enabled ? AppTheme.surface
                                                           : AppTheme.disabledSurface
                                            border.width: 1
                                            border.color:
                                                window.controller.baseInputScriptError.length
                                                > 0 ? AppTheme.error
                                                    : baseInputScriptArea.activeFocus
                                                      ? AppTheme.focus : AppTheme.border
                                            radius: 6
                                        }
                                    }
                                }

                                Label {
                                    objectName: "baseInputScriptErrorLabel"
                                    Layout.fillWidth: true
                                    visible: text.length > 0
                                    text: window.controller.baseInputScriptError
                                    color: AppTheme.error
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 11
                                }
                            }

                            ColumnLayout {
                                id: appearanceControls

                                objectName: "appearanceControls"
                                Layout.fillWidth: true
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                spacing: 8

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Preferences")
                                        color: AppTheme.textMuted
                                        font.pixelSize: 11
                                    }
                                }

                                SteeringUnits {}
                                TimeUnits {}
                            }

                            Item {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 16
                            }
                        }
                    }
                }
            }

            DockPanel {
                id: targetPanel
                objectName: "targetPanel"
                panelId: "target"
                title: qsTr("Target")

                Rectangle {
                    anchors.fill: parent
                    color: AppTheme.panel

                    ScrollView {
                        id: targetScroll
                        objectName: "targetScroll"
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        clip: true
                        contentWidth: availableWidth
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        PanelWheelRedirector {
                            objectName: "targetWheelRedirector"
                            parent: targetScroll.parent
                            anchors.fill: parent
                            flickable: targetScroll.contentItem
                        }

                        ColumnLayout {
                            width: targetScroll.availableWidth
                            spacing: 14

                            Item {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 12
                            }

                            ConfigurationSection {
                                objectName: "evaluationSection"
                                Layout.fillWidth: true
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                title: qsTr("Evaluation")

                                AlgorithmSelector {
                                    objectName: "evaluationTargetSelector"
                                    Layout.fillWidth: true
                                    title: qsTr("Target")
                                    comboObjectName: "evaluationTargetCombo"
                                    iconMode: true
                                    options: window.controller.evaluationTargetOptions
                                    selectedId: window.controller.evaluationTargetId
                                    controller: window.controller
                                    viewer: window.viewer
                                    viewport: viewport
                                    onSelectionRequested: id =>
                                        window.controller.evaluationTargetId = id
                                }
                            }

                            Item {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 16
                            }
                        }
                    }
                }
            }

            DockPanel {
                id: passesPanel
                objectName: "passesPanel"
                panelId: "passes"
                title: qsTr("Passes")

                Rectangle {
                    anchors.fill: parent
                    color: AppTheme.panel

                    ColumnLayout {
                        id: passesTop
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 20
                        spacing: 8

                        ModifierPassControls {
                            Layout.fillWidth: true
                            composition: modifierComposition
                        }
                    }

                    ScrollView {
                        id: passesScroll
                        objectName: "passesScroll"
                        anchors.top: passesTop.bottom
                        anchors.topMargin: 4
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        clip: true
                        contentWidth: availableWidth
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        PanelWheelRedirector {
                            objectName: "passesWheelRedirector"
                            parent: passesScroll.parent
                            anchors.fill: parent
                            flickable: passesScroll.contentItem
                        }

                        ColumnLayout {
                            width: passesScroll.availableWidth
                            spacing: 14

                            Item {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 4
                            }

                            ConfigurationSection {
                                objectName: "modifierSection"
                                visible: window.controller.modifierPasses.length > 0
                                Layout.fillWidth: true
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                title: qsTr("Pass settings")

                                ModifierComposition {
                                    id: modifierComposition
                                    Layout.fillWidth: true
                                    onPassActivated: passesScroll.contentItem.contentY = 0
                                    controller: window.controller
                                    viewer: window.viewer
                                    options: window.controller.modifierOptions
                                    passes: window.controller.modifierPasses
                                }
                            }

                            Item {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 16
                            }
                        }
                    }
                }
            }

            DockPanel {
                id: searchPanel
                objectName: "searchPanel"
                panelId: "search"
                title: qsTr("Search")

                Rectangle {
                    anchors.fill: parent
                    color: AppTheme.panel

                    ScrollView {
                        id: searchScroll
                        objectName: "searchScroll"
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        clip: true
                        contentWidth: availableWidth
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        PanelWheelRedirector {
                            objectName: "searchWheelRedirector"
                            parent: searchScroll.parent
                            anchors.fill: parent
                            flickable: searchScroll.contentItem
                        }

                        ColumnLayout {
                            width: searchScroll.availableWidth
                            spacing: 14

                            Item {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 12
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                spacing: 6

                                Label {
                                    text: qsTr("Physics backend")
                                    font.weight: Font.Medium
                                }

                                StyledComboBox {
                                    id: simulationBackendCombo

                                    objectName: "simulationBackendCombo"
                                    Layout.fillWidth: true
                                    model: window.controller.simulationBackendOptions
                                    textRole: "label"
                                    valueRole: "id"
                                    enabled: !window.controller.running
                                             && !window.viewer.loading

                                    function synchronizeSelection() {
                                        const selected = indexOfValue(
                                            window.controller.simulationBackendId)
                                        if (selected >= 0 && currentIndex !== selected)
                                            currentIndex = selected
                                    }

                                    Component.onCompleted: synchronizeSelection()
                                    onModelChanged: Qt.callLater(synchronizeSelection)
                                    onActivated: selectedIndex =>
                                        window.controller.simulationBackendId =
                                            valueAt(selectedIndex)

                                    Connections {
                                        target: window.controller

                                        function onSimulationBackendIdChanged() {
                                            simulationBackendCombo.synchronizeSelection()
                                        }
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: window.controller.simulationBackendId
                                          === "gpus"
                                          ? qsTr("The ticked graphics cards search at the same time and share their best runs")
                                          : window.controller.simulationBackendId
                                          === "cuda"
                                          ? qsTr("NVIDIA CUDA for Stadium; compute capability 5.0+")
                                          : window.controller.simulationBackendId === "hip"
                                            ? qsTr("HIP Compute for Stadium on compatible GPUs")
                                          : window.controller.simulationBackendId
                                            === "optimized-cpu"
                                            ? qsTr("Faster runtime optimized for Stadium, may break compatibility in other environments")
                                            : window.controller.simulationBackendId
                                              === "multi-threaded-cpu"
                                              ? qsTr("Runs independent optimized CPU simulations across multiple worker threads")
                                            : qsTr("Broadest compatibility")
                                    color: AppTheme.textMuted
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 11
                                }

                                ThemedCheckBox {
                                    objectName: "cudaCardCheckBox"
                                    visible: window.controller.simulationBackendId === "gpus"
                                    text: window.controller.cudaCardName
                                    checked: window.controller.cudaCardEnabled
                                    enabled: !window.controller.running
                                    onToggled: window.controller.cudaCardEnabled = checked
                                }

                                ThemedCheckBox {
                                    objectName: "hipCardCheckBox"
                                    visible: window.controller.simulationBackendId === "gpus"
                                    text: window.controller.hipCardName
                                    checked: window.controller.hipCardEnabled
                                    enabled: !window.controller.running
                                    onToggled: window.controller.hipCardEnabled = checked
                                }

                                Label {
                                    visible: physicsModeCombo.visible
                                    text: qsTr("Physics")
                                    font.weight: Font.Medium
                                }

                                StyledComboBox {
                                    id: physicsModeCombo

                                    objectName: "physicsModeCombo"
                                    Layout.fillWidth: true
                                    visible: window.controller.approximatePhysicsOnGpu
                                             || (window.controller.simulationBackendId === "multi-threaded-cpu"
                                                 && window.controller.approximatePhysicsCpuAvailable
                                                 && Number(window.controller.cpuWorkerCount) >= 2)
                                    model: [
                                        { id: "exact", label: qsTr("Exact") },
                                        { id: "approximate", label: qsTr("Approximate") }
                                    ]
                                    textRole: "label"
                                    valueRole: "id"
                                    selectedValue: window.controller.approximatePhysics
                                                   ? "approximate" : "exact"
                                    enabled: !window.controller.running
                                             && !window.viewer.loading
                                    onActivated: selectedIndex =>
                                        window.controller.approximatePhysics =
                                            valueAt(selectedIndex) === "approximate"
                                }

                                Label {
                                    Layout.fillWidth: true
                                    visible: physicsModeCombo.visible
                                    text: window.controller.approximatePhysics
                                          ? qsTr("Searches with faster approximate physics and rebuilds each new best with exact physics. Results are the exact runs; History adds the approximate score and the drift between the two runs.")
                                          : qsTr("Searches with the game's exact physics")
                                    color: AppTheme.textMuted
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 11
                                }

                                Label {
                                    objectName: "computeModuleStatus"
                                    Layout.fillWidth: true
                                    visible: text.length > 0
                                    text: window.updater ? window.updater.computeStatus : ""
                                    color: AppTheme.textMuted
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 11
                                }

                                Label {
                                    objectName: "cudaCompatibilityStatus"
                                    Layout.fillWidth: true
                                    visible: window.cudaSettingsShown
                                    text: window.controller.cudaStatusText
                                    color: window.controller.cudaAvailable
                                           ? AppTheme.success : AppTheme.error
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 11
                                }

                                Label {
                                    objectName: "hipCompatibilityStatus"
                                    Layout.fillWidth: true
                                    visible: window.hipSettingsShown
                                    text: window.controller.hipStatusText
                                    color: window.controller.hipAvailable
                                           ? AppTheme.success : AppTheme.error
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 11
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    DurationField {
                                        Layout.fillWidth: true
                                        objectName: "simulationHorizonSettings"
                                        fieldObjectName: "simulationHorizonField"
                                        label: qsTr("Search end")
                                        value: window.controller.simulationHorizonMs
                                        running: window.controller.running
                                        minimum: 10
                                        captureVisible: true
                                        captureEnabled: window.viewer.loaded
                                        captureValue: String(Math.max(
                                            window.viewer.tickDurationMs,
                                            window.viewer.currentTick
                                            * window.viewer.tickDurationMs))
                                        captureToolTip: qsTr("Set horizon to now (10 ms tick)")
                                        maximum: 2147481040
                                        dragStep: 1000
                                        liveScrub: false
                                        onEdited: value =>
                                            window.controller.simulationHorizonMs = value
                                    }
                                    WindowChainButton {
                                        Layout.alignment: Qt.AlignTop
                                        controller: window.controller
                                        chainKey: "searchEnd"
                                        label: qsTr("Search end")
                                        running: window.controller.running
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    DurationField {
                                        Layout.fillWidth: true
                                        objectName: "previewExtentSettings"
                                        fieldObjectName: "previewExtentField"
                                        label: qsTr("Preview extent")
                                        info: qsTr("How far the viewer simulates your inputs, "
                                                   + "including history and best runs. It does "
                                                   + "not change how far the search simulates "
                                                   + "(Search end).")
                                        value: window.controller.previewExtentMs
                                        minimum: 10
                                        maximum: 2147481040
                                        dragStep: 1000
                                        liveScrub: false
                                        onEdited: value =>
                                            window.controller.previewExtentMs = value
                                    }
                                    WindowChainButton {
                                        Layout.alignment: Qt.AlignTop
                                        controller: window.controller
                                        chainKey: "previewExtent"
                                        label: qsTr("Preview extent")
                                        running: window.controller.running
                                    }
                                }
                                ThemedButton {
                                    objectName: "extendSearchHorizonButton"
                                    Layout.fillWidth: true
                                    visible: window.controller.requiredSimulationHorizonMs >
                                             Number(window.controller.simulationHorizonMs)
                                    enabled: !window.controller.running
                                    text: qsTr("Extend search to %1").arg(
                                              DurationDisplay.text(window.controller.requiredSimulationHorizonMs))
                                    onClicked: window.controller.extendSimulationHorizon()
                                }

                                ThemedCheckBox {
                                    objectName: "randomizeSeedsOnStartCheckBox"
                                    text: qsTr("Randomize modifier seeds on Start")
                                    checked: window.controller.randomizeSeedsOnStart
                                    enabled: !window.controller.running
                                    onToggled:
                                        window.controller.randomizeSeedsOnStart =
                                            checked
                                }

                                SettingTextField {
                                    objectName: "cpuWorkerSettings"
                                    visible: window.controller.simulationBackendId
                                             === "multi-threaded-cpu"
                                    fieldObjectName: "cpuWorkerCountField"
                                    label: qsTr("Worker threads")
                                    value: window.controller.cpuWorkerCount
                                    running: window.controller.running
                                    minimum: 1
                                    maximum: 256
                                    onEdited: value =>
                                        window.controller.cpuWorkerCount = value
                                }

                                SettingTextField {
                                    objectName: "cudaParallelSampleSettings"
                                    visible: window.cudaSettingsShown
                                    fieldObjectName: "cudaParallelSampleCountField"
                                    label: window.controller.simulationBackendId === "gpus"
                                           ? qsTr("%1: parallel samples at a time").arg(window.controller.cudaCardName)
                                           : qsTr("Parallel samples at a time")
                                    value: window.controller.cudaParallelSampleCount
                                    running: window.controller.running
                                    minimum: 1
                                    onEdited: value =>
                                        window.controller.cudaParallelSampleCount = value
                                }

                                SettingTextField {
                                    objectName: "hipParallelSampleSettings"
                                    visible: window.hipSettingsShown
                                    fieldObjectName: "hipParallelSampleCountField"
                                    label: window.controller.simulationBackendId === "gpus"
                                           ? qsTr("%1: parallel samples at a time").arg(window.controller.hipCardName)
                                           : qsTr("Parallel samples at a time")
                                    value: window.controller.hipParallelSampleCount
                                    running: window.controller.running
                                    minimum: 1
                                    onEdited: value =>
                                        window.controller.hipParallelSampleCount = value
                                }

                                ThemedCheckBox {
                                    objectName: "cudaCalibrationCheckBox"
                                    visible: window.cudaSettingsShown
                                    text: window.controller.simulationBackendId === "gpus"
                                          ? qsTr("%1: calibrate for maximum throughput").arg(window.controller.cudaCardName)
                                          : qsTr("Calibrate for maximum throughput")
                                    checked: window.controller.cudaCalibrationEnabled
                                    enabled: !window.controller.running
                                    onToggled:
                                        window.controller.cudaCalibrationEnabled =
                                            checked
                                }

                                ThemedCheckBox {
                                    objectName: "hipCalibrationCheckBox"
                                    visible: window.hipSettingsShown
                                    text: window.controller.simulationBackendId === "gpus"
                                          ? qsTr("%1: calibrate for maximum throughput").arg(window.controller.hipCardName)
                                          : qsTr("Calibrate for maximum throughput")
                                    checked: window.controller.hipCalibrationEnabled
                                    enabled: !window.controller.running
                                    onToggled:
                                        window.controller.hipCalibrationEnabled =
                                            checked
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 1
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                color: AppTheme.border
                            }

                            ConfigurationSection {
                                objectName: "conditionsSection"
                                Layout.fillWidth: true
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                title: qsTr("Conditions")

                                ScrollView {
                                    id: conditionScriptScroll
                                    objectName: "conditionScriptScrollView"
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 110
                                    clip: true
                                    ScrollBar.horizontal.policy: ScrollBar.AsNeeded
                                    ScrollBar.vertical.policy: ScrollBar.AsNeeded

                                    TextArea {
                                        id: conditionScriptArea
                                        objectName: "conditionScriptTextArea"
                                        width: Math.max(
                                            conditionScriptScroll.availableWidth,
                                            contentWidth + leftPadding + rightPadding)
                                        text: window.controller.conditionScript
                                        enabled: !window.controller.running
                                        selectByMouse: true
                                        wrapMode: TextEdit.NoWrap
                                        textFormat: TextEdit.PlainText
                                        font.family: "monospace"
                                        font.pixelSize: 12
                                        color: enabled ? AppTheme.text
                                                       : AppTheme.disabledText
                                        placeholderText: enabled ? qsTr("No conditions") : ""
                                        Keys.onPressed: event => {
                                            if (event.key === Qt.Key_Space && (event.modifiers & Qt.ControlModifier)) {
                                                conditionReference.complete()
                                                event.accepted = true
                                            }
                                        }
                                        onTextChanged: {
                                            if (window.controller.conditionScript !== text)
                                                window.controller.conditionScript = text
                                        }
                                        background: Rectangle {
                                            color: enabled ? AppTheme.surface
                                                           : AppTheme.disabledSurface
                                            border.width: 1
                                            border.color: conditionScriptArea.activeFocus
                                                          ? AppTheme.focus
                                                          : AppTheme.border
                                            radius: 6
                                        }
                                    }
                                }
                                Label {
                                    objectName: "emptyConditionsStatus"
                                    visible: !conditionScriptArea.enabled
                                             && conditionScriptArea.text.trim().length === 0
                                    text: qsTr("No conditions")
                                    color: AppTheme.textMuted
                                }
                                ConditionReference {
                                    id: conditionReference
                                    objectName: "conditionReference"
                                    controller: window.controller
                                    editor: conditionScriptArea
                                }
                            }

                            ConfigurationSection {
                                objectName: "searchSection"
                                Layout.fillWidth: true
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                title: qsTr("Search")

                                AlgorithmSelector {
                                    objectName: "searchAlgorithmSelector"
                                    Layout.fillWidth: true
                                    title: qsTr("Algorithm")
                                    comboObjectName: "searchAlgorithmCombo"
                                    options: window.controller.searchAlgorithmOptions
                                    selectedId: window.controller.searchAlgorithmId
                                    controller: window.controller
                                    onSelectionRequested: id =>
                                        window.controller.searchAlgorithmId = id
                                }

                                Label {
                                    text: qsTr("Autorestart")
                                    font.weight: Font.Medium
                                }

                                StyledComboBox {
                                    id: autoRestartModeCombo
                                    objectName: "autoRestartModeCombo"
                                    Layout.fillWidth: true
                                    model: [
                                        { "id": "off", "label": qsTr("Off") },
                                        { "id": "duration", "label": qsTr("Duration") },
                                        { "id": "attempts", "label": qsTr("Attempts") }
                                    ]
                                    textRole: "label"
                                    valueRole: "id"
                                    enabled: !window.controller.running
                                    selectedValue: window.controller.autoRestartMode
                                    onActivated: selectedIndex =>
                                        window.controller.autoRestartMode =
                                            valueAt(selectedIndex)
                                }

                                TextField {
                                    objectName: "autoRestartDurationField"
                                    Layout.fillWidth: true
                                    visible: window.controller.autoRestartMode
                                             === "duration"
                                    enabled: !window.controller.running
                                    text: window.controller.autoRestartDuration
                                    placeholderText: qsTr("HH:MM:SS")
                                    onTextEdited:
                                        window.controller.autoRestartDuration = text
                                }

                                TextField {
                                    objectName: "autoRestartAttemptsField"
                                    Layout.fillWidth: true
                                    visible: window.controller.autoRestartMode
                                             === "attempts"
                                    enabled: !window.controller.running
                                    text: window.controller.autoRestartAttempts
                                    placeholderText: qsTr("Individual attempts")
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    onTextEdited:
                                        window.controller.autoRestartAttempts = text
                                }
                            }

                            Item {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 16
                            }
                        }
                    }
                }
            }

            DockPanel {
                id: resultsPanel
                objectName: "resultsPanel"
                panelId: "results"
                title: qsTr("Results")

                Rectangle {
                    anchors.fill: parent
                    color: AppTheme.panel

                    ScrollView {
                        id: resultsScroll
                        objectName: "resultsScroll"
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        clip: true
                        contentWidth: availableWidth
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        PanelWheelRedirector {
                            id: settingsWheelRedirector
                            objectName: "settingsWheelRedirector"
                            parent: resultsScroll.parent
                            anchors.fill: parent
                            flickable: resultsScroll.contentItem
                        }

                        ColumnLayout {
                            width: resultsScroll.availableWidth
                            spacing: 14

                            Item {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 12
                            }

                            Label {
                                Layout.fillWidth: true
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                visible: text.length > 0 && !window.controller.running
                                text: window.controller.validationMessage
                                color: AppTheme.error
                                wrapMode: Text.WordWrap
                                font.pixelSize: 12
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                spacing: 8

                                ThemedButton {
                                    objectName: "startSearchButton"
                                    Layout.fillWidth: true
                                    text: qsTr("Start")
                                    highlighted: true
                                    enabled: window.controller.canStart
                                             && !window.viewer.manualDriving
                                    onClicked: window.controller.startSearch()
                                }

                                ThemedButton {
                                    objectName: "stopSearchButton"
                                    Layout.fillWidth: true
                                    text: window.controller.stopping
                                          ? qsTr("Stopping...")
                                          : qsTr("Stop")
                                    enabled: window.controller.running
                                             && !window.controller.stopping
                                    onClicked: window.controller.stopSearch()
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                ThemedButton {
                                    objectName: "evaluateBaseButton"
                                    text: qsTr("Evaluate base")
                                    icon.source: "qrc:/icons/gauge.svg"
                                    enabled: window.controller.canEvaluateBase
                                    onClicked: window.controller.evaluateBase()
                                }
                                ThemedButton {
                                    visible: window.controller.evaluatingBase
                                    text: qsTr("Cancel")
                                    onClicked: window.controller.cancelBaseEvaluation()
                                }
                            }

                            FailureNotice {
                                objectName: "baseEvaluationFailure"
                                Layout.fillWidth: true
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                title: qsTr("Base evaluation failed")
                                failure: window.controller.baseEvaluationResult.failure
                                         ?? ({})
                            }

                            TextArea {
                                objectName: "baseEvaluationResult"
                                Layout.fillWidth: true
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                visible: text.length > 0
                                         && !window.controller.baseEvaluationResult.failure
                                readOnly: true
                                selectByMouse: true
                                wrapMode: TextEdit.Wrap
                                textFormat: TextEdit.PlainText
                                text: window.controller.baseEvaluationText
                                color: AppTheme.text
                                font.pixelSize: 12
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 1
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                color: AppTheme.border
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 20
                                Layout.rightMargin: 20
                                spacing: 8

                                Label {
                                    Layout.fillWidth: true
                                    text: window.controller.statusText
                                    font.weight: Font.Medium
                                    wrapMode: Text.WordWrap
                                }

                                RowLayout {
                                    id: searchMetricsRow

                                    objectName: "searchMetricsRow"
                                    Layout.fillWidth: true
                                    visible: window.controller.liveMetricsVisible
                                    spacing: 6

                                    Rectangle {
                                        objectName: "iterationsMetricCard"
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: 1
                                        Layout.preferredHeight: 58
                                        radius: 7
                                        color: AppTheme.surfaceRaised
                                        border.width: 1
                                        border.color: AppTheme.border
                                        clip: true

                                        ToolTip.visible: iterationCountMouse.containsMouse
                                        ToolTip.text: window.controller.iterationCountExactText
                                        MouseArea {
                                            id: iterationCountMouse
                                            anchors.fill: parent
                                            acceptedButtons: Qt.RightButton
                                            hoverEnabled: true
                                            onClicked: iterationCountMenu.popup()
                                        }
                                        GlassMenu {
                                            id: iterationCountMenu
                                            ThemedMenuItem {
                                                text: qsTr("Copy exact count")
                                                onTriggered: {
                                                    iterationCountClipboard.selectAll()
                                                    iterationCountClipboard.copy()
                                                }
                                            }
                                        }
                                        TextEdit {
                                            id: iterationCountClipboard
                                            visible: false
                                            text: window.controller.iterationCountRawText
                                        }

                                        ColumnLayout {
                                            anchors.fill: parent
                                            anchors.margins: 8
                                            spacing: 2

                                            Label {
                                                Layout.fillWidth: true
                                                text: qsTr("Iterations")
                                                color: AppTheme.textMuted
                                                font.pixelSize: 10
                                                horizontalAlignment: Text.AlignHCenter
                                            }

                                            Label {
                                                objectName: "iterationsMetricValue"
                                                Layout.fillWidth: true
                                                text: window.controller.iterationCountText
                                                color: AppTheme.text
                                                font.family: "monospace"
                                                font.pixelSize: 13
                                                font.weight: Font.DemiBold
                                                horizontalAlignment: Text.AlignHCenter
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }

                                    Rectangle {
                                        objectName: "throughputMetricCard"
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: 1
                                        Layout.preferredHeight: 58
                                        radius: 7
                                        color: AppTheme.surfaceRaised
                                        border.width: 1
                                        border.color: AppTheme.border
                                        clip: true

                                        ColumnLayout {
                                            anchors.fill: parent
                                            anchors.margins: 8
                                            spacing: 2

                                            Label {
                                                Layout.fillWidth: true
                                                text: qsTr("Throughput")
                                                color: AppTheme.textMuted
                                                font.pixelSize: 10
                                                horizontalAlignment: Text.AlignHCenter
                                            }

                                            Label {
                                                objectName: "throughputMetricValue"
                                                Layout.fillWidth: true
                                                text: window.controller.throughputText.length > 0
                                                      ? window.controller.throughputText
                                                            + qsTr(" /s")
                                                      : ""
                                                color: AppTheme.text
                                                font.family: "monospace"
                                                font.pixelSize: 13
                                                font.weight: Font.DemiBold
                                                horizontalAlignment: Text.AlignHCenter
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }

                                    Rectangle {
                                        objectName: "elapsedMetricCard"
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: 1
                                        Layout.preferredHeight: 58
                                        radius: 7
                                        color: AppTheme.surfaceRaised
                                        border.width: 1
                                        border.color: AppTheme.border
                                        clip: true

                                        ColumnLayout {
                                            anchors.fill: parent
                                            anchors.margins: 8
                                            spacing: 2

                                            Label {
                                                Layout.fillWidth: true
                                                text: qsTr("Elapsed")
                                                color: AppTheme.textMuted
                                                font.pixelSize: 10
                                                horizontalAlignment: Text.AlignHCenter
                                            }

                                            Label {
                                                objectName: "elapsedMetricValue"
                                                Layout.fillWidth: true
                                                text: window.controller.elapsedText
                                                color: AppTheme.text
                                                font.family: "monospace"
                                                font.pixelSize: 12
                                                font.weight: Font.DemiBold
                                                horizontalAlignment: Text.AlignHCenter
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }
                                }

                                ProgressBar {
                                    Layout.fillWidth: true
                                    from: 0
                                    to: 1
                                    value: window.controller.progressValue
                                    indeterminate:
                                        window.controller.progressIndeterminate
                                }

                                FailureNotice {
                                    objectName: "searchFailure"
                                    Layout.fillWidth: true
                                    failure: window.controller.statusText
                                                     === qsTr("Search failed")
                                             ? window.controller.failureDiagnostic
                                             : ({})
                                }

                                SearchInputs {
                                    id: searchInputs
                                    Layout.fillWidth: true
                                    controller: window.controller

                                    SearchSessionHistory {
                                        Layout.fillWidth: true
                                        controller: window.controller
                                        viewer: window.viewer
                                        onInputsSelected: searchInputs.selectedSource = 1
                                        onLiveSelected: searchInputs.selectedSource = 0
                                    }
                                }
                            }

                            Item {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 16
                            }
                        }
                    }
                }
            }

            DockPanel {
                id: codePanel
                objectName: "codePanel"
                panelId: "code"
                title: qsTr("Code")
                onOnTopChanged: window.updateSimulationDebugger()

                // Same wheel steps in the source tree and code as in the
                // other panels; elsewhere the wheel does nothing.
                PanelWheelRedirector {
                    objectName: "codeWheelRedirector"
                    anchors.fill: parent
                    flickable: codeWheelSink
                }
                Flickable {
                    id: codeWheelSink
                    width: 0
                    height: 0
                    interactive: false
                }

                SimulationDebuggerPanel {
                    id: simulationDebuggerPanel
                    objectName: "simulationDebuggerPanel"
                    anchors.fill: parent
                    anchors.margins: 12
                    expanded: window.codeEditorExpanded
                    viewer: window.viewer
                    onExpansionRequested: function(expanded) {
                        window.codeEditorExpanded = expanded
                    }
                }
            }
        }

        GlassMenuBar {
            id: menuBar
            objectName: "mainMenuBar"
            width: parent.width
            height: window.menuBarHeight
            backdropOrigin: Qt.point(-raceBackdrop.x, -raceBackdrop.y)

            GlassMenu {
                id: fileMenu
                objectName: "fileMenu"
                title: qsTr("File")

                ThemedMenuItem {
                    objectName: "openMapMenuItem"
                    text: window.viewer.loading ? qsTr("Loading map...")
                                                : qsTr("Open map or replay...")
                    shortcutText: "Ctrl+O"
                    enabled: window.canOpenMap
                    onTriggered: window.openMapOrReplay()
                }
                ThemedMenuItem {
                    objectName: "packsFolderMenuItem"
                    text: qsTr("Set packs folder...")
                    enabled: !window.controller.running
                             && !window.controller.extractingReplayInputs
                    onTriggered: packsDialog.open()
                }
                ThemedMenuItem {
                    objectName: "extractInputsMenuItem"
                    text: window.controller.extractingReplayInputs
                          ? qsTr("Extracting...")
                          : qsTr("Extract inputs from replay")
                    enabled: window.canExtractInputs
                    onTriggered: window.extractInputs()
                }
                ThemedMenuItem {
                    objectName: "exportVideoMenuItem"
                    visible: videoExportDialog.exporter.available
                    height: visible ? implicitHeight : 0
                    text: videoExportDialog.exporter.running
                          ? qsTr("Exporting to MP4...")
                          : qsTr("Export to MP4...")
                    enabled: window.viewer.loaded
                    onTriggered: videoExportDialog.open()
                }
                MenuSeparator {}
                ThemedMenuItem {
                    objectName: "quitMenuItem"
                    text: qsTr("Quit")
                    shortcutText: "Ctrl+Q"
                    onTriggered: window.close()
                }
            }

            GlassMenu {
                id: editMenu
                objectName: "editMenu"
                title: qsTr("Edit")

                ThemedMenuItem {
                    objectName: "undoMenuItem"
                    text: qsTr("Undo")
                    shortcutText: "Ctrl+Z"
                    enabled: window.canUndoEdit
                    onTriggered: window.undoEdit()
                }
                ThemedMenuItem {
                    objectName: "redoMenuItem"
                    text: qsTr("Redo")
                    shortcutText: "Ctrl+Shift+Z"
                    enabled: window.canRedoEdit
                    onTriggered: window.redoEdit()
                }
                MenuSeparator {}
                ThemedMenuItem {
                    text: qsTr("Cut")
                    shortcutText: "Ctrl+X"
                    enabled: window.lastTextEditor !== null
                    onTriggered: window.editText("cut")
                }
                ThemedMenuItem {
                    text: qsTr("Copy")
                    shortcutText: "Ctrl+C"
                    enabled: window.lastTextEditor !== null
                    onTriggered: window.editText("copy")
                }
                ThemedMenuItem {
                    text: qsTr("Paste")
                    shortcutText: "Ctrl+V"
                    enabled: window.lastTextEditor !== null
                    onTriggered: window.editText("paste")
                }
                ThemedMenuItem {
                    text: qsTr("Select all")
                    shortcutText: "Ctrl+A"
                    enabled: window.lastTextEditor !== null
                    onTriggered: window.editText("selectAll")
                }
                MenuSeparator {}
                ThemedMenuItem {
                    objectName: "preferencesMenuItem"
                    text: qsTr("Preferences...")
                    onTriggered: preferencesDialog.open()
                }
            }

            GlassMenu {
                id: viewMenu
                objectName: "viewMenu"
                title: qsTr("View")

                PanelMenuItem { panelId: "inputs"; text: qsTr("Inputs") }
                PanelMenuItem { panelId: "graphics"; text: qsTr("Graphics") }
                PanelMenuItem { panelId: "base"; text: qsTr("Base") }
                PanelMenuItem { panelId: "target"; text: qsTr("Target") }
                PanelMenuItem { panelId: "passes"; text: qsTr("Passes") }
                PanelMenuItem { panelId: "search"; text: qsTr("Search") }
                PanelMenuItem { panelId: "results"; text: qsTr("Results") }
                PanelMenuItem { panelId: "code"; text: qsTr("Code") }
                PanelMenuItem { panelId: "race"; text: qsTr("Race viewer") }
                PanelMenuItem { panelId: "camera"; text: qsTr("Camera") }
                PanelMenuItem { panelId: "telemetry"; text: qsTr("Scripted telemetry") }
                PanelMenuItem { panelId: "splits"; text: qsTr("Splits") }
                PanelMenuItem { panelId: "playback"; text: qsTr("Playback") }
                PanelMenuItem { panelId: "manualDrive"; text: qsTr("Manual driving") }
                PanelMenuItem { panelId: "attempts"; text: qsTr("Attempts") }
                PanelMenuItem { panelId: "whiteboard"; text: qsTr("Whiteboard") }
                ThemedMenuItem {
                    text: qsTr("Drawings")
                    showMark: true
                    marked: whiteboardOverlay.drawingListOpen
                    onTriggered: whiteboardOverlay.drawingListOpen = !whiteboardOverlay.drawingListOpen
                }
                MenuSeparator {}
                ThemedMenuItem {
                    objectName: "panelHeadersMenuItem"
                    text: qsTr("Panel headers")
                    showMark: true
                    marked: dockSpace.allHeadersShown
                    onTriggered: dockSpace.setAllHeadersShown(!dockSpace.allHeadersShown)
                }
                ThemedMenuItem {
                    objectName: "hideAllPanelsMenuItem"
                    text: qsTr("Hide all panels")
                    shortcutText: "Tab"
                    showMark: true
                    marked: dockSpace.hideAll
                    onTriggered: dockSpace.hideAll = !dockSpace.hideAll
                }
                ThemedMenuItem {
                    objectName: "showViewerMenuItem"
                    text: qsTr("Show viewer")
                    showMark: true
                    marked: window.controller.viewerVisible
                    enabled: !window.viewer.manualDriving
                    onTriggered: window.controller.viewerVisible = !window.controller.viewerVisible
                }
                MenuSeparator {}
                ThemedMenuItem {
                    objectName: "fullScreenMenuItem"
                    text: qsTr("Full screen")
                    shortcutText: "F11"
                    showMark: true
                    marked: window.visibility === Window.FullScreen
                    onTriggered: window.toggleFullScreen()
                }
                ThemedMenuItem {
                    text: qsTr("Zoom in")
                    shortcutText: "Ctrl++"
                    enabled: AppTheme.zoom < 1.6
                    onTriggered: window.setZoom(AppTheme.zoom + 0.1)
                }
                ThemedMenuItem {
                    text: qsTr("Zoom out")
                    shortcutText: "Ctrl+-"
                    enabled: AppTheme.zoom > 0.7
                    onTriggered: window.setZoom(AppTheme.zoom - 0.1)
                }
                ThemedMenuItem {
                    text: qsTr("Reset zoom (%1%)").arg(Math.round(AppTheme.zoom * 100))
                    shortcutText: "Ctrl+0"
                    onTriggered: window.setZoom(1.0)
                }
                MenuSeparator {}
                ThemedMenuItem {
                    objectName: "glassLookMenuItem"
                    text: qsTr("Glass look...")
                    onTriggered: glassLookPopup.open()
                }
                ThemedMenuItem {
                    objectName: "resetLayoutMenuItem"
                    text: qsTr("Reset layout")
                    onTriggered: dockSpace.resetLayout()
                }
            }

            GlassMenu {
                id: helpMenu
                objectName: "helpMenu"
                title: qsTr("Help")

                ThemedMenuItem {
                    objectName: "checkCarMenuItem"
                    text: qsTr("Check the car")
                    enabled: window.logs !== null && window.viewer.loaded
                    onTriggered: window.checkCar()
                }
                ThemedMenuItem {
                    objectName: "openLogFolderMenuItem"
                    text: qsTr("Open log folder")
                    enabled: window.logs !== null
                    onTriggered: window.logs.openFolder()
                }
            }
        }
    }

    // The menu bar as drawn, in the log (a tester's showed no Help menu).
    Timer {
        interval: 2000
        running: window.logs !== null
        onTriggered: {
            const menus = []
            for (let index = 0; index < menuBar.count; ++index) {
                const menu = menuBar.menuAt(index)
                const item = menuBar.itemAt(index)
                menus.push((menu ? menu.title : "?") + (item
                        ? " (" + (item.visible ? "shown" : "hidden") + ", x " + Math.round(item.x)
                          + ", width " + Math.round(item.width) + ")"
                        : " (no item)"))
            }
            window.logs.note("menu bar: " + menus.join(", ") + "; " + Math.round(menuBar.width)
                             + " wide, zoom " + AppTheme.zoom)
        }
    }

    // ---- File > Set packs folder (also offered when one is found) -------------
    GlassDialog {
        id: packsDialog
        objectName: "packsDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(560, parent.width - 32)
        title: qsTr("Packs directory")
        modal: true
        footer: Item {
            implicitHeight: 56
            ThemedButton {
                objectName: "closePacksDialogButton"
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Close")
                onClicked: packsDialog.close()
            }
        }
        // Set when Open map or replay needed the packs folder first.
        property bool loadAfter: false
        onClosed: {
            if (loadAfter)
                window.loadMapIfReady()
            loadAfter = false
        }

    ColumnLayout {
        objectName: "packsDirectorySection"
        width: parent ? parent.width : implicitWidth
        spacing: 6

        Label {
            text: qsTr("Packs directory")
            font.weight: Font.Medium
        }

        Rectangle {
            objectName: "autoPacksSuggestion"
            Layout.fillWidth: true
            implicitHeight: autoPacksSuggestionLayout.implicitHeight
                            + 16
            radius: 8
            color: AppTheme.accentSoft
            border.width: 1
            border.color: AppTheme.accentBorder
            visible:
                window.controller.autoDetectedPacksDirectory.length
                > 0

            RowLayout {
                id: autoPacksSuggestionLayout
                anchors.fill: parent
                anchors.margins: 8
                spacing: 8

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Label {
                        objectName: "autoPacksSuggestionText"
                        Layout.fillWidth: true
                        text: qsTr("This location was found automatically and should work. Apply?")
                        color: AppTheme.text
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                        font.weight: Font.Medium
                    }

                    Label {
                        Layout.fillWidth: true
                        text: window.controller.autoDetectedPacksDirectory
                        color: AppTheme.textMuted
                        elide: Text.ElideMiddle
                        font.family: "monospace"
                        font.pixelSize: 9
                    }
                }

                ThemedButton {
                    objectName: "applyAutoPacksButton"
                    text: qsTr("Apply")
                    enabled: !window.controller.running
                             && !window.controller.extractingReplayInputs
                    onClicked:
                        window.controller.applyAutoDetectedPacksDirectory()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                Layout.fillWidth: true
                text: window.controller.packsDirectory
                enabled: !window.controller.running
                         && !window.controller.extractingReplayInputs
                placeholderText: qsTr("Select installed Packs directory")
                selectByMouse: true
                onTextEdited:
                    window.controller.packsDirectory = text
            }

            ThemedButton {
                text: qsTr("Browse")
                enabled: !window.controller.running
                         && !window.controller.extractingReplayInputs
                onClicked:
                    window.controller.browseForPacksDirectory()
            }
        }

    }
    }

    Connections {
        target: window.controller
        function onAutoDetectedPacksDirectoryChanged() {
            if (window.controller.autoDetectedPacksDirectory.length > 0
                    && !packsDialog.visible)
                packsDialog.open()
        }
    }
}
