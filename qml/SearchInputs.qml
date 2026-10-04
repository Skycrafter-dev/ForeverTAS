import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

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
