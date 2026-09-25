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
        value: root.settings["amplitudeMin"] ?? ""
        running: root.running
        integer: false
        decimals: 3
        dragStep: 0.01
        onEdited: value => root.updateSetting("amplitudeMin", value)
    }
    SettingTextField {
        label: qsTr("Amplitude maximum")
        value: root.settings["amplitudeMax"] ?? ""
        running: root.running
        integer: false
        decimals: 3
        dragStep: 0.01
        onEdited: value => root.updateSetting("amplitudeMax", value)
    }
}
