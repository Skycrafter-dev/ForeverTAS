import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

// How much each axis of a car pose counts, on a six-spoke chart: position
// X, Y, Z on the upper half, yaw, pitch, roll on the lower half. Drag a
// point to set one axis; drag anywhere else to push every weight towards
// that side at once (up favours position, down rotation, and so on).
ColumnLayout {
    id: root

    property var settings: ({})
    property bool running: false
    signal edited(string key, string value)

    readonly property var keys: ["weightX", "weightY", "weightZ",
                                 "weightYaw", "weightPitch", "weightRoll"]
    readonly property var labels: [qsTr("X"), qsTr("Y"), qsTr("Z"),
                                   qsTr("Yaw"), qsTr("Pitch"), qsTr("Roll")]
    // Spoke directions, screen space (y down): position above, rotation below.
    readonly property var angles: [-150, -90, -30, 30, 90, 150]

    // The weights shown, 0-100: the settings, or a drag in progress.
    property var draft: null
    readonly property var weights: draft ?? keys.map(key => {
        const value = Number(settings[key])
        return Number.isFinite(value) ? Math.max(0, Math.min(100, value)) : 100
    })

    function commit(values) {
        for (let i = 0; i < keys.length; ++i) {
            const rounded = String(Math.round(values[i]))
            if (rounded !== String(settings[keys[i]]))
                edited(keys[i], rounded)
        }
    }

    Layout.fillWidth: true
    spacing: 6

    Label {
        text: qsTr("Axis weights")
        color: ThemeControls.AppTheme.textMuted
        font.pixelSize: 11
    }

    Item {
        id: chart
        objectName: "poseWeightsChart"
        Layout.fillWidth: true
        Layout.preferredHeight: 190
        enabled: !root.running
        opacity: enabled ? 1 : 0.5

        readonly property real radius: Math.min(width, height) / 2 - 28
        readonly property point center: Qt.point(width / 2, height / 2)

        function direction(index) {
            const radians = root.angles[index] * Math.PI / 180
            return Qt.point(Math.cos(radians), Math.sin(radians))
        }
        function vertex(index, weight) {
            const d = direction(index)
            const r = radius * weight / 100
            return Qt.point(center.x + d.x * r, center.y + d.y * r)
        }

        Canvas {
            id: canvas
            anchors.fill: parent
            renderStrategy: Canvas.Cooperative
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const c = chart.center
                // Rings at a quarter, half, three quarters and all.
                ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.10)
                ctx.lineWidth = 1
                for (const fraction of [0.25, 0.5, 0.75, 1]) {
                    ctx.beginPath()
                    for (let i = 0; i <= 6; ++i) {
                        const p = chart.vertex(i % 6, fraction * 100)
                        if (i === 0) ctx.moveTo(p.x, p.y)
                        else ctx.lineTo(p.x, p.y)
                    }
                    ctx.stroke()
                }
                for (let i = 0; i < 6; ++i) {
                    const p = chart.vertex(i, 100)
                    ctx.beginPath()
                    ctx.moveTo(c.x, c.y)
                    ctx.lineTo(p.x, p.y)
                    ctx.stroke()
                }
                // The weights.
                const accent = ThemeControls.AppTheme.accent
                ctx.fillStyle = Qt.rgba(accent.r, accent.g, accent.b, 0.22)
                ctx.strokeStyle = accent
                ctx.lineWidth = 2
                ctx.beginPath()
                for (let i = 0; i <= 6; ++i) {
                    const p = chart.vertex(i % 6, root.weights[i % 6])
                    if (i === 0) ctx.moveTo(p.x, p.y)
                    else ctx.lineTo(p.x, p.y)
                }
                ctx.fill()
                ctx.stroke()
                for (let i = 0; i < 6; ++i) {
                    const p = chart.vertex(i, root.weights[i])
                    ctx.beginPath()
                    ctx.fillStyle = i < 3 ? accent : ThemeControls.AppTheme.info
                    ctx.arc(p.x, p.y, mouseArea.axis === i ? 6 : 4.5, 0, 2 * Math.PI)
                    ctx.fill()
                }
            }
            Connections {
                target: root
                function onWeightsChanged() { canvas.requestPaint() }
            }
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
        }

        Repeater {
            model: 6
            Label {
                required property int index
                readonly property point at: chart.vertex(index, 100)
                readonly property point outward: chart.direction(index)
                x: at.x + outward.x * 18 - width / 2
                y: at.y + outward.y * 14 - height / 2
                text: root.labels[index] + " " + Math.round(root.weights[index])
                color: index < 3 ? ThemeControls.AppTheme.text
                                 : ThemeControls.AppTheme.info
                font.pixelSize: 11
            }
        }

        MouseArea {
            id: mouseArea
            anchors.fill: parent
            preventStealing: true
            cursorShape: axis >= 0 ? Qt.SizeAllCursor : Qt.OpenHandCursor
            // The axis being dragged, or -1 while pushing them all.
            property int axis: -1
            property point start
            property var startWeights: []

            onPressed: mouse => {
                start = Qt.point(mouse.x, mouse.y)
                startWeights = root.weights.slice()
                axis = -1
                let nearest = 14
                for (let i = 0; i < 6; ++i) {
                    const p = chart.vertex(i, root.weights[i])
                    const distance = Math.hypot(p.x - mouse.x, p.y - mouse.y)
                    if (distance < nearest) {
                        nearest = distance
                        axis = i
                    }
                }
                root.draft = startWeights.slice()
            }
            onPositionChanged: mouse => {
                if (!pressed)
                    return
                const next = startWeights.slice()
                if (axis >= 0) {
                    const d = chart.direction(axis)
                    const along = (mouse.x - chart.center.x) * d.x
                                  + (mouse.y - chart.center.y) * d.y
                    next[axis] = Math.max(0, Math.min(100, 100 * along / chart.radius))
                } else {
                    const dx = mouse.x - start.x
                    const dy = mouse.y - start.y
                    for (let i = 0; i < 6; ++i) {
                        const d = chart.direction(i)
                        next[i] = Math.max(0, Math.min(100,
                            startWeights[i] + 100 * (dx * d.x + dy * d.y) / chart.radius))
                    }
                }
                root.draft = next
            }
            onReleased: {
                if (root.draft)
                    root.commit(root.draft)
                root.draft = null
                axis = -1
            }
            onCanceled: {
                root.draft = null
                axis = -1
            }
        }
    }

    Flow {
        Layout.fillWidth: true
        spacing: 6
        Repeater {
            model: [
                { label: qsTr("Equal"), values: [100, 100, 100, 100, 100, 100] },
                { label: qsTr("Position"), values: [100, 100, 100, 0, 0, 0] },
                { label: qsTr("Rotation"), values: [0, 0, 0, 100, 100, 100] },
                { label: qsTr("Ground"), values: [100, 0, 100, 100, 0, 0] },
                { label: qsTr("Height"), values: [0, 100, 0, 0, 0, 0] }
            ]
            delegate: ThemeControls.ThemedButton {
                required property var modelData
                text: modelData.label
                enabled: !root.running
                onClicked: root.commit(modelData.values)
            }
        }
    }
}
