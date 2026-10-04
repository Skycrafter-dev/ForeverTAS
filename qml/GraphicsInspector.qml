import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import "settings"

Rectangle {
    id: root

    property var viewer
    property var controller
    property var editHistory
    property string renderMode: "textured"
    property bool rayTracingSupported: false
    property string rayTracingStatus: ""
    readonly property var carPalette:
        ["#ff8a3d", "#3d8dff", "#63c77b", "#c57aeb",
         "#e7c24f", "#54c7c1", "#f56f70", "#7186f1",
         "#a8ca4d", "#e881bb", "#4eb1e8", "#b7a45b",
         "#d95f57", "#936ddb", "#43a98b", "#cf6daf"]
    property int currentTab: 1
    property string selectedLayerId: ""
    property var collapsedGroups: ({})
    property var rows: []
    property var windowRows: []
    property string cuboidRowSignature: ""
    property string customRowSignature: ""
    property string poseRowSignature: ""
    readonly property var rowInputs: !viewer || !controller ? [] : [
        viewer.runOptions, viewer.trajectoryPaths, viewer.selectedRunId,
        controller.evaluationTargetId, controller.modifierPasses, controller.simulationHorizonMs,
        controller.drawTargetsThroughBlocks
    ]
    readonly property var visibleRows: {
        const result = []
        let collapsed = false
        for (const row of rows) {
            if (row.group) {
                collapsed = collapsedGroups[row.name] ?? false
                result.push(row)
            } else if (!collapsed) {
                result.push(row)
            }
        }
        return result
    }
    readonly property var selectedLayer: {
        const requested = rows.find(row => !row.group
                                    && row.id === selectedLayerId)
        return requested ?? rows.find(row => !row.group) ?? null
    }
    readonly property var selectedStyle: styleFor(selectedLayer)

    signal renderModeRequested(string mode)

    objectName: "graphicsInspector"
    color: AppTheme.panel

    // Rebuild once per event-loop turn, not for every intermediate target edit.
    onRowInputsChanged: Qt.callLater(refreshRows)
    onCurrentTabChanged: Qt.callLater(refreshRows)
    Component.onCompleted: Qt.callLater(refreshRows)

    function refreshTargetRows(kind) {
        const targets = kind === "cuboid" ? controller.cuboidTargets.targets
            : kind === "custom" ? controller.customVolumeTargets.targets
            : controller.poseTargets.targets
        const signature = targets.map(target => target.id + ":" + target.name)
                                 .join("|")
        const property = kind + "RowSignature"
        if (root[property] !== signature) {
            root[property] = signature
            Qt.callLater(refreshRows)
        }
    }

    Connections {
        target: root.controller ? root.controller.cuboidTargets : null
        function onTargetsChanged() { root.refreshTargetRows("cuboid") }
    }
    Connections {
        target: root.controller ? root.controller.customVolumeTargets : null
        function onTargetsChanged() { root.refreshTargetRows("custom") }
    }
    Connections {
        target: root.controller ? root.controller.poseTargets : null
        function onTargetsChanged() { root.refreshTargetRows("pose") }
    }

    function refreshRows() {
        const nextWindows = buildRows(1)
        if (!sameRowStructure(windowRows, nextWindows))
            windowRows = nextWindows
        const nextRows = currentTab === 1 ? windowRows : buildRows(currentTab)
        if (!sameRowStructure(rows, nextRows))
            rows = nextRows
    }

    function sameRowStructure(left, right) {
        if (left.length !== right.length)
            return false
        for (let index = 0; index < left.length; ++index) {
            const a = left[index]
            const b = right[index]
            if (a.group !== b.group || a.name !== b.name || a.id !== b.id
                || a.kind !== b.kind || a.color !== b.color
                || a.width !== b.width || a.opacity !== b.opacity
                || a.through !== b.through || a.runId !== b.runId)
                return false
        }
        return true
    }

    function syncRenderMode() {
        renderPicker.currentIndex = Math.max(
            0, renderPicker.indexOfValue(renderMode))
    }
    onRenderModeChanged: syncRenderMode()

    function toggleGroup(name) {
        const next = Object.assign({}, collapsedGroups)
        next[name] = !(next[name] ?? false)
        collapsedGroups = next
    }

    function makeLayer(id, name, kind, color, width, through,
                       minimum, maximum, runId, defaultOpacity) {
        return { id: id, name: name, kind: kind, color: color,
                 width: width ?? 2,
                 opacity: defaultOpacity
                     ?? (id === "evaluation:window" ? 0.55 : 1),
                 through: through ?? false,
                 minimum: minimum ?? -1, maximum: maximum ?? -1,
                 runId: runId ?? "" }
    }

    function makeGroup(name) { return { group: true, name: name } }

    function buildRows(tab) {
        if (!viewer || !controller)
            return []
        const result = []
        const runOptions = viewer.runOptions
        const selectedRun = viewer.selectedRunId
        if (tab === 0) {
            result.push(makeGroup(qsTr("Cars")))
            for (let index = 0; index < runOptions.length; ++index) {
                const run = runOptions[index]
                result.push(makeLayer("car:" + run.id, run.name + qsTr(" car"),
                                  "car", carPalette[index % carPalette.length],
                                  2, false))
            }
            result.push(makeGroup(qsTr("Trajectories")))
            const paths = viewer.trajectoryPaths
            for (let index = 0; index < paths.length; ++index) {
                const path = paths[index]
                const id = path.visualId ?? ("trajectory:" +
                    (path.runId ?? "improvement:" + index))
                result.push(makeLayer(id, path.name, "trajectory",
                                  path.color, 3, false,
                                  -1, -1, "", path.opacity))
            }
        } else if (tab === 1) {
            result.push(makeGroup(qsTr("Evaluation")))
            const evaluation = controller.evaluationTargetSettings
            result.push(makeLayer("evaluation:window", qsTr("Evaluation window"),
                              "window", "#80e0b4", 4, true,
                              Number(evaluation.minTimeMs),
                              evaluation.maxTimeMode === "horizon"
                                  ? Number(controller.simulationHorizonMs)
                                  : Number(evaluation.maxTimeMs), selectedRun))
            const targetId = controller.evaluationTargetId
            const eventId = targetId === "precise-finish-time"
                ? "target:finish"
                : targetId === "stunt-points"
                  ? "evaluation:stunt-deadline" : "evaluation:event"
            const eventName = targetId === "precise-finish-time"
                ? qsTr("Finish event")
                : targetId === "stunt-points"
                  ? qsTr("Stunt event") : qsTr("Evaluation event")
            result.push(makeLayer(eventId, eventName,
                                  "marker", "#fff276", 2, true))
            result.push(makeGroup(qsTr("Modifiers")))
            const passes = controller.modifierPasses
            const colors = ["#b782df", "#f09d6c", "#69bde7", "#cf7db2",
                            "#8bc46f", "#e4c15d", "#8e9fe5", "#da7b78",
                            "#60c6b2", "#c793e6", "#d7ad70", "#9cc96a"]
            for (let index = 0; index < passes.length; ++index) {
                const pass = passes[index]
                if (pass.enabled === false) continue
                result.push(makeLayer("modifier:" + index,
                                  qsTr("Pass %1").arg(index + 1), "window",
                                  colors[index % colors.length], 2, true,
                                  Number(pass.settings.minTimeMs),
                                  pass.settings.maxTimeMode === "horizon"
                                      ? Math.max(0, Number(controller.simulationHorizonMs) - viewer.tickDurationMs)
                                      : Number(pass.settings.maxTimeMs), selectedRun))
            }
            result.push(makeGroup(qsTr("Simulation")))
            result.push(makeLayer("simulation:horizon",
                              qsTr("Simulation horizon"), "marker",
                              "#e8b959", 2, true))
            result.push(makeLayer("evaluation:stunt-deadline",
                              qsTr("Stunt deadline"), "marker",
                              "#fff276", 2, true))
            result.push(makeGroup(qsTr("Conditions")))
            result.push(makeLayer("condition:threshold",
                              qsTr("Condition threshold"), "condition",
                              "#e47a77", 1.5, true))
        } else {
            result.push(makeGroup(qsTr("Volumes")))
            const cuboids = controller.cuboidTargets.targets
            for (let index = 0; index < cuboids.length; ++index)
                result.push(makeLayer("target:cuboid:" + cuboids[index].id,
                                  cuboids[index].name,
                                  "target", "#55a7d8", 2,
                                  controller.drawTargetsThroughBlocks,
                                  -1, -1, "", 0.13))
            const customTargets = controller.customVolumeTargets.targets
            for (let index = 0; index < customTargets.length; ++index)
                result.push(makeLayer("target:custom:" + customTargets[index].id,
                                  customTargets[index].name
                                  ?? qsTr("Custom volume %1").arg(index + 1),
                                  "target", "#ca7ccc", 2,
                                  controller.drawTargetsThroughBlocks,
                                  -1, -1, "", 0.14))
            result.push(makeGroup(qsTr("Pose cars")))
            const poses = controller.poseTargets.targets
            for (let index = 0; index < poses.length; ++index)
                result.push(makeLayer("target:pose:" + poses[index].id,
                                  poses[index].name
                                  ?? qsTr("Pose car %1").arg(index + 1),
                                  "target", "#65a7d8", 2,
                                  controller.drawTargetsThroughBlocks,
                                  -1, -1, "", 0.36))
            result.push(makeGroup(qsTr("Other targets")))
            result.push(makeLayer("target:point", qsTr("Point target"),
                              "target", "#f6a45a", 2, true))
            result.push(makeLayer("target:velocity", qsTr("Velocity direction"),
                              "target", "#65d0e2", 3, true))
            result.push(makeLayer("target:finish", qsTr("Finish event"),
                              "target", "#fff276", 2, true))
        }
        return result
    }

    function styleFor(row) {
        if (!row || row.group || !viewer)
            return ({ visible: false, throughBlocks: false,
                      color: "#ffffff", width: 2, opacity: 1,
                      linePattern: "solid" })
        const revision = viewer.visualStyleRevision
        const override = viewer.visualStyle(row.id)
        return { visible: override.visible ?? true,
                 throughBlocks: override.throughBlocks ?? row.through,
                 color: override.color ?? row.color,
                 width: override.width ?? row.width,
                 opacity: override.opacity ?? row.opacity,
                 linePattern: override.linePattern
                     ?? (row.kind === "condition" ? "dash" : "solid") }
    }

    function change(id, property, value) {
        if (id)
            viewer.setVisualStyle(id, property, value)
    }

    function editColor(id, color) {
        colorDialog.layerId = id
        colorDialog.selectedColor = color
        colorDialog.open()
    }

    ColorDialog {
        id: colorDialog
        property string layerId: ""
        title: qsTr("Layer color")
        onAccepted: root.change(layerId, "color", selectedColor.toString())
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 12
            Layout.topMargin: 12
            Layout.bottomMargin: 9
            Label {
                Layout.fillWidth: true
                text: qsTr("Graphics")
                color: AppTheme.text
                font.pixelSize: 17
                font.weight: Font.DemiBold
            }
            ThemedIconButton {
                objectName: "clearPreviewTrajectoriesButton"
                icon.source: "qrc:/icons/trash-2.svg"
                enabled: root.viewer && root.viewer.trajectoryPaths.some(
                    path => path.kind === "improvement")
                Accessible.name: qsTr("Clear preview trajectories")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: root.viewer.clearPreviewTrajectories()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.bottomMargin: 10
            spacing: 8

            Label {
                text: qsTr("Render")
                color: AppTheme.textMuted
                font.pixelSize: 11
            }
            StyledComboBox {
                id: renderPicker
                objectName: "renderModeSelector"
                Layout.fillWidth: true
                enabled: root.viewer && root.viewer.loaded
                model: root.rayTracingSupported
                    ? [{ text: qsTr("Textured"), value: "textured" },
                       { text: qsTr("Textured (RT)"), value: "textured-rt" },
                       { text: qsTr("Neutral"), value: "neutral" },
                       { text: qsTr("Collision"), value: "collision" },
                       { text: qsTr("Wireframe"), value: "wireframe" },
                       { text: qsTr("High Contrast"),
                         value: "material-debug" }]
                    : [{ text: qsTr("Textured"), value: "textured" },
                       { text: qsTr("Neutral"), value: "neutral" },
                       { text: qsTr("Collision"), value: "collision" },
                       { text: qsTr("Wireframe"), value: "wireframe" },
                       { text: qsTr("High Contrast"),
                         value: "material-debug" }]
                textRole: "text"
                valueRole: "value"
                Component.onCompleted: root.syncRenderMode()
                onActivated: root.renderModeRequested(currentValue)
                ToolTip.visible: hovered && currentValue === "textured-rt"
                ToolTip.text: root.rayTracingStatus
            }
        }

        ThemedCheckBox {
            objectName: "targetMouseEditingLock"
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            text: qsTr("Lock target dragging")
            checked: root.controller && root.controller.targetMouseEditingLocked
            onToggled: root.controller.targetMouseEditingLocked = checked
        }

        TabBar {
            Layout.fillWidth: true
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            Layout.bottomMargin: 8
            currentIndex: root.currentTab
            onCurrentIndexChanged: root.currentTab = currentIndex

            ThemedTabButton { text: qsTr("Runs") }
            ThemedTabButton { text: qsTr("Overlays") }
            ThemedTabButton { text: qsTr("Targets") }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.bottomMargin: 4
            spacing: 4
            Label {
                Layout.fillWidth: true
                text: qsTr("Name")
                color: AppTheme.textMuted
                font.pixelSize: 10
            }
            Label { text: qsTr("Vis"); color: AppTheme.textMuted;
                    font.pixelSize: 10 }
            Label { text: qsTr("Color"); color: AppTheme.textMuted;
                    font.pixelSize: 10 }
            Label { text: qsTr("X-ray"); color: AppTheme.textMuted;
                    font.pixelSize: 10 }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: AppTheme.border
        }

        ScrollView {
            id: layerScroll
            objectName: "graphicsLayerList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 88
            Layout.preferredHeight: root.height * 0.30
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            Column {
                width: layerScroll.availableWidth

                Repeater {
                    model: root.visibleRows

                    delegate: Item {
                        id: rowItem
                        required property var modelData
                        readonly property var rowStyle:
                            root.styleFor(modelData)
                        width: parent.width
                        height: modelData.group ? 27 : 33

                        Rectangle {
                            anchors.fill: parent
                            visible: !rowItem.modelData.group
                            color: root.selectedLayer
                                   && root.selectedLayer.id ===
                                      rowItem.modelData.id
                                   ? AppTheme.selection : "transparent"
                        }
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: AppTheme.border
                            opacity: 0.4
                        }
                        Label {
                            anchors.left: parent.left
                            anchors.leftMargin: rowItem.modelData.group
                                ? 32 : 20
                            anchors.right: rowControls.left
                            anchors.rightMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            text: rowItem.modelData.name
                            color: rowItem.modelData.group
                                   ? AppTheme.text : AppTheme.textMuted
                            font.pixelSize: rowItem.modelData.group ? 11 : 10
                            font.weight: rowItem.modelData.group
                                         ? Font.DemiBold : Font.Normal
                            elide: Text.ElideRight
                        }
                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            ToolTip.visible: containsMouse
                                             && !rowItem.modelData.group
                            ToolTip.delay: 600
                            ToolTip.text: rowItem.modelData.name
                            onClicked: {
                                if (rowItem.modelData.group)
                                    root.toggleGroup(rowItem.modelData.name)
                                else
                                    root.selectedLayerId = rowItem.modelData.id
                            }
                        }
                        ThemedIconButton {
                            anchors.left: parent.left
                            anchors.leftMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            visible: rowItem.modelData.group === true
                            width: 22
                            height: 22
                            icon.source: root.collapsedGroups[
                                rowItem.modelData.name]
                                ? "qrc:/icons/chevron-right.svg"
                                : "qrc:/icons/chevron-down.svg"
                            icon.width: 13
                            icon.height: 13
                            background: Item {}
                            ToolTip.visible: hovered
                            ToolTip.text: root.collapsedGroups[
                                rowItem.modelData.name]
                                ? qsTr("Expand group") : qsTr("Collapse group")
                            onClicked: root.toggleGroup(rowItem.modelData.name)
                        }
                        Row {
                            id: rowControls
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            visible: !rowItem.modelData.group

                            ThemedIconButton {
                                width: 26
                                height: 26
                                icon.source: rowItem.rowStyle.visible
                                    ? "qrc:/icons/eye.svg"
                                    : "qrc:/icons/eye-off.svg"
                                icon.width: 14
                                icon.height: 14
                                ToolTip.visible: hovered
                                ToolTip.text: rowItem.rowStyle.visible
                                    ? qsTr("Hide layer")
                                    : qsTr("Show layer")
                                Accessible.name: ToolTip.text
                                onClicked: root.change(rowItem.modelData.id,
                                                       "visible",
                                                       !rowItem.rowStyle.visible)
                            }
                            ThemedButton {
                                width: 26
                                height: 26
                                padding: 5
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Choose layer color")
                                Accessible.name: ToolTip.text
                                onClicked: root.editColor(
                                    rowItem.modelData.id,
                                    rowItem.rowStyle.color)
                                background: Rectangle {
                                    radius: 4
                                    color: AppTheme.control
                                    border.width: 1
                                    border.color: AppTheme.borderStrong
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 6
                                        radius: 2
                                        color: rowItem.rowStyle.color
                                    }
                                }
                            }
                            ThemedIconButton {
                                width: 26
                                height: 26
                                selected: rowItem.rowStyle.throughBlocks
                                icon.source: "qrc:/icons/box.svg"
                                icon.width: 14
                                icon.height: 14
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Draw through blocks")
                                Accessible.name: ToolTip.text
                                onClicked: root.change(
                                    rowItem.modelData.id, "throughBlocks",
                                    !rowItem.rowStyle.throughBlocks)
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: AppTheme.border
        }

        ScrollView {
            id: layerEditorScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 120
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            visible: root.selectedLayer !== null

        ColumnLayout {
            objectName: "graphicsLayerEditor"
            x: 12
            width: layerEditorScroll.availableWidth - 24
            spacing: 5

            Label {
                Layout.fillWidth: true
                text: root.selectedLayer ? root.selectedLayer.name : ""
                color: AppTheme.text
                font.pixelSize: 13
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }
            ThemedSwitch {
                text: qsTr("Visible")
                checked: root.selectedStyle.visible
                onToggled: root.change(root.selectedLayer.id,
                                       "visible", checked)
            }
            ThemedSwitch {
                text: qsTr("Draw through blocks")
                checked: root.selectedStyle.throughBlocks
                onToggled: root.change(root.selectedLayer.id,
                                       "throughBlocks", checked)
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Color"); color: AppTheme.textMuted;
                        Layout.fillWidth: true }
                ThemedButton {
                    Layout.preferredWidth: 66
                    Layout.preferredHeight: 28
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Choose layer color")
                    onClicked: root.editColor(root.selectedLayer.id,
                                              root.selectedStyle.color)
                    background: Rectangle {
                        color: AppTheme.control
                        border.width: 1
                        border.color: AppTheme.borderStrong
                        radius: 4
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 6
                            color: root.selectedStyle.color
                            radius: 2
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                visible: root.selectedLayer
                         && root.selectedLayer.kind !== "car"
                         && root.selectedLayer.kind !== "target"
                Label { text: qsTr("Width"); color: AppTheme.textMuted;
                        Layout.preferredWidth: 52 }
                ThemedSlider {
                    Layout.fillWidth: true
                    from: 0.5
                    to: 20
                    value: root.selectedStyle.width
                    onPressedChanged: {
                        if (!root.editHistory) return
                        if (pressed) root.editHistory.beginGesture()
                        else root.editHistory.endGesture()
                    }
                    onMoved: root.change(root.selectedLayer.id,
                                         "width", value)
                }
                SliderValueField {
                    objectName: "graphicsWidthField"
                    Layout.preferredWidth: 68
                    value: String(root.selectedStyle.width)
                    from: 0.5
                    to: 20
                    displayDecimals: 2
                    horizontalAlignment: TextInput.AlignLeft
                    suffix: "px"
                    accessibleName: qsTr("Layer width")
                    onEdited: value => root.change(root.selectedLayer.id,
                                                    "width", Number(value))
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Opacity"); color: AppTheme.textMuted;
                        Layout.preferredWidth: 52 }
                ThemedSlider {
                    Layout.fillWidth: true
                    from: 0
                    to: 100
                    value: root.selectedStyle.opacity * 100
                    onPressedChanged: {
                        if (!root.editHistory) return
                        if (pressed) root.editHistory.beginGesture()
                        else root.editHistory.endGesture()
                    }
                    onMoved: root.change(root.selectedLayer.id,
                                         "opacity", value / 100)
                }
                SliderValueField {
                    Layout.preferredWidth: 58
                    value: String(Math.round(root.selectedStyle.opacity
                                             * 100))
                    from: 0
                    to: 100
                    integer: true
                    suffix: "%"
                    accessibleName: qsTr("Layer opacity")
                    onEdited: value => root.change(root.selectedLayer.id,
                                                    "opacity",
                                                    Number(value) / 100)
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Line pattern"); color: AppTheme.textMuted;
                        Layout.fillWidth: true }
                StyledComboBox {
                    Layout.preferredWidth: 125
                    enabled: root.selectedLayer
                             && (root.selectedLayer.kind === "window"
                                 || root.selectedLayer.kind === "condition")
                    model: [{ text: qsTr("Solid"), value: "solid" },
                            { text: qsTr("Dashed"), value: "dash" }]
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: indexOfValue(
                        root.selectedStyle.linePattern)
                    onActivated: root.change(root.selectedLayer.id,
                                             "linePattern", currentValue)
                }
            }
        }
        }
    }
}
