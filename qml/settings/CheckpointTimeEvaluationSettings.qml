import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root
    objectName: "checkpointTimeEvaluationSettings"
    property var controller
    property var viewer
    readonly property var settings: controller.evaluationTargetSettings
    Layout.fillWidth: true
    spacing: 8

    SettingCombo {
        label: qsTr("Event")
        comboObjectName: "checkpointTimeEventTypeField"
        options: [{ label: qsTr("Checkpoint"), value: "checkpoint" },
                  { label: qsTr("Finish"), value: "finish" }]
        value: root.settings["eventType"] ?? "checkpoint"
        running: root.controller.running
        onSelected: value => root.controller.setEvaluationTargetSetting("eventType", value)
    }
    SettingTextField {
        label: qsTr("Checkpoint number")
        fieldObjectName: "checkpointTimeNumberField"
        visible: root.settings["eventType"] !== "finish"
        value: root.settings["checkpointIndex"] ?? "1"
        running: root.controller.running
        minimum: 1
        maximum: 4294967295
        onEdited: value => root.controller.setEvaluationTargetSetting("checkpointIndex", value)
    }
    SettingTextField {
        label: qsTr("Lap")
        fieldObjectName: "checkpointTimeLapField"
        value: root.settings["lap"] ?? "1"
        running: root.controller.running
        minimum: 1
        maximum: 4294967295
        onEdited: value => root.controller.setEvaluationTargetSetting("lap", value)
    }
    SettingTextField {
        label: qsTr("Map slot (-1 = any)")
        fieldObjectName: "checkpointTimeSlotField"
        value: root.settings["checkpointSlot"] ?? "-1"
        running: root.controller.running
        minimum: -1
        maximum: 4294967295
        onEdited: value => root.controller.setEvaluationTargetSetting("checkpointSlot", value)
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 12
        Label {
            Layout.fillWidth: true
            text: qsTr("Event index (0 = any)")
            wrapMode: Text.WordWrap
        }
        TextField {
            objectName: "checkpointTimeEventIndexField"
            Layout.preferredWidth: 126
            text: root.settings["eventIndex"] ?? "0"
            enabled: !root.controller.running
            selectByMouse: true
            inputMethodHints: Qt.ImhDigitsOnly
            validator: RegularExpressionValidator { regularExpression: /[0-9]+/ }
            color: enabled ? ThemeControls.AppTheme.text : ThemeControls.AppTheme.disabledText
            selectionColor: ThemeControls.AppTheme.selection
            selectedTextColor: ThemeControls.AppTheme.text
            onEditingFinished: root.controller.setEvaluationTargetSetting("eventIndex", text)
            background: Rectangle {
                color: parent.enabled ? ThemeControls.AppTheme.surface : ThemeControls.AppTheme.disabledSurface
                border.color: parent.activeFocus ? ThemeControls.AppTheme.focus : ThemeControls.AppTheme.border
                border.width: 1
                radius: 6
            }
        }
    }
}
