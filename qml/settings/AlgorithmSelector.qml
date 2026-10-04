import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root

    property string title
    property string comboObjectName
    property var options: []
    property string selectedId
    property var controller
    property var viewer
    property var viewport
    property bool iconMode: false
    readonly property var selectedOption:
        optionCombo.currentIndex >= 0
        && optionCombo.currentIndex < options.length
        ? options[optionCombo.currentIndex]
        : null
    readonly property bool settingsLoaded:
        settingsLoader.status === Loader.Ready
        && settingsLoader.item !== null
    readonly property var settingsItem:
        settingsLoaded ? settingsLoader.item : null
    readonly property string settingsObjectName:
        settingsLoaded ? settingsLoader.item.objectName : ""

    signal selectionRequested(string id)

    spacing: 6

    function optionIndex(id) {
        for (let index = 0; index < options.length; ++index) {
            if (options[index].id === id)
                return index
        }
        return -1
    }

    Label {
        visible: root.title.length > 0
        text: root.title
        font.weight: Font.Medium
    }

    StyledComboBox {
        id: optionCombo
        objectName: root.comboObjectName
        Layout.fillWidth: true
        visible: !root.iconMode
        model: root.options
        textRole: "label"
        valueRole: "id"
        currentIndex: root.optionIndex(root.selectedId)
        enabled: !root.controller.running
        onActivated: selectedIndex =>
            root.selectionRequested(valueAt(selectedIndex).toString())
    }

    GridLayout {
        id: targetGrid
        objectName: "evaluationTargetIconRow"
        visible: root.iconMode
        Layout.fillWidth: true
        columns: root.width >= 300 ? 4 : 3
        columnSpacing: 4
        rowSpacing: 4

        Repeater {
            model: root.options.length

            delegate: Button {
                readonly property bool themedControl: true
                required property int index
                readonly property var modelData: root.options[index]
                readonly property string iconName: {
                    const icons = {
                        "velocity": "gauge",
                        "stunt-points": "trophy",
                        "precise-finish-time": "flag",
                        "checkpoint-time": "clock",
                        "time": "timer",
                        "volume-entry-time": "box",
                        "custom-volume-entry-time": "pentagon",
                        "point-target": "crosshair",
                        "pose-target": "axis-3d",
                        "scripted-target": "crosshair"
                    }
                    return icons[modelData.id] ?? "crosshair"
                }
                readonly property string shortLabel: {
                    const labels = {
                        "velocity": qsTr("Velocity"),
                        "stunt-points": qsTr("Stunts"),
                        "precise-finish-time": qsTr("Finish"),
                        "checkpoint-time": qsTr("CP time"),
                        "time": qsTr("Time"),
                        "volume-entry-time": qsTr("Box entry"),
                        "custom-volume-entry-time": qsTr("Poly entry"),
                        "point-target": qsTr("Point"),
                        "pose-target": qsTr("Pose"),
                        "scripted-target": qsTr("Custom")
                    }
                    return labels[modelData.id] ?? modelData.label
                }

                objectName: "evaluationTargetIcon" + index
                Layout.fillWidth: true
                Layout.preferredWidth:
                    (root.width - (targetGrid.columns - 1)
                     * targetGrid.columnSpacing) / targetGrid.columns
                Layout.preferredHeight: 50
                enabled: !root.controller.running
                checkable: true
                checked: root.selectedId === modelData.id
                text: shortLabel
                font.pixelSize: 11
                font.weight: checked ? Font.DemiBold : Font.Normal
                icon.source: "qrc:/icons/" + iconName + ".svg"
                icon.width: 18
                icon.height: 18
                icon.color: checked ? ThemeControls.AppTheme.textOnAccent
                                    : ThemeControls.AppTheme.text
                display: AbstractButton.TextUnderIcon
                spacing: 2
                leftPadding: 2
                rightPadding: 2
                topPadding: 4
                bottomPadding: 3
                palette.buttonText: checked
                    ? ThemeControls.AppTheme.textOnAccent
                    : ThemeControls.AppTheme.text
                ToolTip.visible: hovered
                ToolTip.text: modelData.label
                Accessible.name: modelData.label
                onClicked: root.selectionRequested(modelData.id)

                background: Rectangle {
                    radius: 5
                    color: parent.checked
                           ? ThemeControls.AppTheme.accent
                           : parent.hovered
                             ? ThemeControls.AppTheme.controlHover
                             : ThemeControls.AppTheme.surface
                    border.width: 1
                    border.color: parent.checked
                                  ? ThemeControls.AppTheme.accentBorder
                                  : ThemeControls.AppTheme.border
                }
            }
        }
    }

    Label {
        visible: root.iconMode && root.selectedOption !== null
        Layout.fillWidth: true
        text: root.selectedOption ? root.selectedOption.label : ""
        color: ThemeControls.AppTheme.textMuted
        font.pixelSize: 11
        wrapMode: Text.WordWrap
    }

    Loader {
        id: settingsLoader
        objectName: root.comboObjectName + "SettingsLoader"
        Layout.fillWidth: true
        Layout.preferredHeight: settingsLoaded
                                ? settingsLoader.item.implicitHeight : 0
        visible: active
        active: root.selectedOption !== null
                && root.selectedOption.settingsComponent.length > 0
        source: active ? root.selectedOption.settingsComponent : ""
        onLoaded: {
            if (item) {
                item.controller = root.controller
                if ("viewer" in item)
                    item.viewer = root.viewer
                if ("viewport" in item)
                    item.viewport = root.viewport
            }
        }
    }
}
