import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    objectName: "checkpointTimeEvaluationSettings"
    property var controller
    property var viewer
    readonly property var settings: controller.evaluationTargetSettings
    readonly property string findNumbersHint: "\n\n" + qsTr(
        "All the numbers you set must match the same checkpoint. To find "
        + "them, click Evaluate base: it shows the checkpoint number, map "
        + "slot and event index that your base inputs reach.")
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
        info: qsTr("Which checkpoint to aim for, counted in the order the car "
                   + "passes them during the lap. 1 is the first checkpoint the "
                   + "car passes, 2 is the second, and so on.")
              + root.findNumbersHint
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
        info: qsTr("Picks one exact checkpoint on the map, no matter when the "
                   + "car passes it. Each checkpoint on the map has its own "
                   + "fixed number, starting at 0. Use -1 to allow any "
                   + "checkpoint.")
              + root.findNumbersHint
        fieldObjectName: "checkpointTimeSlotField"
        value: root.settings["checkpointSlot"] ?? "-1"
        running: root.controller.running
        minimum: -1
        maximum: 4294967295
        onEdited: value => root.controller.setEvaluationTargetSetting("checkpointSlot", value)
    }
    SettingTextField {
        label: qsTr("Event index (0 = any)")
        info: qsTr("Counts every checkpoint and finish the car passes since "
                   + "the start, across all laps. 1 is the first one, 2 is the "
                   + "second, and so on. Use 0 to allow any.")
              + root.findNumbersHint
        fieldObjectName: "checkpointTimeEventIndexField"
        value: root.settings["eventIndex"] ?? "0"
        running: root.controller.running
        minimum: 0
        maximum: 18446744073709551615
        onEdited: value => root.controller.setEvaluationTargetSetting("eventIndex", value)
    }
}
