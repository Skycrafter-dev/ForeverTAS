import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes

Item {
    id: root

    property var controller
    property var viewer
    property var projector
    property bool suppressed: false
    readonly property string targetId: controller.evaluationTargetId
    readonly property var settings: controller.evaluationTargetSettings
    readonly property var samples: {
        const selected = viewer.selectedRunId
        const runs = viewer.runOptions
        // Materialize once: repeatedly indexing a QVariantList from JavaScript
        // wraps its maps and vectors again on every access.
        return viewer.selectedRunSamples().map(sample => ({
            timeMs: Number(sample.timeMs), position: sample.position,
            rotation: sample.rotation, velocity: sample.velocity,
            checkpointsCollected: sample.checkpointsCollected,
            raceCompleted: sample.raceCompleted,
            finishTimeMs: sample.finishTimeMs, stuntsScore: sample.stuntsScore
        }))
    }
    readonly property var selectedPose: controller.poseTargets.selectedTarget
    readonly property var selectedCustom:
        controller.customVolumeTargets.selectedTarget
    property var dragPreview: null
    property var summary: ({ label: "", event: null })
    // A drag may emit many setting changes before the next rendered frame.
    onSamplesChanged: {
        Qt.callLater(refreshSummary)
        projectionDirty = true
    }
    onTargetIdChanged: Qt.callLater(refreshSummary)
    onSettingsChanged: Qt.callLater(refreshSummary)
    onSelectedPoseChanged: Qt.callLater(refreshSummary)
    onSelectedCustomChanged: Qt.callLater(refreshSummary)
    Component.onCompleted: Qt.callLater(refreshSummary)

    Connections {
        target: root.controller.cuboidTargets
        function onSelectedTargetChanged() {
            Qt.callLater(root.refreshSummary)
        }
    }

    function refreshSummary() {
        summary = calculateSummary()
    }
    readonly property var current: calculateCurrent()
    readonly property var condition: {
        const script = controller.conditionScript
        const time = viewer.timeMs
        const selected = viewer.selectedRunId
        return script.trim().length ? viewer.conditionPreview(script) : ({})
    }
    readonly property var telemetryContext: {
        const target = controller.evaluationTargetOptions.find(
            option => option.id === targetId)?.label ?? ""
        const name = qsTr("Target: ") + target + qsTr(" · selected run")
        const detail = [name, summary.label, current]
            .filter(line => line.length > 0).join("\n")
        const hasCondition = controller.conditionScript.trim().length > 0
        const conditionColor = condition.available
            ? condition.passed ? AppTheme.success : AppTheme.error
            : AppTheme.viewerOverlayMuted
        const score = Number(summary.score)
        const deadline = Number(settings.targetTimeMs)
        const context = {
            "target.name": target,
            "target.summary": summary.label,
            "target.now": current,
            "target.readout": detail,
            "conditions.acceptance": {
                text: hasCondition
                    ? qsTr("Conditions: ") + (condition.message ?? "") : "",
                color: conditionColor
            }
        }
        if (Number.isFinite(score))
            context["target.score"] = score
        if (targetId === "stunt-points" && Number.isFinite(deadline))
            context["stunt.deadline"] = deadline
        return context
    }
    readonly property var requestedProjectionFrame: {
        const camera = projector.camera
        return [camera.scenePosition, camera.sceneRotation,
                camera.fieldOfView, projector.width, projector.height,
                camera.clipNear]
    }
    property var projectionFrame: null
    property bool projectionDirty: true
    onRequestedProjectionFrameChanged: projectionDirty = true
    FrameAnimation {
        running: root.visible && root.projectionDirty
        onTriggered: {
            root.projectionDirty = false
            root.projectionFrame = root.requestedProjectionFrame
        }
    }
    readonly property var screenSamples: {
        const cameraFrame = projectionFrame
        const runSamples = samples
        if (!cameraFrame)
            return []
        return viewer.projectSelectedRunSamples(
            cameraFrame[0], cameraFrame[1], cameraFrame[2],
            cameraFrame[3], cameraFrame[4], cameraFrame[5])
            .map(sample => ({timeMs: Number(sample.timeMs),
                             point: sample.point ?? null}))
    }
    readonly property var velocityVisual: calculateVelocityVisual()
    readonly property var passColors: ["#b782df", "#f09d6c", "#69bde7",
                                      "#cf7db2", "#8bc46f", "#e4c15d",
                                      "#8e9fe5", "#da7b78", "#60c6b2",
                                      "#c793e6", "#d7ad70", "#9cc96a"]
    function style(id, color, width) {
        const revision = viewer.visualStyleRevision
        const saved = viewer.visualStyle(id)
        return { visible: saved.visible ?? true,
                 throughBlocks: saved.throughBlocks ?? true,
                 color: saved.color ?? color,
                 width: saved.width ?? width,
                 opacity: saved.opacity ?? (id === "evaluation:window"
                                             ? 0.55 : 1),
                 linePattern: saved.linePattern
                     ?? (id === "condition:threshold" ? "dash" : "solid") }
    }
    function stroke(style) {
        const color = Qt.lighter(style.color, 1)
        return Qt.rgba(color.r, color.g, color.b,
                       color.a * style.opacity)
    }
    readonly property var windowStyle: style("evaluation:window", "#80e0b4", 4)
    readonly property var horizonStyle: style("simulation:horizon", "#e8b959", 2)
    readonly property var stuntStyle: style("evaluation:stunt-deadline", "#fff276", 2)
    readonly property var pointStyle: style("target:point", "#f6a45a", 2)
    readonly property var velocityStyle: style("target:velocity", "#65d0e2", 3)
    readonly property var finishStyle: style("target:finish", "#fff276", 2)
    readonly property var eventStyle:
        targetId === "precise-finish-time" ? finishStyle
        : targetId === "stunt-points" ? stuntStyle
        : style("evaluation:event", "#fff276", 2)
    readonly property var conditionStyle: style("condition:threshold", "#e47a77", 1.5)
    readonly property var windowHandles: {
        const result = []
        const evaluation = windowRange("evaluation", -1)
        const min = evaluation[0]
        const max = evaluation[1]
        if (Number.isFinite(min) && Number.isFinite(max)) {
            result.push({ kind: "evaluation", endpoint: "minTimeMs",
                          time: min, other: max, index: -1 })
            result.push({ kind: "evaluation", endpoint: "maxTimeMs",
                          time: max, other: min, index: -1 })
        }
        const modifiers = controller.modifierPasses
        for (let index = 0; index < modifiers.length; ++index) {
            if (modifiers[index].enabled === false) continue
            const window = windowRange("modifier", index)
            const start = window[0]
            const end = window[1]
            if (!Number.isFinite(start) || !Number.isFinite(end))
                continue
            result.push({ kind: "modifier", endpoint: "minTimeMs",
                          time: start, other: end, index: index })
            result.push({ kind: "modifier", endpoint: "maxTimeMs",
                          time: end, other: start, index: index })
        }
        if (targetId === "stunt-points") {
            const deadline = stuntTime()
            if (Number.isFinite(deadline))
                result.push({ kind: "stunt", endpoint: "targetTimeMs",
                              time: deadline, index: -1 })
        }
        const horizon = horizonTime()
        if (Number.isFinite(horizon))
            result.push({ kind: "horizon", endpoint: "simulationHorizonMs",
                          time: horizon, index: -1 })
        return result
    }

    objectName: "evaluationRaceOverlay"
    visible: viewer.loaded && !suppressed
    clip: true
    z: 2.8

    function distance(a, b) {
        return Math.hypot(a.x - b.x, a.y - b.y, a.z - b.z)
    }

    function pointTarget() {
        return Qt.vector3d(Number(settings.x), Number(settings.y),
                           Number(settings.z))
    }

    function direction() {
        const x = Number(settings.directionX)
        const y = Number(settings.directionY)
        const z = Number(settings.directionZ)
        const length = Math.hypot(x, y, z)
        return length > 0 ? Qt.vector3d(x / length, y / length, z / length)
                          : Qt.vector3d(1, 0, 0)
    }

    function dot(a, b) {
        return a.x * b.x + a.y * b.y + a.z * b.z
    }

    function insideWindow(time) {
        return time >= Number(settings.minTimeMs)
               && time <= Number(settings.maxTimeMs)
    }

    function sampleAt(time) {
        if (!Number.isFinite(time) || !samples.length || time < samples[0].timeMs
            || time > samples[samples.length - 1].timeMs)
            return null
        let low = 0
        let high = samples.length - 1
        while (low < high) {
            const middle = Math.floor((low + high) / 2)
            if (samples[middle].timeMs < time)
                low = middle + 1
            else
                high = middle
        }
        if (low === 0)
            return samples[0]
        const before = samples[low - 1]
        const after = samples[low]
        const portion = after.timeMs === before.timeMs ? 0
            : Math.max(0, Math.min(1,
                (time - before.timeMs) / (after.timeMs - before.timeMs)))
        return {
            timeMs: time,
            position: Qt.vector3d(
                before.position.x + (after.position.x
                    - before.position.x) * portion,
                before.position.y + (after.position.y
                    - before.position.y) * portion,
                before.position.z + (after.position.z
                    - before.position.z) * portion),
            stuntsScore: portion < 1 ? before.stuntsScore : after.stuntsScore
        }
    }

    function cuboidEntry(a, b, box) {
        const center = box.center
        const size = box.size
        if (!center || !size)
            return -1
        let enter = 0
        let leave = 1
        for (const axis of ["x", "y", "z"]) {
            const minimum = center[axis] - size[axis] / 2
            const maximum = center[axis] + size[axis] / 2
            const delta = b[axis] - a[axis]
            if (Math.abs(delta) <= 1e-12) {
                if (a[axis] < minimum || a[axis] > maximum)
                    return -1
                continue
            }
            let near = (minimum - a[axis]) / delta
            let far = (maximum - a[axis]) / delta
            if (near > far) [near, far] = [far, near]
            enter = Math.max(enter, near)
            leave = Math.min(leave, far)
            if (enter > leave)
                return -1
        }
        return enter
    }

    function projectCustom(position, shape) {
        const origin = shape.origin
        if (shape.plane === "xy")
            return { u: position.x - origin.x,
                     v: position.y - origin.y,
                     n: position.z - origin.z }
        if (shape.plane === "yz")
            return { u: position.y - origin.y,
                     v: position.z - origin.z,
                     n: position.x - origin.x }
        return { u: position.x - origin.x,
                 v: position.z - origin.z,
                 n: position.y - origin.y }
    }

    function polygonContains(point, vertices) {
        let inside = false
        for (let i = 0, j = vertices.length - 1;
             i < vertices.length; j = i++) {
            const a = vertices[j]
            const b = vertices[i]
            const ax = Number(a.u), ay = Number(a.v)
            const bx = Number(b.u), by = Number(b.v)
            const cross = (point.u - ax) * (by - ay)
                        - (point.v - ay) * (bx - ax)
            if (Math.abs(cross) <= 1e-9
                && point.u >= Math.min(ax, bx) - 1e-9
                && point.u <= Math.max(ax, bx) + 1e-9
                && point.v >= Math.min(ay, by) - 1e-9
                && point.v <= Math.max(ay, by) + 1e-9)
                return true
            if ((ay > point.v) !== (by > point.v)
                && point.u < (bx - ax) * (point.v - ay)
                             / (by - ay) + ax)
                inside = !inside
        }
        return inside
    }

    function customContains(position, shape) {
        if (!shape.valid || !shape.vertices || shape.vertices.length < 3)
            return false
        const point = projectCustom(position, shape)
        return point.n >= 0 && point.n <= Number(shape.depth)
               && polygonContains(point, shape.vertices)
    }

    function customEntry(a, b, shape) {
        if (!shape.valid || !shape.vertices || shape.vertices.length < 3)
            return -1
        const first = projectCustom(a, shape)
        const last = projectCustom(b, shape)
        const candidates = [0, 1]
        const dn = last.n - first.n
        if (Math.abs(dn) > 1e-12) {
            candidates.push(-first.n / dn)
            candidates.push((Number(shape.depth) - first.n) / dn)
        }
        const pu = last.u - first.u, pv = last.v - first.v
        const vertices = shape.vertices
        for (let i = 0; i < vertices.length; ++i) {
            const a2 = vertices[i]
            const b2 = vertices[(i + 1) % vertices.length]
            const au = Number(a2.u), av = Number(a2.v)
            const eu = Number(b2.u) - au
            const ev = Number(b2.v) - av
            const denominator = pu * ev - pv * eu
            if (Math.abs(denominator) <= 1e-12)
                continue
            const ou = au - first.u, ov = av - first.v
            const fraction = (ou * ev - ov * eu) / denominator
            const edgeFraction = (ou * pv - ov * pu) / denominator
            if (edgeFraction >= -1e-9 && edgeFraction <= 1 + 1e-9)
                candidates.push(fraction)
        }
        const sorted = candidates.filter(t => t >= -1e-9 && t <= 1 + 1e-9)
                                 .map(t => Math.max(0, Math.min(1, t)))
                                 .sort((a, b) => a - b)
        for (let i = 0; i < sorted.length; ++i) {
            const t = sorted[i]
            const at = fraction => Qt.vector3d(
                a.x + (b.x - a.x) * fraction,
                a.y + (b.y - a.y) * fraction,
                a.z + (b.z - a.z) * fraction)
            if (customContains(at(t), shape)
                || (i + 1 < sorted.length
                    && customContains(at((t + sorted[i + 1]) / 2), shape)))
                return t
        }
        return -1
    }

    function calculateSummary() {
        const frames = samples
        const id = targetId
        const config = settings
        if (!frames.length)
            return { label: qsTr("No run sampled yet"), event: null }
        let best = null
        let score = id === "velocity" ? -Infinity : Infinity
        if (id === "point-target" || id === "velocity"
            || id === "pose-target") {
            const target = id === "point-target" ? pointTarget()
                         : selectedPose.position
            const axis = direction()
            const weight = Number(config.rotationWeightPercent) / 100
            for (const frame of frames) {
                if (!insideWindow(frame.timeMs))
                    continue
                let value = 0
                if (id === "point-target") {
                    value = distance(frame.position, target)
                } else if (id === "velocity") {
                    const speed = Math.hypot(frame.velocity.x,
                                             frame.velocity.y,
                                             frame.velocity.z)
                    const alignment = speed > 1e-12
                            ? dot(frame.velocity, axis) / speed : 0
                    if ((config.mode === "projected"
                         || config.alignmentEnabled === "true")
                        && alignment < Number(config.minAlignmentPercent)
                                       / 100)
                        continue
                    value = config.mode === "projected"
                            ? dot(frame.velocity, axis) : speed
                } else {
                    const r = frame.rotation
                    const t = selectedPose.rotation
                    const quaternionDot = Math.abs(
                        r.x * t.x + r.y * t.y + r.z * t.z
                        + r.scalar * t.scalar)
                    const radians = 2 * Math.acos(Math.min(1, quaternionDot))
                    value = (1 - weight) * distance(frame.position, target)
                            + weight * radians
                }
                if (id === "velocity" ? value > score : value < score) {
                    score = value
                    best = frame
                }
            }
            if (!best)
                return { label: qsTr("No eligible sample"), event: null }
            const valueText = id === "velocity" ? controller.formatSpeed(score)
                : score.toFixed(2) + (id === "point-target" ? " m" : "")
            return {
                label: qsTr("Best: %1 at %2 ms")
                    .arg(valueText).arg(best.timeMs),
                event: best,
                score: score
            }
        }
        if (id === "stunt-points") {
            const at = sampleAt(Number(config.targetTimeMs))
            return {
                label: at && at.stuntsScore !== undefined
                    ? qsTr("Deadline: %1 points at %2 ms")
                        .arg(at.stuntsScore).arg(config.targetTimeMs)
                    : qsTr("Stunt score unavailable for this run"),
                event: at
            }
        }
        if (id === "precise-finish-time") {
            for (const frame of frames) {
                if (frame.raceCompleted || frame.finishTimeMs !== undefined) {
                    const time = frame.finishTimeMs ?? frame.timeMs
                    return { label: qsTr("Finish: %1 ms").arg(time),
                             event: sampleAt(time) }
                }
            }
            return { label: qsTr("Finish not reached"), event: null }
        }
        if (id === "volume-entry-time"
            || id === "custom-volume-entry-time") {
            const custom = id === "custom-volume-entry-time"
            const shape = custom ? selectedCustom
                                 : controller.cuboidTargets.selectedTarget
            const contains = position => custom
                ? customContains(position, shape)
                : cuboidEntry(position, position, shape) >= 0
            if (contains(frames[0].position))
                return { label: qsTr("Entry: %1").arg(controller.formatInterpolatedTime(frames[0].timeMs)),
                         event: frames[0] }
            for (let index = 1; index < frames.length; ++index) {
                const before = frames[index - 1]
                const after = frames[index]
                if (contains(before.position))
                    continue
                const fraction = custom
                    ? customEntry(before.position, after.position, shape)
                    : cuboidEntry(before.position, after.position, shape)
                if (fraction >= 0) {
                    const time = before.timeMs
                        + fraction * (after.timeMs - before.timeMs)
                    return { label: qsTr("Entry: %1")
                                        .arg(controller.formatInterpolatedTime(time)),
                             event: sampleAt(time) }
                }
            }
            return { label: qsTr("Target not entered"), event: null }
        }
        return { label: "", event: null }
    }

    function calculateCurrent() {
        const position = viewer.carPosition
        if (targetId === "point-target")
            return qsTr("Now: %1 m from target")
                .arg(distance(position, pointTarget()).toFixed(2))
        if (targetId === "velocity") {
            const velocity = viewer.carVelocity
            const speed = Math.hypot(velocity.x, velocity.y, velocity.z)
            const projected = dot(velocity, direction())
            const alignment = speed > 1e-12
                ? 100 * projected / speed : 0
            const active = (settings.mode === "projected"
                            || settings.alignmentEnabled === "true")
                           && alignment < Number(settings.minAlignmentPercent)
            const projectedMode = settings.mode === "projected"
            return qsTr("Now: %1%2 · %3% aligned%4")
                .arg(controller.formatSpeed(projectedMode ? projected : speed))
                .arg(projectedMode ? qsTr(" projected") : "").arg(alignment.toFixed(0))
                .arg(active ? qsTr(" · below threshold") : "")
        }
        if (targetId === "pose-target" && selectedPose.position) {
            const positionError = distance(position, selectedPose.position)
            const r = viewer.carRotation, t = selectedPose.rotation
            const quaternionDot = Math.abs(
                r.x * t.x + r.y * t.y + r.z * t.z
                + r.scalar * t.scalar)
            const angle = 2 * Math.acos(Math.min(1, quaternionDot))
            const weight = Number(settings.rotationWeightPercent) / 100
            const score = (1 - weight) * positionError + weight * angle
            return qsTr("Now: %1 m · %2° · score %3")
                .arg(positionError.toFixed(2))
                .arg((angle * 180 / Math.PI).toFixed(1))
                .arg(score.toFixed(2))
        }
        if (targetId === "stunt-points") {
            const at = sampleAt(viewer.timeMs)
            return at && at.stuntsScore !== undefined
                ? qsTr("Now: %1 points").arg(at.stuntsScore) : ""
        }
        return ""
    }

    function projected(position) {
        const cameraFrame = projectionFrame
        if (!cameraFrame)
            return null
        const point = projector.mapFrom3DScene(position)
        return point && point.z > 0 ? point : null
    }

    function markerPaths(position, radius, square) {
        if (!position)
            return []
        const point = projected(position)
        if (!point)
            return []
        if (square)
            return [[Qt.point(point.x - radius, point.y - radius),
                     Qt.point(point.x + radius, point.y - radius),
                     Qt.point(point.x + radius, point.y + radius),
                     Qt.point(point.x - radius, point.y + radius),
                     Qt.point(point.x - radius, point.y - radius)]]
        const circle = []
        for (let step = 0; step <= 24; ++step) {
            const angle = step * Math.PI / 12
            circle.push(Qt.point(point.x + radius * Math.cos(angle),
                                 point.y + radius * Math.sin(angle)))
        }
        return [circle]
    }

    function windowPaths(minimum, maximum) {
        if (!Number.isFinite(minimum) || !Number.isFinite(maximum)
            || maximum <= minimum)
            return []
        const paths = []
        let current = []
        const frames = screenSamples
        let low = 0
        let high = frames.length
        while (low < high) {
            const middle = Math.floor((low + high) / 2)
            if (frames[middle].timeMs < minimum)
                low = middle + 1
            else
                high = middle
        }
        for (let index = low; index < frames.length; ++index) {
            const frame = frames[index]
            if (frame.timeMs > maximum)
                break
            const point = frame.point
            if (!point) {
                if (current.length > 1)
                    paths.push(current)
                current = []
                continue
            }
            current.push(point)
        }
        if (current.length > 1)
            paths.push(current)
        return paths
    }

    function windowMarkerPaths(minimum, maximum, radius) {
        return markerPaths(sampleAt(minimum)?.position, radius, true)
            .concat(markerPaths(sampleAt(maximum)?.position, radius, true))
    }

    function linePaths(start, end) {
        if (!start || !end)
            return []
        const a = projected(start)
        const b = projected(end)
        return a && b ? [[Qt.point(a.x, a.y), Qt.point(b.x, b.y)]] : []
    }

    function calculateVelocityVisual() {
        const result = { axis: [], arrow: [], cone: [] }
        if (targetId !== "velocity")
            return result
        const start = viewer.carPosition
        const axis = direction()
        const length = Math.max(4, viewer.carVelocity.length())
        const end = Qt.vector3d(start.x + axis.x * length,
                                start.y + axis.y * length,
                                start.z + axis.z * length)
        const a = projected(start)
        const b = projected(end)
        if (!a || !b)
            return result
        result.axis = [[Qt.point(a.x, a.y), Qt.point(b.x, b.y)]]
        const angle = Math.atan2(b.y - a.y, b.x - a.x)
        result.arrow = [[Qt.point(b.x - 12 * Math.cos(angle - 0.45),
                                  b.y - 12 * Math.sin(angle - 0.45)),
                         Qt.point(b.x, b.y),
                         Qt.point(b.x - 12 * Math.cos(angle + 0.45),
                                  b.y - 12 * Math.sin(angle + 0.45))]]
        const threshold = Number(settings.minAlignmentPercent)
        if ((settings.mode !== "projected"
             && settings.alignmentEnabled !== "true")
            || threshold <= -100 || threshold >= 100)
            return result
        const halfAngle = Math.acos(threshold / 100)
        const perpendicularLength = Math.hypot(axis.z, axis.x)
        const perpendicular = perpendicularLength > 1e-6
            ? Qt.vector3d(-axis.z / perpendicularLength, 0,
                          axis.x / perpendicularLength)
            : Qt.vector3d(1, 0, 0)
        for (const side of [-1, 1]) {
            const edge = Qt.vector3d(
                start.x + (axis.x * Math.cos(halfAngle)
                           + side * perpendicular.x * Math.sin(halfAngle))
                          * length,
                start.y + axis.y * Math.cos(halfAngle) * length,
                start.z + (axis.z * Math.cos(halfAngle)
                           + side * perpendicular.z * Math.sin(halfAngle))
                          * length)
            result.cone = result.cone.concat(linePaths(start, edge))
        }
        return result
    }

    function conditionPlaneCorners() {
        const axis = condition.thresholdAxis
        if (!axis)
            return []
        const value = condition.thresholdValue
        const minimum = viewer.sceneBoundsMin
        const maximum = viewer.sceneBoundsMax
        const pad = 4
        let corners
        if (axis === 1) {
            corners = [Qt.vector3d(value, minimum.y - pad, minimum.z - pad),
                       Qt.vector3d(value, maximum.y + pad, minimum.z - pad),
                       Qt.vector3d(value, maximum.y + pad, maximum.z + pad),
                       Qt.vector3d(value, minimum.y - pad, maximum.z + pad)]
        } else if (axis === 2) {
            corners = [Qt.vector3d(minimum.x - pad, value, minimum.z - pad),
                       Qt.vector3d(maximum.x + pad, value, minimum.z - pad),
                       Qt.vector3d(maximum.x + pad, value, maximum.z + pad),
                       Qt.vector3d(minimum.x - pad, value, maximum.z + pad)]
        } else {
            corners = [Qt.vector3d(minimum.x - pad, minimum.y - pad, value),
                       Qt.vector3d(maximum.x + pad, minimum.y - pad, value),
                       Qt.vector3d(maximum.x + pad, maximum.y + pad, value),
                       Qt.vector3d(minimum.x - pad, maximum.y + pad, value)]
        }
        return corners
    }

    function conditionPlanePaths() {
        const path = []
        const corners = conditionPlaneCorners()
        for (const corner of corners) {
            const screen = projected(corner)
            if (screen)
                path.push(Qt.point(screen.x, screen.y))
        }
        if (path.length < 2)
            return []
        path.push(path[0])
        return [path]
    }

    function nearestSampleTime(position) {
        return nearestProjectedSample(position, -Infinity, Infinity).time
    }

    function windowRange(kind, index) {
        if (dragPreview && dragPreview.kind === kind
            && dragPreview.index === index)
            return [dragPreview.minimum, dragPreview.maximum]
        const settingsForRange = kind === "evaluation" ? settings
            : controller.modifierPasses[index]?.settings
        return settingsForRange
            ? [Number(settingsForRange.minTimeMs),
               Number(settingsForRange.maxTimeMs)] : [-1, -1]
    }

    function horizonTime() {
        return dragPreview?.kind === "horizon"
            ? dragPreview.time : Number(controller.simulationHorizonMs)
    }

    function stuntTime() {
        return dragPreview?.kind === "stunt"
            ? dragPreview.time : Number(settings.targetTimeMs)
    }

    function nearestProjectedSample(position, minimum, maximum) {
        let bestTime = -1
        let bestDistance = Infinity
        let previous = null
        for (const frame of screenSamples) {
            if (frame.timeMs < minimum || frame.timeMs > maximum
                || !frame.point) {
                previous = null
                continue
            }
            const point = frame.point
            let fraction = 0
            if (previous) {
                const dx = point.x - previous.point.x
                const dy = point.y - previous.point.y
                const lengthSquared = dx * dx + dy * dy
                if (lengthSquared > 0)
                    fraction = Math.max(0, Math.min(1,
                        ((position.x - previous.point.x) * dx
                         + (position.y - previous.point.y) * dy)
                        / lengthSquared))
            }
            const x = previous
                ? previous.point.x + (point.x - previous.point.x) * fraction
                : point.x
            const y = previous
                ? previous.point.y + (point.y - previous.point.y) * fraction
                : point.y
            const squared = (position.x - x) ** 2
                          + (position.y - y) ** 2
            if (squared < bestDistance) {
                bestDistance = squared
                bestTime = previous
                    ? previous.timeMs + (frame.timeMs - previous.timeMs)
                        * fraction : frame.timeMs
            }
            previous = frame
        }
        return { time: bestTime < 0 ? -1 : Math.round(bestTime / 10) * 10,
                 distance: Math.sqrt(bestDistance) }
    }

    function handleStyle(handle) {
        return handle.kind === "evaluation" ? windowStyle
             : handle.kind === "modifier"
               ? style("modifier:" + handle.index,
                       passColors[handle.index % passColors.length], 2)
             : handle.kind === "stunt" ? stuntStyle : horizonStyle
    }

    function canDragAt(layer, position, time) {
        if (layer.throughBlocks)
            return true
        const sample = sampleAt(time)
        if (!sample)
            return false
        const picked = projector.pick(position.x, position.y)
        if (!picked.objectHit)
            return true
        const camera = projector.camera.scenePosition
        return distance(camera, picked.scenePosition) + 0.1
               >= distance(camera, sample.position)
    }

    function targetDragLocked(hit) {
        return controller.targetMouseEditingLocked
            && (hit.kind === "evaluation" || hit.kind === "stunt")
    }

    function dragCandidates(position) {
        if (controller.running || !samples.length)
            return []
        if (nearestProjectedSample(position, -Infinity, Infinity).distance > 25)
            return []
        const handles = []
        for (const handle of windowHandles) {
            if (targetDragLocked(handle)) continue
            const layer = handleStyle(handle)
            if (!layer.visible)
                continue
            const sample = sampleAt(handle.time)
            const point = sample ? projected(sample.position) : null
            if (!point)
                continue
            const distance = Math.hypot(position.x - point.x,
                                        position.y - point.y)
            if (distance <= 12 && canDragAt(layer, position, handle.time))
                handles.push({ hit: handle, distance: distance })
        }
        if (handles.length) {
            handles.sort((a, b) => a.distance - b.distance)
            return handles.map(candidate => candidate.hit)
        }
        const evaluation = windowRange("evaluation", -1)
        const ranges = [{ kind: "evaluation", index: -1,
                          minimum: evaluation[0], maximum: evaluation[1],
                          style: windowStyle }]
        for (let index = 0; index < controller.modifierPasses.length; ++index) {
            if (controller.modifierPasses[index].enabled === false) continue
            const pass = windowRange("modifier", index)
            ranges.push({ kind: "modifier", index: index,
                          minimum: pass[0], maximum: pass[1],
                          style: style("modifier:" + index,
                              passColors[index % passColors.length], 2) })
        }
        const paths = []
        for (const range of ranges) {
            if (targetDragLocked(range)) continue
            if (!range.style.visible || !Number.isFinite(range.minimum)
                || !Number.isFinite(range.maximum)
                || range.maximum <= range.minimum)
                continue
            const nearest = nearestProjectedSample(
                position, range.minimum, range.maximum)
            const distance = nearest.distance
            if (nearest.time >= 0 && distance <= Math.max(9, range.style.width + 5)
                && canDragAt(range.style, position, nearest.time)) {
                paths.push({ distance: distance,
                             hit: { kind: range.kind, index: range.index,
                                    endpoint: "range", time: nearest.time,
                                    minimum: range.minimum,
                                    maximum: range.maximum } })
            }
        }
        paths.sort((a, b) => a.distance - b.distance)
        return paths.map(candidate => candidate.hit)
    }

    function dragHit(position, cycle) {
        const candidates = dragCandidates(position)
        return candidates.length
            ? candidates[(cycle ?? 0) % candidates.length] : null
    }

    function applyDrag(hit, time) {
        if (!hit || time < 0 || controller.running || targetDragLocked(hit))
            return
        time = Math.round(time / 10) * 10
        const start = Math.ceil(samples[0].timeMs / 10) * 10
        const end = Math.floor(samples[samples.length - 1].timeMs / 10) * 10
        if (hit.endpoint === "range") {
            const delta = Math.max(start - hit.minimum,
                Math.min(end - hit.maximum, time - hit.time))
            const minimum = Math.round((hit.minimum + delta) / 10) * 10
            const maximum = Math.round((hit.maximum + delta) / 10) * 10
            dragPreview = { kind: hit.kind, index: hit.index,
                            minimum: minimum, maximum: maximum }
            return
        }
        let next = Math.max(start, Math.min(end, time))
        if (hit.endpoint === "minTimeMs")
            next = Math.min(next, Math.floor((hit.other - 10) / 10) * 10)
        else if (hit.endpoint === "maxTimeMs")
            next = Math.max(next, Math.ceil((hit.other + 10) / 10) * 10)
        else if (hit.kind === "horizon")
            next = Math.max(10, next)
        if (hit.kind === "evaluation" || hit.kind === "modifier") {
            const range = windowRange(hit.kind, hit.index)
            dragPreview = { kind: hit.kind, index: hit.index,
                            minimum: hit.endpoint === "minTimeMs"
                                ? next : range[0],
                            maximum: hit.endpoint === "maxTimeMs"
                                ? next : range[1] }
        } else {
            dragPreview = { kind: hit.kind, index: hit.index, time: next }
        }
    }

    function commitDrag(hit) {
        const preview = dragPreview
        if (hit && (controller.running || targetDragLocked(hit))) {
            dragPreview = null
            return
        }
        if (!hit || !preview)
            return
        if (preview.kind === "horizon") {
            controller.simulationHorizonMs = String(preview.time)
        } else if (preview.kind === "stunt") {
            controller.setEvaluationTargetSetting("targetTimeMs",
                                                  String(preview.time))
        } else {
            const previous = hit.kind === "evaluation"
                ? [Number(settings.minTimeMs), Number(settings.maxTimeMs)]
                : [Number(controller.modifierPasses[hit.index]
                          .settings.minTimeMs),
                   Number(controller.modifierPasses[hit.index]
                          .settings.maxTimeMs)]
            const set = (key, value) => {
                if (hit.kind === "evaluation")
                    controller.setEvaluationTargetSetting(key, String(value))
                else
                    controller.setModifierPassSetting(hit.index, key,
                                                      String(value))
            }
            if (preview.minimum > previous[0]) {
                set("maxTimeMs", preview.maximum)
                set("minTimeMs", preview.minimum)
            } else {
                set("minTimeMs", preview.minimum)
                set("maxTimeMs", preview.maximum)
            }
        }
        dragPreview = null
    }

    function dragPreviewText(hit) {
        if (!hit || !dragPreview)
            return ""
        if (hit.kind === "horizon")
            return qsTr("Simulation horizon: %1 ms").arg(dragPreview.time)
        if (hit.kind === "stunt")
            return qsTr("Stunt deadline: %1 ms").arg(dragPreview.time)
        const title = hit.kind === "evaluation"
            ? qsTr("Evaluation window")
            : qsTr("Pass %1").arg(hit.index + 1)
        return qsTr("%1: %2 - %3 ms")
            .arg(title).arg(dragPreview.minimum).arg(dragPreview.maximum)
    }

    function dragHint(hit) {
        if (!hit)
            return ""
        if (hit.kind === "stunt")
            return qsTr("Drag stunt deadline")
        if (hit.kind === "horizon")
            return qsTr("Drag simulation horizon")
        const part = hit.endpoint === "range" ? qsTr("window")
                   : hit.endpoint === "minTimeMs" ? qsTr("start")
                                                    : qsTr("end")
        return hit.kind === "evaluation"
            ? qsTr("Drag evaluation %1").arg(part)
            : qsTr("Drag pass %1 %2").arg(hit.index + 1).arg(part)
    }

    Shape {
        objectName: "evaluationOverlayShapes"
        anchors.fill: parent
        visible: root.viewer.runCount > 0
        asynchronous: false

        ShapePath {
            strokeColor: root.stroke(root.windowStyle)
            strokeWidth: root.windowStyle.width
            strokeStyle: root.windowStyle.linePattern === "dash"
                ? ShapePath.DashLine : ShapePath.SolidLine
            fillColor: "transparent"
            PathMultiline {
                objectName: "evaluationWindowPath"
                paths: root.windowStyle.visible && root.windowStyle.throughBlocks
                    ? root.windowPaths(root.windowRange("evaluation", -1)[0],
                                       root.windowRange("evaluation", -1)[1]) : []
            }
        }
        ShapePath {
            strokeColor: root.stroke(root.windowStyle)
            strokeWidth: root.windowStyle.width
            fillColor: "transparent"
            PathMultiline {
                paths: root.windowStyle.visible && root.windowStyle.throughBlocks
                    ? root.windowMarkerPaths(
                        root.windowRange("evaluation", -1)[0],
                        root.windowRange("evaluation", -1)[1], 4) : []
            }
        }
        ShapePath {
            strokeColor: root.stroke(root.horizonStyle)
            strokeWidth: root.horizonStyle.width
            fillColor: "transparent"
            PathMultiline {
                paths: root.horizonStyle.visible
                       && root.horizonStyle.throughBlocks
                    ? root.markerPaths(root.sampleAt(
                    root.horizonTime())?.position,
                    5, true) : []
            }
        }
        ShapePath {
            strokeColor: root.stroke(root.eventStyle)
            strokeWidth: root.eventStyle.width
            fillColor: "transparent"
            PathMultiline {
                paths: root.eventStyle.visible
                       && root.eventStyle.throughBlocks
                    ? root.markerPaths(root.targetId === "stunt-points"
                                       ? root.sampleAt(root.stuntTime())?.position
                                       : root.summary.event?.position,
                                       7, false) : []
            }
        }
        ShapePath {
            strokeColor: root.stroke(root.pointStyle)
            strokeWidth: root.pointStyle.width
            fillColor: "transparent"
            PathMultiline {
                paths: root.pointStyle.visible
                    && root.pointStyle.throughBlocks
                    && root.targetId === "point-target"
                    ? root.markerPaths(root.pointTarget(), 9, false) : []
            }
        }
        ShapePath {
            strokeColor: root.stroke(root.pointStyle)
            strokeWidth: root.pointStyle.width
            strokeStyle: ShapePath.DashLine
            dashPattern: [5, 4]
            fillColor: "transparent"
            PathMultiline {
                paths: root.pointStyle.visible
                    && root.pointStyle.throughBlocks
                    && root.targetId === "point-target"
                    ? root.linePaths(root.summary.event?.position,
                                     root.pointTarget()) : []
            }
        }
        ShapePath {
            strokeColor: root.stroke(root.velocityStyle)
            strokeWidth: root.velocityStyle.width
            fillColor: "transparent"
            PathMultiline { paths: root.velocityStyle.visible
                                   && root.velocityStyle.throughBlocks
                                   ? root.velocityVisual.axis : [] }
        }
        ShapePath {
            strokeColor: root.stroke(root.velocityStyle)
            strokeWidth: root.velocityStyle.width
            fillColor: "transparent"
            PathMultiline { paths: root.velocityStyle.visible
                                   && root.velocityStyle.throughBlocks
                                   ? root.velocityVisual.arrow : [] }
        }
        ShapePath {
            strokeColor: root.stroke(root.velocityStyle)
            strokeWidth: root.velocityStyle.width
            strokeStyle: ShapePath.DashLine
            dashPattern: [4, 4]
            fillColor: "transparent"
            PathMultiline { paths: root.velocityStyle.visible
                                   && root.velocityStyle.throughBlocks
                                   ? root.velocityVisual.cone : [] }
        }
        ShapePath {
            strokeColor: root.stroke(root.conditionStyle)
            strokeWidth: root.conditionStyle.width
            strokeStyle: root.conditionStyle.linePattern === "dash"
                ? ShapePath.DashLine : ShapePath.SolidLine
            dashPattern: [6, 5]
            fillColor: "transparent"
            PathMultiline { paths: root.conditionStyle.visible
                                   && root.conditionStyle.throughBlocks
                                   ? root.conditionPlanePaths() : [] }
        }
    }

    Repeater {
        model: root.controller.modifierPasses

        delegate: Shape {
            id: passShape
            required property int index
            required property var modelData
            readonly property color passColor:
                root.passColors[index % root.passColors.length]
            readonly property var layerStyle:
                root.style("modifier:" + index, passColor, 2)
            readonly property var passSettings:
                root.windowRange("modifier", index)

            anchors.fill: parent
            visible: root.viewer.runCount > 0
                     && modelData.enabled !== false
                     && layerStyle.visible && layerStyle.throughBlocks
            asynchronous: false

            ShapePath {
                strokeColor: root.stroke(passShape.layerStyle)
                strokeWidth: passShape.layerStyle.width
                strokeStyle: passShape.layerStyle.linePattern === "dash"
                    ? ShapePath.DashLine : ShapePath.SolidLine
                fillColor: "transparent"
                PathMultiline {
                    paths: root.windowPaths(
                        passShape.passSettings[0],
                        passShape.passSettings[1])
                }
            }
            ShapePath {
                strokeColor: root.stroke(passShape.layerStyle)
                strokeWidth: passShape.layerStyle.width
                fillColor: "transparent"
                PathMultiline {
                    paths: root.windowMarkerPaths(
                        passShape.passSettings[0],
                        passShape.passSettings[1], 3)
                }
            }
        }
    }

    MouseArea {
        id: worldDragArea
        objectName: "evaluationWorldDragArea"
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        property var activeHit: null
        property var hoveredHit: null
        property point lastPressPosition: Qt.point(-100, -100)
        property real lastPressTime: 0
        property int overlapIndex: 0
        property bool movedDuringDrag: false
        property real lastDragTime: -1
        cursorShape: activeHit || hoveredHit ? Qt.OpenHandCursor
                                             : Qt.ArrowCursor
        onPressed: mouse => {
            const position = Qt.point(mouse.x, mouse.y)
            const candidates = root.dragCandidates(position)
            const now = Date.now()
            if (now - lastPressTime < 800
                && Math.hypot(position.x - lastPressPosition.x,
                              position.y - lastPressPosition.y) < 12)
                overlapIndex = (overlapIndex + 1) % Math.max(1, candidates.length)
            else
                overlapIndex = 0
            activeHit = candidates[overlapIndex] ?? null
            lastDragTime = activeHit ? activeHit.time : -1
            lastPressPosition = position
            lastPressTime = now
            movedDuringDrag = false
            if (!activeHit)
                mouse.accepted = false
        }
        onPositionChanged: mouse => {
            const position = Qt.point(mouse.x, mouse.y)
            if (pressed && activeHit) {
                if (Math.hypot(position.x - lastPressPosition.x,
                               position.y - lastPressPosition.y) > 3)
                    movedDuringDrag = true
                if (movedDuringDrag) {
                    const global = root.nearestProjectedSample(
                        position, -Infinity, Infinity)
                    const nearby = root.nearestProjectedSample(
                        position, lastDragTime - 1000,
                        lastDragTime + 1000)
                    const time = nearby.time >= 0
                        && nearby.distance <= global.distance + 6
                        ? nearby.time : global.time
                    if (time >= 0) {
                        lastDragTime = time
                        root.applyDrag(activeHit, time)
                    }
                }
            } else {
                hoveredHit = root.dragHit(position)
            }
        }
        onReleased: {
            root.commitDrag(activeHit)
            if (movedDuringDrag)
                lastPressTime = 0
            activeHit = null
        }
        onCanceled: {
            root.dragPreview = null
            lastPressTime = 0
            activeHit = null
        }
        onExited: hoveredHit = null
        ToolTip.visible: worldDragArea.containsMouse
                         && (activeHit || hoveredHit)
        ToolTip.delay: activeHit ? 0 : 500
        ToolTip.text: activeHit && root.dragPreview
            ? root.dragPreviewText(activeHit)
            : root.dragHint(activeHit ?? hoveredHit)
    }

}
