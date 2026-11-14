import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

// The Gates target: an ordered list of gates, one per line (stored with '|'
// between them). Earlier gates are waypoints; the last one is scored.
ColumnLayout {
    id: root

    objectName: "gateEntryEvaluationSettings"
    property var controller
    property var viewer
    readonly property var settings: controller.evaluationTargetSettings
    readonly property string storedGates: settings["gates"] ?? ""

    Layout.fillWidth: true
    spacing: 8

    function editedGates() {
        return gatesArea.text.split("\n")
                       .map(line => line.trim())
                       .filter(line => line.length > 0)
                       .join("|")
    }
    // The text takes the stored gates only while it is not being edited, so
    // a save never moves the cursor or rewrites what is typed.
    function showStoredGates() {
        if (!gatesArea.activeFocus && editedGates() !== storedGates)
            gatesArea.text = storedGates.split("|").join("\n")
    }
    function commitGates() {
        commitTimer.stop()
        const joined = editedGates()
        if (joined !== storedGates)
            controller.setEvaluationTargetSetting("gates", joined)
    }
    onStoredGatesChanged: showStoredGates()
    Component.onCompleted: showStoredGates()

    // Saved a moment after typing stops (each save is validated and undoable).
    Timer {
        id: commitTimer
        interval: 400
        onTriggered: root.commitGates()
    }

    RowLayout {
        Layout.fillWidth: true

        Label {
            Layout.fillWidth: true
            text: qsTr("Gates, in order: the last one is scored")
            font.weight: Font.Medium
            wrapMode: Text.WordWrap
        }

        InfoTip {
            info: qsTr("One gate per line: x=, y=, z= and dir= (its heading in degrees: 0 = +Z, 90 = +X), and optionally hw= or lmin=/lmax= (metres across, + = left), ymin=/ymax= (metres up), vmin=/vmax= (km/h), yawmin=/yawmax= (degrees) and wheels= (0 to 4). The car must cross each earlier gate in order and within its limits; the time it crosses the last one is what the search lowers. Runs on the CPU backends.")
        }
    }

    ScrollView {
        id: gatesScroll
        Layout.fillWidth: true
        Layout.preferredHeight: 120
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AsNeeded
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        TextArea {
            id: gatesArea
            objectName: "gateEntryTextArea"
            width: Math.max(gatesScroll.availableWidth,
                            contentWidth + leftPadding + rightPadding)
            enabled: !root.controller.running
            selectByMouse: true
            wrapMode: TextEdit.NoWrap
            textFormat: TextEdit.PlainText
            font.family: "monospace"
            font.pixelSize: 12
            color: enabled ? ThemeControls.AppTheme.text
                           : ThemeControls.AppTheme.disabledText
            placeholderText: "x=0,y=0,z=0,dir=0,hw=8"
            onTextChanged: if (activeFocus) commitTimer.restart()
            onActiveFocusChanged: {
                if (!activeFocus) {
                    root.commitGates()
                    root.showStoredGates()
                }
            }
            background: Rectangle {
                color: parent.enabled ? ThemeControls.AppTheme.surface
                                      : ThemeControls.AppTheme.disabledSurface
                border.width: 1
                border.color: parent.activeFocus
                              ? ThemeControls.AppTheme.focus
                              : ThemeControls.AppTheme.border
                radius: 6
            }
        }
    }

    SettingSwitch {
        label: qsTr("Rank misses by how close they came")
        checked: (root.settings["soft"] ?? "true") === "true"
        running: root.controller.running
        onToggled: checked =>
            root.controller.setEvaluationTargetSetting("soft", checked ? "true" : "false")
    }

    SettingTextField {
        fieldObjectName: "gateYawWeightField"
        label: qsTr("Miss penalty per degree (m)")
        value: root.settings["yawWeight"] ?? "0.5"
        running: root.controller.running
        integer: false
        decimals: 2
        dragStep: 0.05
        minimum: 0
        onEdited: value => root.controller.setEvaluationTargetSetting("yawWeight", value)
    }

    SettingTextField {
        fieldObjectName: "gateSpeedWeightField"
        label: qsTr("Miss penalty per km/h (m)")
        value: root.settings["speedWeight"] ?? "0.5"
        running: root.controller.running
        integer: false
        decimals: 2
        dragStep: 0.05
        minimum: 0
        onEdited: value => root.controller.setEvaluationTargetSetting("speedWeight", value)
    }
}
