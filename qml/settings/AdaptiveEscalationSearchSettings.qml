import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root

    objectName: "adaptiveEscalationSearchSettings"
    property var controller
    readonly property var settings: controller.searchAlgorithmSettings

    function wholeNumber(key) {
        const parsed = parseInt(root.settings[key] ?? "", 10)
        return isNaN(parsed) ? -1 : parsed
    }

    readonly property int segmentCount: root.wholeNumber("segmentCount")
    readonly property int escalateAfterTrees:
        root.wholeNumber("escalateAfterTrees")
    readonly property int migrationSeconds:
        root.wholeNumber("migrationSeconds")
    readonly property bool settingsValid:
        root.segmentCount >= 1 && root.segmentCount <= 20 &&
        root.escalateAfterTrees >= 1 && root.migrationSeconds >= 0

    Layout.fillWidth: true
    spacing: 8

    SettingTextField {
        fieldObjectName: "escalationSegmentCountField"
        label: qsTr("Segments")
        value: root.settings["segmentCount"] ?? ""
        running: root.controller.running
        minimum: 1
        maximum: 20
        onEdited: value =>
            root.controller.setSearchAlgorithmSetting("segmentCount", value)
    }

    SettingTextField {
        fieldObjectName: "escalationTreesField"
        label: qsTr("Trees without improvement before widening")
        value: root.settings["escalateAfterTrees"] ?? ""
        running: root.controller.running
        minimum: 1
        maximum: 1000000
        onEdited: value =>
            root.controller.setSearchAlgorithmSetting(
                "escalateAfterTrees", value)
    }

    SettingTextField {
        fieldObjectName: "escalationMigrationSecondsField"
        label: qsTr("Island migration, seconds (0 = share every best)")
        value: root.settings["migrationSeconds"] ?? ""
        running: root.controller.running
        minimum: 0
        maximum: 3600
        onEdited: value =>
            root.controller.setSearchAlgorithmSetting(
                "migrationSeconds", value)
    }

    SettingSwitch {
        objectName: "escalationAutoPromoteBestSwitch"
        label: qsTr("Promote each best result to baseline")
        checked: root.settings["autoPromoteBest"] === "true"
        running: root.controller.running
        onToggled: checked =>
            root.controller.setSearchAlgorithmSetting(
                "autoPromoteBest", checked ? "true" : "false")
    }

    Label {
        objectName: "escalationSummary"
        Layout.fillWidth: true
        text: root.settingsValid
              ? qsTr("Starts by changing one of %1 segments per attempt and "
                     + "doubles the changed segments after %2 trees without "
                     + "improvement, up to all of them. Any improvement "
                     + "returns it to one.")
                    .arg(root.segmentCount).arg(root.escalateAfterTrees)
              : qsTr("Use 1 to 20 segments, at least one tree before "
                     + "widening, and whole seconds of migration.")
        color: root.settingsValid
               ? ThemeControls.AppTheme.textMuted
               : ThemeControls.AppTheme.error
        wrapMode: Text.WordWrap
        font.pixelSize: 11
    }

    Label {
        Layout.fillWidth: true
        visible: root.controller.validationMessage.startsWith(
                     "Adaptive escalation")
        text: root.controller.validationMessage
        color: ThemeControls.AppTheme.error
        wrapMode: Text.WordWrap
        font.pixelSize: 11
    }
}
