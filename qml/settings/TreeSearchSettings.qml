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
    readonly property int leafCount: {
        const parsed = parseInt(root.settings["leafCount"] ?? "", 10)
        return isNaN(parsed) ? 0 : parsed
    }
    readonly property bool segmentCountValid:
        root.segmentCount >= 1 && root.segmentCount <= 20

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

    SettingTextField {
        fieldObjectName: "treeLeafCountField"
        label: qsTr("Leaves per tree (0 = 2^segments)")
        value: root.settings["leafCount"] ?? ""
        running: root.controller.running
        minimum: 0
        maximum: 1048576
        onEdited: value =>
            root.controller.setSearchAlgorithmSetting("leafCount", value)
    }

    SettingTextField {
        fieldObjectName: "treeBranchedSegmentCountField"
        label: qsTr("Branched segments per tree (0 = all)")
        value: root.settings["branchedSegmentCount"] ?? ""
        running: root.controller.running
        minimum: 0
        maximum: 20
        onEdited: value =>
            root.controller.setSearchAlgorithmSetting(
                "branchedSegmentCount", value)
    }

    SettingSwitch {
        objectName: "treeVaryBranchedSegmentCountSwitch"
        label: qsTr("Branch a random number of segments up to that")
        checked: root.settings["varyBranchedSegmentCount"] === "true"
        running: root.controller.running
        onToggled: checked =>
            root.controller.setSearchAlgorithmSetting(
                "varyBranchedSegmentCount", checked ? "true" : "false")
    }

    SettingCombo {
        comboObjectName: "treeUnbranchedSegmentsCombo"
        label: qsTr("Other segments")
        value: root.settings["unbranchedSegments"] ?? "draw"
        options: [
            { "label": qsTr("Draw once"), "value": "draw" },
            { "label": qsTr("Keep baseline"), "value": "keep" }
        ]
        running: root.controller.running
        onSelected: value =>
            root.controller.setSearchAlgorithmSetting(
                "unbranchedSegments", value)
    }

    SettingTextField {
        fieldObjectName: "treeFlatWorkerCountField"
        label: qsTr("Basic bruteforce workers (multi-threaded CPU)")
        value: root.settings["flatWorkerCount"] ?? ""
        running: root.controller.running
        minimum: 0
        maximum: 256
        onEdited: value =>
            root.controller.setSearchAlgorithmSetting(
                "flatWorkerCount", value)
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
        text: root.segmentCountValid
              ? qsTr("Each tree shares simulated prefixes across up to %1 "
                     + "attempts. Keeping the other segments changes only "
                     + "the branched ones in every attempt. CPU physics "
                     + "backends only.")
                    .arg((root.leafCount > 0
                          ? root.leafCount
                          : Math.pow(2, root.segmentCount)).toLocaleString(
                             Qt.locale(), "f", 0))
              : qsTr("Segments must be a whole number from 1 to 20.")
        color: root.segmentCountValid
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
