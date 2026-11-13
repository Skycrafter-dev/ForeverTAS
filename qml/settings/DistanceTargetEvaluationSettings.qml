import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

// Distance to a checkpoint or the finish, picked like the Checkpoint time
// target: runs that reach it rank by when they do, the others by how many
// checkpoints they still need before it, then by how close the car's body
// got to its trigger.
ColumnLayout {
    id: root
    objectName: "distanceTargetEvaluationSettings"
    property var controller
    property var viewer
    readonly property var settings: controller.evaluationTargetSettings
    readonly property string findNumbersHint: "\n\n" + qsTr(
        "The race view shows each checkpoint's map slot while this target "
        + "is picked. Evaluate base shows the checkpoint number, map slot "
        + "and event index your base inputs reach.")

    Layout.fillWidth: true
    spacing: 8

    TimeWindowSettings {
        chainController: root.controller
        chainKey: "target"
        settings: root.settings
        updateSetting: root.controller.setEvaluationTargetSetting
        running: root.controller.running
        viewer: root.viewer
    }

    SettingCombo {
        label: qsTr("Measure")
        info: qsTr("Straight line: the gap between the car and the trigger, "
                   + "through walls. Around walls: the shortest way to the "
                   + "trigger that goes around the walls near it, ignoring "
                   + "gravity. Not on Vulkan yet.")
        comboObjectName: "distanceRouteField"
        options: [{ label: qsTr("Straight line"), value: "straight" },
                  { label: qsTr("Around walls"), value: "around" }]
        value: root.settings["route"] ?? "straight"
        running: root.controller.running
        onSelected: value => root.controller.setEvaluationTargetSetting("route", value)
    }
    SettingTextField {
        label: qsTr("Walls within (m)")
        info: qsTr("Only walls this close to the trigger count. Farther out, "
                   + "the way is a straight line. Larger takes longer to "
                   + "prepare and to measure.")
        fieldObjectName: "distanceReachField"
        visible: root.settings["route"] === "around"
        value: root.settings["reach"] ?? "32"
        running: root.controller.running
        minimum: 1
        maximum: 256
        onEdited: value => root.controller.setEvaluationTargetSetting("reach", value)
    }
    SettingCombo {
        label: qsTr("Event")
        comboObjectName: "distanceEventTypeField"
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
                   + "car passes, 2 is the second, and so on. With any map slot, "
                   + "the distance is to the nearest checkpoint not taken yet.")
              + root.findNumbersHint
        fieldObjectName: "distanceCheckpointNumberField"
        visible: root.settings["eventType"] !== "finish"
        value: root.settings["checkpointIndex"] ?? "1"
        running: root.controller.running
        minimum: 1
        maximum: 4294967295
        onEdited: value => root.controller.setEvaluationTargetSetting("checkpointIndex", value)
    }
    SettingTextField {
        label: qsTr("Lap")
        fieldObjectName: "distanceLapField"
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
        fieldObjectName: "distanceSlotField"
        visible: root.settings["eventType"] !== "finish"
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
        fieldObjectName: "distanceEventIndexField"
        value: root.settings["eventIndex"] ?? "0"
        running: root.controller.running
        minimum: 0
        maximum: 18446744073709551615
        onEdited: value => root.controller.setEvaluationTargetSetting("eventIndex", value)
    }
}
