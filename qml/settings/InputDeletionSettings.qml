import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    objectName: "inputDeletionSettings"

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

    SettingSwitch {
        label: qsTr("Delete steering events")
        checked: (root.settings["steerEnabled"] ?? "false") === "true"
        running: root.running
        onToggled: checked =>
            root.updateSetting("steerEnabled", checked ? "true" : "false")
    }
    SettingTextField {
        label: qsTr("Maximum steering deletions")
        value: root.settings["steerMaxCount"] ?? ""
        running: root.running
        minimum: 0
        onEdited: value => root.updateSetting("steerMaxCount", value)
    }
    SettingSwitch {
        label: qsTr("Delete accelerate events")
        checked: (root.settings["accelerateEnabled"] ?? "false") === "true"
        running: root.running
        onToggled: checked =>
            root.updateSetting("accelerateEnabled", checked ? "true" : "false")
    }
    SettingTextField {
        label: qsTr("Maximum accelerate deletions")
        value: root.settings["accelerateMaxCount"] ?? ""
        running: root.running
        minimum: 0
        onEdited: value => root.updateSetting("accelerateMaxCount", value)
    }
    SettingSwitch {
        label: qsTr("Delete brake events")
        checked: (root.settings["brakeEnabled"] ?? "false") === "true"
        running: root.running
        onToggled: checked =>
            root.updateSetting("brakeEnabled", checked ? "true" : "false")
    }
    SettingTextField {
        label: qsTr("Maximum brake deletions")
        value: root.settings["brakeMaxCount"] ?? ""
        running: root.running
        minimum: 0
        onEdited: value => root.updateSetting("brakeMaxCount", value)
    }
}
