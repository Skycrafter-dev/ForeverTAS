import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: root
    objectName: "existingEventPerturbationSettings"

    property var settings: ({})
    property var updateSetting
    property bool running: false
    property var viewer

    Layout.fillWidth: true
    spacing: 6

    TimeWindowSettings {
        viewer: root.viewer
        settings: root.settings
        updateSetting: root.updateSetting
        running: root.running
    }

    SettingTextField {
        label: qsTr("Seed")
        value: root.settings["seed"] ?? ""
        running: root.running
        minimum: 0
        onEdited: value => root.updateSetting("seed", value)
    }
    SettingTextField {
        label: qsTr("Minimum event count")
        value: root.settings["minCount"] ?? ""
        running: root.running
        minimum: 0
        onEdited: value => root.updateSetting("minCount", value)
    }
    SettingTextField {
        label: qsTr("Maximum event count")
        value: root.settings["maxCount"] ?? ""
        running: root.running
        minimum: 0
        onEdited: value => root.updateSetting("maxCount", value)
    }
    SettingTextField {
        label: qsTr("Maximum timing shift (ms)")
        value: root.settings["maxTimeShiftMs"] ?? ""
        running: root.running
        dragStep: 10
        minimum: 0
        onEdited: value => root.updateSetting("maxTimeShiftMs", value)
    }
    SettingCombo {
        comboObjectName: "perturbationSteeringModeCombo"
        label: qsTr("Steering perturbation")
        options: [
            { label: qsTr("Delta"), value: "delta" },
            { label: qsTr("Absolute"), value: "absolute" }
        ]
        value: root.settings["steerMode"] ?? "delta"
        running: root.running
        onSelected: value => root.updateSetting("steerMode", value)
    }
    SettingTextField {
        label: qsTr("Steering delta minimum")
        value: root.settings["steerDeltaMin"] ?? ""
        running: root.running
        integer: false
        decimals: 3
        dragStep: 0.01
        onEdited: value => root.updateSetting("steerDeltaMin", value)
    }
    SettingTextField {
        label: qsTr("Steering delta maximum")
        value: root.settings["steerDeltaMax"] ?? ""
        running: root.running
        integer: false
        decimals: 3
        dragStep: 0.01
        onEdited: value => root.updateSetting("steerDeltaMax", value)
    }
    SettingSlider {
        sliderObjectName: "perturbationAbsoluteMinimumSlider"
        label: qsTr("Steering absolute minimum")
        value: root.settings["steerAbsoluteMin"] ?? ""
        running: root.running
        from: -1
        to: 1
        stepSize: 0.01
        decimals: 2
        onEdited: value => root.updateSetting("steerAbsoluteMin", value)
    }
    SettingSlider {
        sliderObjectName: "perturbationAbsoluteMaximumSlider"
        label: qsTr("Steering absolute maximum")
        value: root.settings["steerAbsoluteMax"] ?? ""
        running: root.running
        from: -1
        to: 1
        stepSize: 0.01
        decimals: 2
        onEdited: value => root.updateSetting("steerAbsoluteMax", value)
    }
    SettingSwitch {
        label: qsTr("Toggle accelerate events")
        checked: (root.settings["toggleAccelerate"] ?? "false") === "true"
        running: root.running
        onToggled: checked =>
            root.updateSetting("toggleAccelerate", checked ? "true" : "false")
    }
    SettingSwitch {
        label: qsTr("Toggle brake events")
        checked: (root.settings["toggleBrake"] ?? "false") === "true"
        running: root.running
        onToggled: checked =>
            root.updateSetting("toggleBrake", checked ? "true" : "false")
    }
}
