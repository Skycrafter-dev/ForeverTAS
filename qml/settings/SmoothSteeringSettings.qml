import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: root
    objectName: "smoothSteeringSettings"

    property var settings: ({})
    property var updateSetting
    property bool running: false
    property var viewer

    Layout.fillWidth: true
    spacing: 6

    SteeringUnits {
        id: steeringUnits
        running: root.running
    }

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
    SettingTextField {
        label: qsTr("Radius (ms)")
        value: root.settings["radiusMs"] ?? ""
        running: root.running
        dragStep: 10
        minimum: 0
        onEdited: value => root.updateSetting("radiusMs", value)
    }
    SettingTextField {
        label: qsTr("Amplitude minimum")
        value: steeringUnits.displayValue(root.settings["amplitudeMin"] ?? "")
        running: root.running
        integer: steeringUnits.nativeUnits
        decimals: steeringUnits.nativeUnits ? 0 : 3
        dragStep: steeringUnits.nativeUnits ? 1 : 0.01
        minimum: steeringUnits.nativeUnits ? -65536 : -1
        maximum: steeringUnits.nativeUnits ? 65536 : 1
        onEdited: value => root.updateSetting("amplitudeMin", steeringUnits.storedValue(value))
    }
    SettingTextField {
        label: qsTr("Amplitude maximum")
        value: steeringUnits.displayValue(root.settings["amplitudeMax"] ?? "")
        running: root.running
        integer: steeringUnits.nativeUnits
        decimals: steeringUnits.nativeUnits ? 0 : 3
        dragStep: steeringUnits.nativeUnits ? 1 : 0.01
        minimum: steeringUnits.nativeUnits ? -65536 : -1
        maximum: steeringUnits.nativeUnits ? 65536 : 1
        onEdited: value => root.updateSetting("amplitudeMax", steeringUnits.storedValue(value))
    }
}
