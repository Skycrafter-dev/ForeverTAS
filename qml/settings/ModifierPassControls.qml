import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root

    required property var composition
    readonly property bool layoutValid:
        passSelector.width >= width - 1 && passActions.width >= width - 1

    objectName: "modifierPassControls"
    spacing: 6

    Connections {
        target: root.composition
        function onActivePassIndexChanged() {
            passSelector.synchronizeSelection()
        }
    }

    StyledComboBox {
        id: passSelector

        function synchronizeSelection() {
            currentIndex = root.composition.activePassIndex
        }

        objectName: "modifierPassSelector"
        Layout.fillWidth: true
        Accessible.name: qsTr("Input pass")
        model: root.composition.passes.map((pass, index) => {
            const descriptor = root.composition.option(pass.id)
            return qsTr("%1. %2").arg(index + 1)
                .arg(descriptor ? descriptor.label : pass.id)
        })
        currentIndex: root.composition.activePassIndex
        onModelChanged: Qt.callLater(synchronizeSelection)
        displayText: root.composition.passCount === 0 ? qsTr("No input passes")
                                         : currentText
        enabled: root.composition.passCount > 0
        onActivated: index => root.composition.selectPass(index)
    }

    RowLayout {
        id: passActions

        objectName: "modifierPassActions"
        Layout.fillWidth: true
        spacing: 4

        ThemeControls.ThemedIconButton {
            id: addPassButton

            objectName: "addModifierButton"
            icon.source: "qrc:/icons/plus.svg"
            enabled: root.composition.options.length > 0
                     && !root.composition.controller.running
            Accessible.name: qsTr("Add input pass")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: addPassMenu.popup(addPassButton, 0, addPassButton.height)

            Menu {
                id: addPassMenu

                objectName: "addModifierMenu"
                y: addPassButton.height
                width: Math.max(240, root.width)
                enabled: !root.composition.controller.running

                Repeater {
                    model: root.composition.options

                    delegate: ThemeControls.ThemedMenuItem {
                        required property var modelData

                        text: modelData.label
                        enabled: !root.composition.controller.running
                        onTriggered: root.composition.addPass(modelData.id)
                    }

                }

            }

        }

        ThemeControls.ThemedIconButton {
            objectName: "modifierPassUp"
            icon.source: "qrc:/icons/arrow-up.svg"
            enabled: root.composition.activePassIndex > 0
                     && !root.composition.controller.running
            Accessible.name: qsTr("Move pass earlier")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.composition.movePass(-1)
        }

        ThemeControls.ThemedIconButton {
            objectName: "modifierPassDown"
            icon.source: "qrc:/icons/arrow-down.svg"
            enabled: root.composition.activePassIndex >= 0
                     && root.composition.activePassIndex + 1
                        < root.composition.passCount
                     && !root.composition.controller.running
            Accessible.name: qsTr("Move pass later")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.composition.movePass(1)
        }

        ThemeControls.ThemedIconButton {
            objectName: "modifierPassRemove"
            icon.source: "qrc:/icons/trash-2.svg"
            enabled: root.composition.passCount > 0
                     && !root.composition.controller.running
            Accessible.name: qsTr("Delete selected pass")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.composition.removePass()
        }

        Item {
            Layout.fillWidth: true
        }

        Label {
            text: root.composition.passCount === 0 ? "0 / 0"
                : qsTr("%1 / %2").arg(root.composition.activePassIndex + 1).arg(root.composition.passCount)
            color: ThemeControls.AppTheme.textMuted
        }

    }

}
