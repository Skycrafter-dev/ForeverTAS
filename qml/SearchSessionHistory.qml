import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "settings"

ColumnLayout {
    id: root
    required property var controller
    required property var viewer
    property int sortColumn: 0
    property bool sortAscending: true
    property int selectedRestart: -1
    property int observedRowCount: 0
    property string observedDirectory: ""
    readonly property var metricLabels:
        controller.cycleRows.length > 0
        ? controller.cycleRows[0].metricLabels : []
    readonly property var sortedRows:
        sorted(controller.cycleRows, sortColumn, sortAscending)

    objectName: "searchSessionHistory"
    spacing: 6

    function sorted(rows, column, ascending) {
        let result = []
        for (let i = 0; i < rows.length; ++i)
            result.push({ data: rows[i], sourceIndex: i })
        result.sort((a, b) => {
            if (column === 1) {
                const av = String(a.data.attemptsExact ?? a.data.attempts)
                const bv = String(b.data.attemptsExact ?? b.data.attempts)
                const comparison = av.length !== bv.length ? av.length - bv.length
                    : av < bv ? -1 : av > bv ? 1 : 0
                return ascending ? comparison : -comparison
            }
            const av = column === 0 ? Number(a.data.restart)
                : column === 1 ? Number(a.data.attempts)
                : column === 2 ? Number(a.data.elapsedMs)
                : Number(a.data.metrics[column - 3])
            const bv = column === 0 ? Number(b.data.restart)
                : column === 1 ? Number(b.data.attempts)
                : column === 2 ? Number(b.data.elapsedMs)
                : Number(b.data.metrics[column - 3])
            const comparison = av < bv ? -1 : av > bv ? 1 : 0
            return ascending ? comparison : -comparison
        })
        return result
    }

    function chooseRow(index, preview = true) {
        if (index < 0 || index >= controller.cycleRows.length)
            return
        controller.selectCycle(index)
        selectedRestart = Number(controller.cycleRows[index].restart)
        if (!preview)
            return
        let session = null
        for (let i = 0; i < controller.sessionOptions.length; ++i) {
            if (controller.sessionOptions[i].directory ===
                    controller.selectedSessionDirectory) {
                session = controller.sessionOptions[i]
                break
            }
        }
        if (session && Number(session.horizonMs) > 0)
            viewer.simulationHorizonMs = Number(session.horizonMs)
        const inputScript = controller.selectedInputsText
        const alreadySelected = viewer.previewInputScript === inputScript
        viewer.previewInputScript = inputScript
        const packs = session && session.packsDirectory
            ? session.packsDirectory : controller.packsDirectory
        const replay = session && session.replayPath
            ? session.replayPath : controller.replayPath
        const needsMap = packs.length > 0 && replay.length > 0 &&
            (!viewer.loaded || viewer.loadedReplayPath !== replay ||
             viewer.loadedPacksDirectory !== packs)
        if (needsMap)
            viewer.loadMap(packs, replay)
        else if (alreadySelected && viewer.loaded &&
                 packs.length > 0 && replay.length > 0)
            viewer.refreshInputPreview()
    }

    Connections {
        target: root.controller
        function onHistoryChanged() {
            const directory = root.controller.selectedSessionDirectory
            const count = root.controller.cycleRows.length
            if (directory !== root.observedDirectory ||
                count !== root.observedRowCount) {
                root.observedDirectory = directory
                root.observedRowCount = count
                if (count > 0)
                    Qt.callLater(() => {
                        if (root.controller.selectedSessionDirectory ===
                                directory)
                            root.chooseRow(count - 1, false)
                    })
                else
                    root.selectedRestart = -1
            }
        }
    }

    Component.onCompleted: {
        observedDirectory = controller.selectedSessionDirectory
        observedRowCount = controller.cycleRows.length
        if (observedRowCount > 0)
            Qt.callLater(() => chooseRow(observedRowCount - 1, false))
    }

    RowLayout {
        Layout.fillWidth: true
        Label {
            Layout.fillWidth: true
            text: qsTr("Session")
            font.weight: Font.Medium
        }
        StyledComboBox {
            id: sessionSelector
            objectName: "sessionSelector"
            enabled: !root.controller.running
            Layout.fillWidth: true
            model: root.controller.sessionOptions
            textRole: "label"
            valueRole: "directory"
            function synchronizeSelection() {
                const selected = indexOfValue(
                    root.controller.selectedSessionDirectory)
                if (currentIndex !== selected)
                    currentIndex = selected
            }
            Component.onCompleted: synchronizeSelection()
            onModelChanged: Qt.callLater(synchronizeSelection)
            onActivated: index => {
                root.selectedRestart = -1
                root.controller.selectSession(index)
                root.chooseRow(root.controller.cycleRows.length - 1)
            }
            Connections {
                target: root.controller
                function onHistoryChanged() {
                    sessionSelector.synchronizeSelection()
                }
            }
        }
    }

    ScrollView {
        objectName: "sessionTableScroll"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(270,
            40 + root.sortedRows.length * 38)
        visible: root.sortedRows.length > 0
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AsNeeded
        ScrollBar.vertical.policy: ScrollBar.AsNeeded
        contentWidth: Math.max(availableWidth,
                               310 + root.metricLabels.length * 110)

        ColumnLayout {
            width: Math.max(parent.width,
                            310 + root.metricLabels.length * 110)
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                spacing: 0
                Repeater {
                    model: 3 + root.metricLabels.length
                    ThemedButton {
                        required property int index
                        Layout.preferredWidth: index === 0 ? 55
                            : index === 1 ? 80
                            : index === 2 ? 90 : 110
                        flat: true
                        text: index === 0 ? qsTr("#")
                            : index === 1 ? qsTr("Attempts")
                            : index === 2 ? qsTr("Elapsed")
                            : root.metricLabels[index - 3]
                        ToolTip.visible: hovered
                        ToolTip.text: text
                        onClicked: {
                            if (root.sortColumn === index)
                                root.sortAscending = !root.sortAscending
                            else {
                                root.sortColumn = index
                                root.sortAscending = true
                            }
                        }
                    }
                }
                Item { Layout.fillWidth: true }
            }

            Repeater {
                model: root.sortedRows
                RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 0
                    Repeater {
                        model: 3 + root.metricLabels.length
                        Label {
                            required property int index
                            Layout.preferredWidth: index === 0 ? 55
                                : index === 1 ? 80
                                : index === 2 ? 90 : 110
                            Layout.preferredHeight: 36
                            verticalAlignment: Text.AlignVCenter
                            horizontalAlignment: Text.AlignRight
                            rightPadding: 8
                            font.family: "monospace"
                            elide: Text.ElideRight
                            text: index === 0 ? modelData.data.restart
                                : index === 1 ? (modelData.data.attemptsText ?? modelData.data.attempts)
                                : index === 2
                                  ? (Number(modelData.data.elapsedMs) / 1000)
                                      .toFixed(1) + " s"
                                : Number(modelData.data.metrics[index - 3])
                                      .toPrecision(6)
                            background: Rectangle {
                                color: root.selectedRestart ===
                                       Number(modelData.data.restart)
                                       ? AppTheme.selection
                                       : AppTheme.surface
                            }
                            MouseArea {
                                id: countHover
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onClicked: mouse => {
                                    if (mouse.button === Qt.RightButton && index === 1)
                                        countMenu.popup()
                                    else
                                        root.chooseRow(modelData.sourceIndex)
                                }
                            }
                            ToolTip.visible: index === 1 && countHover.containsMouse
                            ToolTip.text: String(modelData.data.attemptsText ?? modelData.data.attempts)
                            Menu {
                                id: countMenu
                                ThemedMenuItem {
                                    text: qsTr("Copy exact count")
                                    onTriggered: {
                                        countClipboard.text = String(modelData.data.attemptsExact ?? modelData.data.attempts)
                                        countClipboard.selectAll()
                                        countClipboard.copy()
                                    }
                                }
                            }
                        }
                    }
                    ThemedButton {
                        Layout.preferredWidth: 70
                        text: qsTr("Copy")
                        onClicked: {
                            root.chooseRow(modelData.sourceIndex)
                            clipboardSource.selectAll()
                            clipboardSource.copy()
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
            }
        }
    }

    TextArea {
        id: clipboardSource
        visible: false
        text: root.controller.selectedInputsText
    }
    TextEdit { id: countClipboard; visible: false }

    Label {
        Layout.fillWidth: true
        visible: root.controller.sessionOptions.length === 0
        text: qsTr("No saved sessions for this map")
        color: AppTheme.textMuted
    }
}
