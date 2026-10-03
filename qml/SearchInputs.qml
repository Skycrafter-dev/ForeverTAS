import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property var controller
    property alias selectedSource: sourceTabs.currentIndex
    readonly property string script: selectedSource === 0
        ? controller.bestInputsText : controller.selectedInputsText
    visible: controller.bestInputsText.length > 0 || controller.selectedInputsText.length > 0
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
    ScrollView {
        id: bestInputsScroll
        objectName: "bestInputsScrollView"
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
