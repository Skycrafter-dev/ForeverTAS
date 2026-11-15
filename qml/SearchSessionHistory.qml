import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "settings"

ColumnLayout {
    id: root
    required property var controller
    required property var viewer
    signal inputsSelected()
    signal liveSelected()
    readonly property double selectedRestart: controller.selectedCycleRestart
    // the running restart, until it is saved as a row
    readonly property var liveRow: {
        const live = controller.liveRow
        if (!controller.running || live === undefined || live.columns === undefined) return null
        for (const row of controller.cycleRows)
            if (Number(row.restart) === Number(live.restart)) return null
        return live
    }

    objectName: "searchSessionHistory"
    spacing: 6

    function chooseRow(index, preview = true) {
        if (index < 0 || index >= controller.cycleRows.length)
            return
        controller.selectCycle(index)
        if (!preview)
            return
        inputsSelected()
        viewer.previewingHistory = true
        let session = null
        for (let i = 0; i < controller.sessionOptions.length; ++i) {
            if (controller.sessionOptions[i].directory ===
                    controller.selectedSessionDirectory) {
                session = controller.sessionOptions[i]
                break
            }
        }
        const inputScript = controller.selectedInputsText
        const alreadySelected = viewer.previewInputScript === inputScript
        viewer.previewInputScript = inputScript
        const packs = session && session.packsDirectory
            ? session.packsDirectory : controller.packsDirectory
        const replay = session && session.replayPath
            ? session.replayPath : controller.replayPath
        const needsMap = packs.length > 0 && replay.length > 0 &&
            !viewer.isMapLoaded(packs, replay)
        if (needsMap)
            viewer.loadMap(packs, replay)
        else if (alreadySelected && viewer.loaded &&
                 packs.length > 0 && replay.length > 0)
            viewer.refreshInputPreview()
    }

    RowLayout {
        Layout.fillWidth: true
        visible: root.controller.sessionOptions.length > 0
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

    RowLayout {
        Layout.fillWidth: true
        visible: root.controller.historyHasOlder || root.controller.historyHasNewer
        ThemedButton {
            objectName: "olderCyclePage"
            text: qsTr("Older")
            icon.source: "qrc:/icons/arrow-up.svg"
            enabled: root.controller.historyHasOlder
            onClicked: root.controller.changeCyclePage(true)
        }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: root.controller.cycleRows.length > 0
                ? qsTr("%1 - %2").arg(root.controller.cycleRows[0].restart)
                    .arg(root.controller.cycleRows[root.controller.cycleRows.length - 1].restart)
                : ""
        }
        ThemedButton {
            objectName: "newerCyclePage"
            text: qsTr("Newer")
            icon.source: "qrc:/icons/arrow-down.svg"
            enabled: root.controller.historyHasNewer
            onClicked: root.controller.changeCyclePage(false)
        }
    }

    ScrollView {
        objectName: "sessionTableScroll"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(300, table.implicitHeight + 16)
        visible: root.controller.cycleRows.length > 0 || root.liveRow !== null
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AsNeeded
        ScrollBar.vertical.policy: ScrollBar.AsNeeded
        contentWidth: table.implicitWidth

        ResultTable {
            id: table
            rows: root.controller.cycleRows
            liveRow: root.liveRow
            selectedRestart: root.selectedRestart
            onChosen: row => row < 0 ? root.liveSelected() : root.chooseRow(row)
            onCopyRequested: row => {
                if (row < 0) {
                    liveClipboard.selectAll()
                    liveClipboard.copy()
                    return
                }
                root.chooseRow(row)
                clipboardSource.selectAll()
                clipboardSource.copy()
            }
        }
    }

    ThemedButton {
        objectName: "previewBaseInputsButton"
        Layout.alignment: Qt.AlignRight
        visible: root.viewer.previewingHistory
        icon.source: "qrc:/icons/flag.svg"
        text: qsTr("Preview base inputs")
        onClicked: root.controller.previewBaseInputs()
    }

    TextArea {
        id: clipboardSource
        visible: false
        text: root.controller.selectedInputsText
    }
    TextEdit {
        id: liveClipboard
        visible: false
        text: root.controller.bestInputsText ?? ""
    }

    Label {
        Layout.fillWidth: true
        visible: root.controller.sessionOptions.length === 0
        text: qsTr("No saved sessions for this map")
        color: AppTheme.textMuted
    }
}
