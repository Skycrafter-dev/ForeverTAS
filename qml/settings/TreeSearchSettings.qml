import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root

    objectName: "treeSearchSettings"
    property var controller
    readonly property var settings: controller.searchAlgorithmSettings
    property bool showAdvanced: false

    function wholeNumber(key) {
        const parsed = parseInt(root.settings[key] ?? "", 10)
        return isNaN(parsed) ? -1 : parsed
    }

    readonly property int segmentCount: root.wholeNumber("segmentCount")
    readonly property int leafCount: root.wholeNumber("leafCount")
    readonly property int branchedSegmentCount:
        root.wholeNumber("branchedSegmentCount")
    readonly property int migrationSeconds:
        root.wholeNumber("migrationSeconds")
    readonly property bool keepOtherSegments:
        root.settings["unbranchedSegments"] === "keep"
    readonly property bool settingsValid:
        root.segmentCount >= 1 && root.segmentCount <= 20 &&
        root.branchedSegmentCount >= 0 &&
        root.branchedSegmentCount <= root.segmentCount &&
        root.migrationSeconds >= 0

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
        fieldObjectName: "treeBranchedSegmentCountField"
        label: qsTr("Segments changed per attempt (0 = all)")
        value: root.settings["branchedSegmentCount"] ?? ""
        running: root.controller.running
        minimum: 0
        maximum: 20
        onEdited: value =>
            root.controller.setSearchAlgorithmSetting(
                "branchedSegmentCount", value)
    }

    SettingCombo {
        comboObjectName: "treeUnbranchedSegmentsCombo"
        label: qsTr("Other segments")
        value: root.settings["unbranchedSegments"] ?? "keep"
        options: [
            { "label": qsTr("Keep the current best"), "value": "keep" },
            { "label": qsTr("Redraw once per tree"), "value": "draw" }
        ]
        running: root.controller.running
        onSelected: value =>
            root.controller.setSearchAlgorithmSetting(
                "unbranchedSegments", value)
    }

    SettingTextField {
        fieldObjectName: "treeMigrationSecondsField"
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
        text: {
            if (!root.settingsValid) {
                return qsTr("Use 1 to 20 segments, at most that many changed "
                            + "segments, and whole seconds of migration.")
            }
            const changed = root.branchedSegmentCount === 0
                    ? root.segmentCount
                    : root.branchedSegmentCount
            const shape = root.keepOtherSegments
                    ? qsTr("Local search: each attempt changes %1 of %2 "
                           + "segments and keeps the others at the current "
                           + "best.").arg(changed).arg(root.segmentCount)
                    : qsTr("Each tree shares simulated prefixes across up "
                           + "to %1 attempts.")
                          .arg((root.leafCount > 0
                                ? root.leafCount
                                : Math.pow(2, root.segmentCount))
                               .toLocaleString(Qt.locale(), "f", 0))
            const islands = root.migrationSeconds > 0
                    ? qsTr(" With several CPU workers, each follows its own "
                           + "improvements for %1 s before adopting the "
                           + "shared best.").arg(root.migrationSeconds)
                    : qsTr(" CPU workers adopt every new best at once.")
            return shape + islands
        }
        color: root.settingsValid
               ? ThemeControls.AppTheme.textMuted
               : ThemeControls.AppTheme.error
        wrapMode: Text.WordWrap
        font.pixelSize: 11
    }

    SettingSwitch {
        objectName: "treeAdvancedSwitch"
        label: qsTr("Show advanced tree options")
        checked: root.showAdvanced
        running: false
        onToggled: checked => root.showAdvanced = checked
    }

    ColumnLayout {
        objectName: "treeAdvancedOptions"
        Layout.fillWidth: true
        visible: root.showAdvanced
        spacing: 8

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

        SettingSwitch {
            objectName: "treeVaryBranchedSegmentCountSwitch"
            label: qsTr("Change a random number of segments up to that")
            checked: root.settings["varyBranchedSegmentCount"] === "true"
            running: root.controller.running
            onToggled: checked =>
                root.controller.setSearchAlgorithmSetting(
                    "varyBranchedSegmentCount", checked ? "true" : "false")
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
