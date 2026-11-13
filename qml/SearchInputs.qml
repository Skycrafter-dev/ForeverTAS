import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ResultFormat.js" as ResultFormat

// Results panel: the Current best / History tabs switch everything below
// them. History-only content (sessions and restarts) is placed by the user
// of this component and shown only on the History tab.
ColumnLayout {
    id: root
    objectName: "searchInputs"
    required property var controller
    default property alias historyContent: historyColumn.data
    property alias selectedSource: sourceTabs.currentIndex
    readonly property bool showingHistory: selectedSource === 1
    readonly property var liveRow: controller.liveRow ?? ({})
    readonly property string script: showingHistory
        ? controller.selectedInputsText : controller.bestInputsText
    spacing: 6

    Connections {
        target: root.controller
        function onRunningChanged() {
            if (root.controller.running) root.selectedSource = 0
        }
    }

    RowLayout {
        Layout.fillWidth: true
        TabBar {
            // On glass: no band behind the tabs.
            background: Item {}
            id: sourceTabs
            objectName: "bestInputsSourceTabs"
            Layout.fillWidth: true
            currentIndex: 1
            ThemedTabButton { text: qsTr("Current best") }
            ThemedTabButton { text: qsTr("History") }
        }
        ThemedButton {
            objectName: "copyBestInputsButton"
            text: qsTr("Copy all")
            enabled: root.script.length > 0
            onClicked: {
                bestInputsArea.selectAll()
                bestInputsArea.copy()
                bestInputsArea.select(0, 0)
            }
        }
    }
    // Current best: the search's best as one row of the History table, and
    // how it got there
    ColumnLayout {
        objectName: "currentBestRow"
        Layout.fillWidth: true
        visible: !root.showingHistory && root.liveRow.columns !== undefined
        spacing: 4
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: currentBest.implicitHeight + 14
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AsNeeded
            ScrollBar.vertical.policy: ScrollBar.AlwaysOff
            contentWidth: currentBest.implicitWidth
            ResultTable {
                id: currentBest
                sortable: false
                liveRow: root.liveRow.columns !== undefined ? root.liveRow : null
                onCopyRequested: {
                    bestInputsArea.selectAll()
                    bestInputsArea.copy()
                    bestInputsArea.select(0, 0)
                }
            }
        }
        Label {
            Layout.fillWidth: true
            readonly property var live: root.liveRow
            readonly property int count: Number(live.improvements ?? 0)
            text: count === 0 ? qsTr("No improvement yet")
                : (count === 1 ? qsTr("1 improvement") : qsTr("%1 improvements").arg(count))
                  + (live.lastImprovementMs !== undefined
                     ? qsTr(", the last after %1").arg(
                           ResultFormat.format("elapsed", live.lastImprovementMs, 1))
                     : "")
            color: AppTheme.textMuted
            font.pixelSize: 11
        }
    }

    ColumnLayout {
        id: historyColumn
        objectName: "historyContent"
        Layout.fillWidth: true
        visible: root.showingHistory
        spacing: 6
    }

    Label {
        objectName: "inputsEmptyHint"
        Layout.fillWidth: true
        visible: root.script.length === 0
                 && (!root.showingHistory || (root.controller.cycleRows ?? []).length > 0)
        text: root.showingHistory
              ? qsTr("Select a restart in the table to see its inputs.")
              : qsTr("No best inputs yet. Start a search to find some.")
        color: AppTheme.textMuted
        wrapMode: Text.WordWrap
    }

    ScrollView {
        id: bestInputsScroll
        objectName: "bestInputsScrollView"
        visible: root.script.length > 0
        Layout.fillWidth: true
        Layout.preferredHeight: 260
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AsNeeded
        ScrollBar.vertical.policy: ScrollBar.AsNeeded
        TextArea {
            id: bestInputsArea
            objectName: "bestInputsTextArea"
            width: Math.max(bestInputsScroll.availableWidth,
                            contentWidth + leftPadding + rightPadding)
            text: root.script
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.NoWrap
            textFormat: TextEdit.PlainText
            font.family: "monospace"
            font.pixelSize: 12
            color: AppTheme.text
            background: Rectangle {
                color: AppTheme.surface
                border.width: 1
                border.color: bestInputsArea.activeFocus ? AppTheme.focus : AppTheme.border
                radius: 6
            }
        }
    }
}
