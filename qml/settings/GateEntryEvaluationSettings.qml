import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root

    objectName: "gateEntryEvaluationSettings"
    property var controller
    property var viewer
    readonly property var settings: controller.evaluationTargetSettings
    // Stored with '|' between gates; edited one gate per line.
    readonly property string storedGates: settings["gates"] ?? ""

    Layout.fillWidth: true
    spacing: 8

    RowLayout {
        Layout.fillWidth: true

        Label {
            Layout.fillWidth: true
            text: qsTr("Gates (in order; the last one is scored)")
            font.weight: Font.Medium
        }

        ThemeControls.ThemedToolButton {
            text: "?"
            Accessible.name: qsTr("Gate syntax")
            ToolTip.visible: hovered
            ToolTip.text: qsTr("One gate per line: x=,y=,z=,dir= (heading in degrees, 0 = +Z, 90 = +X) and optional hw= or lmin=/lmax= (lateral metres, + = left), ymin=/ymax=, vmin=/vmax= (km/h), yawmin=/yawmax= (degrees), wheels= (0-4). Earlier gates are waypoints that must be passed within their limits; the last gate's interpolated crossing time is minimised.")
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
            text: root.storedGates.split("|").join("\n")
            enabled: !root.controller.running
            selectByMouse: true
            wrapMode: TextEdit.NoWrap
            textFormat: TextEdit.PlainText
            font.family: "monospace"
            font.pixelSize: 12
            color: enabled ? ThemeControls.AppTheme.text
                           : ThemeControls.AppTheme.disabledText
            placeholderText: "x=0,y=0,z=0,dir=0,hw=8"
            onTextChanged: {
                const joined = text.split("\n")
                                   .map(line => line.trim())
                                   .filter(line => line.length > 0)
                                   .join("|")
                if (joined !== root.storedGates)
                    root.controller.setEvaluationTargetSetting("gates", joined)
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
        label: qsTr("Soft score (rank misses by how close they came)")
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

    Label {
        Layout.fillWidth: true
        visible: root.controller.validationMessage.length > 0
                 && root.controller.validationMessage.toLowerCase().indexOf("gate") >= 0
        text: root.controller.validationMessage
        color: ThemeControls.AppTheme.error
        wrapMode: Text.WordWrap
        font.pixelSize: 11
    }
}
