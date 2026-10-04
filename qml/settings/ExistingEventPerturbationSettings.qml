import QtQuick
import QtQuick.Layouts
import ".." as ThemeControls

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
    DurationField {
        label: qsTr("Maximum timing shift")
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
        value: ThemeControls.SteeringDisplay.displayValue(root.settings["steerDeltaMin"] ?? "")
        running: root.running
        integer: ThemeControls.SteeringDisplay.nativeUnits
        decimals: ThemeControls.SteeringDisplay.nativeUnits ? 0 : 3
        dragStep: ThemeControls.SteeringDisplay.nativeUnits ? 1 : 0.01
        minimum: ThemeControls.SteeringDisplay.nativeUnits ? -65536 : -1
        maximum: ThemeControls.SteeringDisplay.nativeUnits ? 65536 : 1
        onEdited: value => root.updateSetting("steerDeltaMin", ThemeControls.SteeringDisplay.storedValue(value))
    }
    SettingTextField {
        label: qsTr("Steering delta maximum")
        value: ThemeControls.SteeringDisplay.displayValue(root.settings["steerDeltaMax"] ?? "")
        running: root.running
        integer: ThemeControls.SteeringDisplay.nativeUnits
        decimals: ThemeControls.SteeringDisplay.nativeUnits ? 0 : 3
        dragStep: ThemeControls.SteeringDisplay.nativeUnits ? 1 : 0.01
        minimum: ThemeControls.SteeringDisplay.nativeUnits ? -65536 : -1
        maximum: ThemeControls.SteeringDisplay.nativeUnits ? 65536 : 1
        onEdited: value => root.updateSetting("steerDeltaMax", ThemeControls.SteeringDisplay.storedValue(value))
    }
    SettingSlider {
        sliderObjectName: "perturbationAbsoluteMinimumSlider"
        label: qsTr("Steering absolute minimum")
        value: ThemeControls.SteeringDisplay.displayValue(root.settings["steerAbsoluteMin"] ?? "")
        running: root.running
        from: ThemeControls.SteeringDisplay.nativeUnits ? -65536 : -1
        to: ThemeControls.SteeringDisplay.nativeUnits ? 65536 : 1
        stepSize: ThemeControls.SteeringDisplay.nativeUnits ? 1 : 0.01
        decimals: ThemeControls.SteeringDisplay.nativeUnits ? 0 : 2
        onEdited: value => root.updateSetting("steerAbsoluteMin", ThemeControls.SteeringDisplay.storedValue(value))
    }
    SettingSlider {
        sliderObjectName: "perturbationAbsoluteMaximumSlider"
        label: qsTr("Steering absolute maximum")
        value: ThemeControls.SteeringDisplay.displayValue(root.settings["steerAbsoluteMax"] ?? "")
        running: root.running
        from: ThemeControls.SteeringDisplay.nativeUnits ? -65536 : -1
        to: ThemeControls.SteeringDisplay.nativeUnits ? 65536 : 1
        stepSize: ThemeControls.SteeringDisplay.nativeUnits ? 1 : 0.01
        decimals: ThemeControls.SteeringDisplay.nativeUnits ? 0 : 2
        onEdited: value => root.updateSetting("steerAbsoluteMax", ThemeControls.SteeringDisplay.storedValue(value))
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
