import QtQuick
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root
    objectName: "smoothSteeringSettings"

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
        label: qsTr("Deformation count")
        value: root.settings["deformationCount"] ?? ""
        running: root.running
        minimum: 1
        onEdited: value => root.updateSetting("deformationCount", value)
    }
    DurationField {
        label: qsTr("Radius")
        value: root.settings["radiusMs"] ?? ""
        running: root.running
        dragStep: 10
        minimum: 0
        onEdited: value => root.updateSetting("radiusMs", value)
    }
    SettingTextField {
        label: qsTr("Amplitude minimum")
        value: ThemeControls.SteeringDisplay.displayValue(root.settings["amplitudeMin"] ?? "")
        running: root.running
        integer: ThemeControls.SteeringDisplay.nativeUnits
        decimals: ThemeControls.SteeringDisplay.nativeUnits ? 0 : 3
        dragStep: ThemeControls.SteeringDisplay.nativeUnits ? 1 : 0.01
        minimum: ThemeControls.SteeringDisplay.nativeUnits ? -65536 : -1
        maximum: ThemeControls.SteeringDisplay.nativeUnits ? 65536 : 1
        onEdited: value => root.updateSetting("amplitudeMin", ThemeControls.SteeringDisplay.storedValue(value))
    }
    SettingTextField {
        label: qsTr("Amplitude maximum")
        value: ThemeControls.SteeringDisplay.displayValue(root.settings["amplitudeMax"] ?? "")
        running: root.running
        integer: ThemeControls.SteeringDisplay.nativeUnits
        decimals: ThemeControls.SteeringDisplay.nativeUnits ? 0 : 3
        dragStep: ThemeControls.SteeringDisplay.nativeUnits ? 1 : 0.01
        minimum: ThemeControls.SteeringDisplay.nativeUnits ? -65536 : -1
        maximum: ThemeControls.SteeringDisplay.nativeUnits ? 65536 : 1
        onEdited: value => root.updateSetting("amplitudeMax", ThemeControls.SteeringDisplay.storedValue(value))
    }
}
