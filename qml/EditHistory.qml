import QtQuick

QtObject {
    id: history

    required property var controller
    required property var viewer
    property string renderMode: "textured"
    property var states: []
    property int position: 0
    property bool restoring: false
    property bool pending: false
    property real lastSignalTime: 0
    property int gestureDepth: 0
    readonly property bool canUndo: position > 0 || pending
    readonly property bool canRedo: position < states.length - 1

    signal renderModeRequested(string mode)

    readonly property var scalarNames: [
        "packsDirectory", "replayPath", "baseInputScript",
        "simulationBackendId", "simulationHorizonMs",
        "conditionScript", "cpuWorkerCount", "cudaParallelSampleCount",
        "cudaCalibrationEnabled", "hipParallelSampleCount",
        "hipCalibrationEnabled", "vulkanParallelSampleCount",
        "vulkanCalibrationEnabled", "cudaSessionSpecializationEnabled",
        "randomizeSeedsOnStart", "drawTargetsThroughBlocks", "darkMode"
    ]
    readonly property var cuboidKeys:
        ["id", "name", "centerX", "centerY", "centerZ",
         "sizeX", "sizeY", "sizeZ", "selected"]
    readonly property var volumeKeys:
        ["id", "name", "plane", "originX", "originY", "originZ",
         "depth", "polygon", "selected"]
    readonly property var poseKeys:
        ["id", "name", "x", "y", "z", "yawDegrees",
         "pitchDegrees", "rollDegrees", "selected"]

    function plainTargets(targets, keys) {
        return targets.map(target => {
            const result = {}
            for (const key of keys)
                result[key] = target[key]
            return result
        })
    }

    function capture() {
        const scalars = {}
        for (const key of scalarNames)
            scalars[key] = controller[key]
        return {
            scalars: scalars,
            algorithm: { id: controller.searchAlgorithmId,
                         settings: Object.assign({}, controller.searchAlgorithmSettings) },
            evaluation: { id: controller.evaluationTargetId,
                          settings: Object.assign({}, controller.evaluationTargetSettings) },
            passes: controller.modifierPasses.map(pass => ({
                id: pass.id, settings: Object.assign({}, pass.settings) })),
            cuboids: plainTargets(controller.cuboidTargets.targets, cuboidKeys),
            volumes: plainTargets(controller.customVolumeTargets.targets,
                                  volumeKeys),
            poses: plainTargets(controller.poseTargets.targets, poseKeys),
            whiteboard: viewer.whiteboard.captureHistorySnapshot(),
            telemetryScript: viewer.telemetryScript,
            styles: viewer.visualStylesSnapshot(),
            renderMode: renderMode
        }
    }

    function reset() {
        pending = false
        gestureDepth = 0
        settleTimer.stop()
        states = [capture()]
        position = 0
        viewer.whiteboard.pruneHistorySnapshots(
            states.map(state => state.whiteboard))
    }

    function noteChange() {
        if (restoring || controller.running || viewer.manualDriving)
            return
        if (!states.length) {
            reset()
            return
        }
        if (gestureDepth > 0) {
            pending = true
            return
        }
        const now = Date.now()
        if (!pending || now - lastSignalTime >= 50) {
            pending = true
            lastSignalTime = now
            settleTimer.restart()
        }
    }

    function beginGesture() {
        if (gestureDepth === 0 && pending) {
            settleTimer.stop()
            settle()
        }
        ++gestureDepth
    }

    function endGesture() {
        if (gestureDepth <= 0)
            return
        --gestureDepth
        if (gestureDepth === 0) {
            settleTimer.stop()
            settle()
        }
    }

    function settle() {
        if (!pending || restoring)
            return
        pending = false
        const next = capture()
        if (JSON.stringify(next) === JSON.stringify(states[position])) {
            viewer.whiteboard.pruneHistorySnapshots(
                states.map(state => state.whiteboard))
            return
        }
        const updated = states.slice(0, position + 1)
        updated.push(next)
        if (updated.length > 501)
            updated.shift()
        states = updated
        position = states.length - 1
        viewer.whiteboard.pruneHistorySnapshots(
            states.map(state => state.whiteboard))
    }

    function restore(snapshot) {
        restoring = true
        try {
            const different = (left, right) =>
                JSON.stringify(left) !== JSON.stringify(right)
            if (different(plainTargets(controller.cuboidTargets.targets,
                                       cuboidKeys), snapshot.cuboids))
                controller.cuboidTargets.restoreTargets(snapshot.cuboids)
            if (different(plainTargets(controller.customVolumeTargets.targets,
                                       volumeKeys), snapshot.volumes))
                controller.customVolumeTargets.restoreTargets(snapshot.volumes)
            if (different(plainTargets(controller.poseTargets.targets,
                                       poseKeys), snapshot.poses))
                controller.poseTargets.restoreTargets(snapshot.poses)
            viewer.whiteboard.restoreHistorySnapshot(snapshot.whiteboard)
            for (const key of scalarNames)
                controller[key] = snapshot.scalars[key]
            controller.searchAlgorithmId = snapshot.algorithm.id
            for (const key in snapshot.algorithm.settings)
                controller.setSearchAlgorithmSetting(
                    key, String(snapshot.algorithm.settings[key]))
            const currentPasses = controller.modifierPasses.map(pass => ({
                id: pass.id, settings: Object.assign({}, pass.settings) }))
            if (different(currentPasses, snapshot.passes)) {
                for (let index = controller.modifierPasses.length - 1;
                     index >= 0; --index)
                    controller.removeModifierPass(index)
                for (const pass of snapshot.passes) {
                    const index = controller.modifierPasses.length
                    controller.addModifierPass(pass.id)
                    for (const key in pass.settings)
                        controller.setModifierPassSetting(
                            index, key, String(pass.settings[key]))
                }
            }
            controller.evaluationTargetId = snapshot.evaluation.id
            for (const key in snapshot.evaluation.settings)
                controller.setEvaluationTargetSetting(
                    key, String(snapshot.evaluation.settings[key]))
            viewer.restoreVisualStyles(snapshot.styles)
            viewer.telemetryScript = snapshot.telemetryScript
            renderModeRequested(snapshot.renderMode)
        } finally {
            restoring = false
        }
    }

    function undo() {
        settleTimer.stop()
        settle()
        if (position <= 0)
            return false
        --position
        restore(states[position])
        return true
    }

    function redo() {
        settleTimer.stop()
        settle()
        if (position >= states.length - 1)
            return false
        ++position
        restore(states[position])
        return true
    }

    property Timer settleTimer: Timer {
        interval: 180
        repeat: false
        onTriggered: history.settle()
    }

    Component.onCompleted: Qt.callLater(reset)
}
