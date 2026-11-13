import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: root

    objectName: "randomSteeringMutationSettings"
    property var settings: ({})
    property var updateSetting
    property string windowChainKey: ""
    property var chainController: null
    property bool running: false
    property var viewer

    Layout.fillWidth: true
    spacing: 8

    TimeWindowSettings {
        chainController: root.chainController
        chainKey: root.windowChainKey
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
}
