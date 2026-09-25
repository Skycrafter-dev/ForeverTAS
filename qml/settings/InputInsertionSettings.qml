import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

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
    SettingTextField {
        label: qsTr("Maximum steering hold (ms)")
        value: root.settings["steerMaxHoldMs"] ?? ""
        running: root.running
        dragStep: 10
        minimum: 0
        onEdited: value => root.updateSetting("steerMaxHoldMs", value)
    }
    SettingSlider {
        sliderObjectName: "insertionAbsoluteMinimumSlider"
        label: qsTr("Absolute steering minimum")
        value: root.settings["steerAbsoluteMin"] ?? ""
        running: root.running
        from: -1
        to: 1
        stepSize: 0.01
        decimals: 2
        onEdited: value => root.updateSetting("steerAbsoluteMin", value)
    }
    SettingSlider {
        sliderObjectName: "insertionAbsoluteMaximumSlider"
        label: qsTr("Absolute steering maximum")
        value: root.settings["steerAbsoluteMax"] ?? ""
        running: root.running
        from: -1
        to: 1
        stepSize: 0.01
        decimals: 2
        onEdited: value => root.updateSetting("steerAbsoluteMax", value)
    }
    SettingTextField {
        label: qsTr("Steering offset minimum")
        value: root.settings["steerOffsetMin"] ?? ""
        running: root.running
        integer: false
        decimals: 3
        dragStep: 0.01
        onEdited: value => root.updateSetting("steerOffsetMin", value)
    }
    SettingTextField {
        label: qsTr("Steering offset maximum")
        value: root.settings["steerOffsetMax"] ?? ""
        running: root.running
        integer: false
        decimals: 3
        dragStep: 0.01
        onEdited: value => root.updateSetting("steerOffsetMax", value)
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
    SettingTextField {
        label: qsTr("Maximum accelerate hold (ms)")
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
    SettingTextField {
        label: qsTr("Maximum brake hold (ms)")
        value: root.settings["brakeMaxHoldMs"] ?? ""
        running: root.running
        dragStep: 10
        minimum: 0
        onEdited: value => root.updateSetting("brakeMaxHoldMs", value)
    }
}
