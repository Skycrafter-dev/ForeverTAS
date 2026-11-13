import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root

    property var settings: ({})
    property var updateSetting
    property bool running: false
    property var viewer
    property string minimumKey: "minTimeMs"
    property string maximumKey: "maxTimeMs"
    property string minimumLabel: qsTr("Minimum time")
    property string maximumLabel: qsTr("Maximum time")
    // Window chains: which window this is ("pass:0", "target") and the
    // controller holding the chains; no chain button without them.
    property var chainController: null
    property string chainKey: ""

    Layout.fillWidth: true
    spacing: 6

    RowLayout {
        Layout.fillWidth: true
        visible: root.chainKey.length > 0 && !!root.chainController
        Label {
            text: qsTr("Time window")
            color: ThemeControls.AppTheme.textMuted
            font.pixelSize: 11
        }
        Item { Layout.fillWidth: true }
        WindowChainButton {
            controller: root.chainController
            chainKey: root.chainKey
            label: qsTr("Time window")
            running: root.running
        }
    }

    SettingCombo {
        comboObjectName: "timeWindowEndMode"
        label: qsTr("End")
        options: [ { label: qsTr("At time"), value: "fixed" },
                   { label: qsTr("Until horizon"), value: "horizon" } ]
        value: root.settings.maxTimeMode ?? "fixed"
        running: root.running
        onSelected: value => root.updateSetting("maxTimeMode", value)
    }

    DurationField {
        fieldObjectName: "minimumTimeField"
        label: root.minimumLabel
        value: root.settings[root.minimumKey] ?? ""
        running: root.running
        dragStep: 10
        minimum: 0
        captureVisible: root.viewer !== null && root.viewer !== undefined
        captureEnabled: !!root.viewer && root.viewer.loaded
        captureValue: root.viewer
                      ? String(root.viewer.currentTick
                               * root.viewer.tickDurationMs) : ""
        captureToolTip: qsTr("Set start to now (10 ms tick)")
        onEdited: value => root.updateSetting(root.minimumKey, value)
    }

    DurationField {
        fieldObjectName: "maximumTimeField"
        visible: (root.settings.maxTimeMode ?? "fixed") === "fixed"
        label: root.maximumLabel
        value: root.settings[root.maximumKey] ?? ""
        running: root.running
        dragStep: 10
        minimum: 0
        captureVisible: root.viewer !== null && root.viewer !== undefined
        captureEnabled: !!root.viewer && root.viewer.loaded
        captureValue: root.viewer
                      ? String(root.viewer.currentTick
                               * root.viewer.tickDurationMs) : ""
        captureToolTip: qsTr("Set end to now (10 ms tick)")
        onEdited: value => root.updateSetting(root.maximumKey, value)
    }

    RowLayout {
        visible: root.viewer !== null && root.viewer !== undefined
        Layout.fillWidth: true
        Item { Layout.fillWidth: true }
        ThemeControls.ThemedButton {
            objectName: "timeWindowFullRunButton"
            text: qsTr("Full run")
            enabled: !root.running && !!root.viewer && root.viewer.loaded
            onClicked: {
                root.updateSetting("maxTimeMode", "fixed")
                root.updateSetting(root.minimumKey, "0")
                root.updateSetting(root.maximumKey,
                                   String(Math.floor(
                                       root.viewer.durationMs
                                       / root.viewer.tickDurationMs)
                                       * root.viewer.tickDurationMs))
            }
        }
        ThemeControls.ThemedButton {
            objectName: "timeWindowPlayheadToEndButton"
            text: qsTr("Now to end")
            enabled: !root.running && !!root.viewer && root.viewer.loaded
            onClicked: {
                root.updateSetting("maxTimeMode", "fixed")
                root.updateSetting(root.minimumKey,
                                   String(root.viewer.currentTick
                                          * root.viewer.tickDurationMs))
                root.updateSetting(root.maximumKey,
                                   String(Math.floor(
                                       root.viewer.durationMs
                                       / root.viewer.tickDurationMs)
                                       * root.viewer.tickDurationMs))
            }
        }
    }
}
