import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root

    objectName: "treeSearchSettings"
    property var controller
    readonly property var settings: controller.searchAlgorithmSettings
    readonly property int segmentCount: {
        const parsed = parseInt(root.settings["segmentCount"] ?? "", 10)
        return isNaN(parsed) ? 0 : parsed
    }

    Layout.fillWidth: true
    spacing: 8

    SettingTextField {
        fieldObjectName: "treeSegmentCountField"
        label: qsTr("Segments")
        value: root.settings["segmentCount"] ?? ""
        running: root.controller.running
        minimum: 1
        maximum: 20
        onEdited: value =>
            root.controller.setSearchAlgorithmSetting("segmentCount", value)
    }

    SettingSwitch {
        objectName: "treeAutoPromoteBestSwitch"
        label: qsTr("Promote each best result to baseline")
        checked: root.settings["autoPromoteBest"] === "true"
        running: root.controller.running
        onToggled: checked =>
            root.controller.setSearchAlgorithmSetting(
                "autoPromoteBest", checked ? "true" : "false")
    }

    Label {
        objectName: "treeSearchSummary"
        Layout.fillWidth: true
        text: root.segmentCount >= 1 && root.segmentCount <= 20
              ? qsTr("Each tree shares simulated prefixes across up to %1 "
                     + "attempts. CPU physics backends only.")
                    .arg(Math.pow(2, root.segmentCount).toLocaleString(
                             Qt.locale(), "f", 0))
              : qsTr("Segments must be a whole number from 1 to 20.")
        color: root.segmentCount >= 1 && root.segmentCount <= 20
               ? ThemeControls.AppTheme.textMuted
               : ThemeControls.AppTheme.error
        wrapMode: Text.WordWrap
        font.pixelSize: 11
    }

    Label {
        Layout.fillWidth: true
        visible: root.controller.validationMessage.startsWith("Tree search")
        text: root.controller.validationMessage
        color: ThemeControls.AppTheme.error
        wrapMode: Text.WordWrap
        font.pixelSize: 11
    }
}
