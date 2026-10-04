import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root
    objectName: "inputInsertionSettings"

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

    Label { text: qsTr("Steering"); font.weight: Font.Medium }
    SettingSwitch {
        label: qsTr("Enable steering insertion")
        checked: (root.settings["steerEnabled"] ?? "false") === "true"
        running: root.running
        onToggled: checked =>
            root.updateSetting("steerEnabled", checked ? "true" : "false")
    }
    SettingCombo {
        comboObjectName: "insertionSteeringModeCombo"
        label: qsTr("Steering value mode")
        options: [
            { label: qsTr("Offset"), value: "offset" },
            { label: qsTr("Absolute"), value: "absolute" }
        ]
        value: root.settings["steerMode"] ?? "offset"
        running: root.running
        onSelected: value => root.updateSetting("steerMode", value)
    }
    SettingTextField {
        label: qsTr("Minimum steering insertions")
        value: root.settings["steerMinCount"] ?? ""
        running: root.running
        minimum: 0
        onEdited: value => root.updateSetting("steerMinCount", value)
    }
    SettingTextField {
        label: qsTr("Maximum steering insertions")
        value: root.settings["steerMaxCount"] ?? ""
        running: root.running
        minimum: 0
        onEdited: value => root.updateSetting("steerMaxCount", value)
    }
    DurationField {
        label: qsTr("Maximum steering hold")
        value: root.settings["steerMaxHoldMs"] ?? ""
        running: root.running
        dragStep: 10
        minimum: 0
        onEdited: value => root.updateSetting("steerMaxHoldMs", value)
    }
    SettingSlider {
        sliderObjectName: "insertionAbsoluteMinimumSlider"
        label: qsTr("Absolute steering minimum")
        value: ThemeControls.SteeringDisplay.displayValue(root.settings["steerAbsoluteMin"] ?? "")
        running: root.running
        from: ThemeControls.SteeringDisplay.nativeUnits ? -65536 : -1
        to: ThemeControls.SteeringDisplay.nativeUnits ? 65536 : 1
        stepSize: ThemeControls.SteeringDisplay.nativeUnits ? 1 : 0.01
        decimals: ThemeControls.SteeringDisplay.nativeUnits ? 0 : 2
        onEdited: value => root.updateSetting("steerAbsoluteMin", ThemeControls.SteeringDisplay.storedValue(value))
    }
    SettingSlider {
        sliderObjectName: "insertionAbsoluteMaximumSlider"
        label: qsTr("Absolute steering maximum")
        value: ThemeControls.SteeringDisplay.displayValue(root.settings["steerAbsoluteMax"] ?? "")
        running: root.running
        from: ThemeControls.SteeringDisplay.nativeUnits ? -65536 : -1
        to: ThemeControls.SteeringDisplay.nativeUnits ? 65536 : 1
        stepSize: ThemeControls.SteeringDisplay.nativeUnits ? 1 : 0.01
        decimals: ThemeControls.SteeringDisplay.nativeUnits ? 0 : 2
        onEdited: value => root.updateSetting("steerAbsoluteMax", ThemeControls.SteeringDisplay.storedValue(value))
    }
    SettingTextField {
        label: qsTr("Steering offset minimum")
        value: ThemeControls.SteeringDisplay.displayValue(root.settings["steerOffsetMin"] ?? "")
        running: root.running
        integer: ThemeControls.SteeringDisplay.nativeUnits
        decimals: ThemeControls.SteeringDisplay.nativeUnits ? 0 : 3
        dragStep: ThemeControls.SteeringDisplay.nativeUnits ? 1 : 0.01
        minimum: ThemeControls.SteeringDisplay.nativeUnits ? -65536 : -1
        maximum: ThemeControls.SteeringDisplay.nativeUnits ? 65536 : 1
        onEdited: value => root.updateSetting("steerOffsetMin", ThemeControls.SteeringDisplay.storedValue(value))
    }
    SettingTextField {
        label: qsTr("Steering offset maximum")
        value: ThemeControls.SteeringDisplay.displayValue(root.settings["steerOffsetMax"] ?? "")
        running: root.running
        integer: ThemeControls.SteeringDisplay.nativeUnits
        decimals: ThemeControls.SteeringDisplay.nativeUnits ? 0 : 3
        dragStep: ThemeControls.SteeringDisplay.nativeUnits ? 1 : 0.01
        minimum: ThemeControls.SteeringDisplay.nativeUnits ? -65536 : -1
        maximum: ThemeControls.SteeringDisplay.nativeUnits ? 65536 : 1
        onEdited: value => root.updateSetting("steerOffsetMax", ThemeControls.SteeringDisplay.storedValue(value))
    }

    Label { text: qsTr("Accelerate"); font.weight: Font.Medium }
    SettingSwitch {
        label: qsTr("Enable accelerate insertion")
        checked: (root.settings["accelerateEnabled"] ?? "false") === "true"
        running: root.running
        onToggled: checked =>
            root.updateSetting("accelerateEnabled", checked ? "true" : "false")
    }
    SettingTextField {
        label: qsTr("Minimum accelerate insertions")
        value: root.settings["accelerateMinCount"] ?? ""
        running: root.running
        minimum: 0
        onEdited: value => root.updateSetting("accelerateMinCount", value)
    }
    SettingTextField {
        label: qsTr("Maximum accelerate insertions")
        value: root.settings["accelerateMaxCount"] ?? ""
        running: root.running
        minimum: 0
        onEdited: value => root.updateSetting("accelerateMaxCount", value)
    }
    DurationField {
        label: qsTr("Maximum accelerate hold")
        value: root.settings["accelerateMaxHoldMs"] ?? ""
        running: root.running
        dragStep: 10
        minimum: 0
        onEdited: value => root.updateSetting("accelerateMaxHoldMs", value)
    }

    Label { text: qsTr("Brake"); font.weight: Font.Medium }
    SettingSwitch {
        label: qsTr("Enable brake insertion")
        checked: (root.settings["brakeEnabled"] ?? "false") === "true"
        running: root.running
        onToggled: checked =>
            root.updateSetting("brakeEnabled", checked ? "true" : "false")
    }
    SettingTextField {
        label: qsTr("Minimum brake insertions")
        value: root.settings["brakeMinCount"] ?? ""
        running: root.running
        minimum: 0
        onEdited: value => root.updateSetting("brakeMinCount", value)
    }
    SettingTextField {
        label: qsTr("Maximum brake insertions")
        value: root.settings["brakeMaxCount"] ?? ""
        running: root.running
        minimum: 0
        onEdited: value => root.updateSetting("brakeMaxCount", value)
    }
    DurationField {
        label: qsTr("Maximum brake hold")
        value: root.settings["brakeMaxHoldMs"] ?? ""
        running: root.running
        dragStep: 10
        minimum: 0
        onEdited: value => root.updateSetting("brakeMaxHoldMs", value)
    }
}
