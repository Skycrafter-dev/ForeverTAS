import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root

    objectName: "scriptedTargetEvaluationSettings"
    property var controller
    property var viewer
    readonly property var settings: controller.evaluationTargetSettings

    Layout.fillWidth: true
    spacing: 8

    TimeWindowSettings {
        settings: root.settings
        updateSetting: root.controller.setEvaluationTargetSetting
        running: root.controller.running
        viewer: root.viewer
    }

    RowLayout {
        Layout.fillWidth: true

        Label {
            Layout.fillWidth: true
            text: qsTr("Target script")
            font.weight: Font.Medium
        }

        ThemeControls.ThemedToolButton {
            text: "?"
            Accessible.name: qsTr("Target script syntax")
            ToolTip.visible: hovered
            ToolTip.text: qsTr("One directive per line: min EXPRESSION, max EXPRESSION, or target VALUE EXPRESSION. Lines starting with # are comments. For example: max kmh(car.speed)")
        }
    }

    ScrollView {
        id: scriptScroll
        Layout.fillWidth: true
        Layout.preferredHeight: 160
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AsNeeded
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        TextArea {
            id: scriptArea
            objectName: "scriptedTargetTextArea"
            width: Math.max(scriptScroll.availableWidth,
                            contentWidth + leftPadding + rightPadding)
            text: root.settings["script"] ?? ""
            enabled: !root.controller.running
            selectByMouse: true
            wrapMode: TextEdit.NoWrap
            textFormat: TextEdit.PlainText
            font.family: "monospace"
            font.pixelSize: 12
            color: enabled ? ThemeControls.AppTheme.text
                           : ThemeControls.AppTheme.disabledText
            placeholderText: qsTr("max kmh(car.speed)")
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Space && (event.modifiers & Qt.ControlModifier)) {
                    reference.complete()
                    event.accepted = true
                }
            }
            onTextChanged: {
                if (root.settings["script"] !== text)
                    root.controller.setEvaluationTargetSetting("script", text)
            }
            background: Rectangle {
                color: parent.enabled ? ThemeControls.AppTheme.surface
                                      : ThemeControls.AppTheme.disabledSurface
                border.width: 1
                border.color: parent.activeFocus
                              ? ThemeControls.AppTheme.focus
                              : ThemeControls.AppTheme.border
                radius: 6
            }
        }
    }

    ThemeControls.ConditionReference {
        id: reference
        objectName: "customTargetReference"
        controller: root.controller
        editor: scriptArea
        customTarget: true
    }

    Label {
        Layout.fillWidth: true
        visible: root.controller.validationMessage.startsWith("Custom target")
        text: root.controller.validationMessage
        color: ThemeControls.AppTheme.error
        wrapMode: Text.WordWrap
        font.pixelSize: 11
    }
}
