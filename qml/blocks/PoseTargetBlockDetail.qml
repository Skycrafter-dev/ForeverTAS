import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

// Detail panel for the distance-to-pose evaluation block: the persistent
// pose collection acts as a picker; the selected pose's values are copied
// into the block's own fields (position and rotation), and stay in sync
// while the pose is edited. The block owns its values, so the text
// interchange and reporters keep working.
ColumnLayout {
    id: root

    objectName: "poseTargetBlockDetail"
    property var controller
    property var viewer
    property var viewport
    property int blockId: 0
    property var blockInformation: null
    readonly property var targets: controller.poseTargets
    readonly property var selected: targets.selectedTarget
    readonly property var settings: {
        const values = {}
        if (!blockInformation)
            return values
        for (let index = 0; index < blockInformation.fields.length;
             ++index) {
            values[blockInformation.fields[index].key] =
                    blockInformation.fields[index].value
        }
        return values
    }

    Layout.fillWidth: true
    spacing: 8

    function applySelectedPose() {
        const target = root.targets.selectedTarget
        if (!target || !root.controller)
            return
        root.controller.setBlockField(root.blockId, "x",
                                      String(target.x))
        root.controller.setBlockField(root.blockId, "y",
                                      String(target.y))
        root.controller.setBlockField(root.blockId, "z",
                                      String(target.z))
        root.controller.setBlockField(root.blockId, "yawDegrees",
                                      String(target.yawDegrees))
        root.controller.setBlockField(root.blockId, "pitchDegrees",
                                      String(target.pitchDegrees))
        root.controller.setBlockField(root.blockId, "rollDegrees",
                                      String(target.rollDegrees))
    }

    Component.onCompleted: applySelectedPose()

    Connections {
        target: root.controller

        function onBlockUpdated(updatedBlockId) {
            if (updatedBlockId === root.blockId)
                root.blockInformation = root.controller.blockData(
                            root.blockId)
        }
    }

    Connections {
        target: root.targets

        function onSelectedTargetChanged() {
            root.applySelectedPose()
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        StyledComboBox {
            id: targetSelector

            objectName: "poseTargetSelector"
            Layout.fillWidth: true
            model: root.targets.targets
            textRole: "name"
            valueRole: "id"
            currentIndex: root.targets.selectedIndex
            enabled: !root.controller.running
            onActivated: index => root.targets.selectTarget(index)
        }

        ThemeControls.ThemedButton {
            objectName: "addPoseTargetButton"
            text: "+"
            Layout.preferredWidth: 38
            enabled: !root.controller.running
                     && root.targets.count < root.targets.maximumCount
            onClicked: {
                const position = root.viewer && root.viewer.loaded
                                 ? root.viewer.carPosition
                                 : root.selected.position
                const rotation = root.viewer && root.viewer.loaded
                                 ? root.viewer.carRotation
                                 : root.selected.rotation
                root.targets.addTarget(
                    position.x, position.y, position.z, rotation)
            }
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Add car pose")
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        ThemeControls.ThemedButton {
            objectName: "duplicatePoseTargetButton"
            Layout.fillWidth: true
            text: qsTr("Duplicate")
            enabled: !root.controller.running
                     && root.targets.count < root.targets.maximumCount
            onClicked: root.targets.duplicateSelected()
        }

        ThemeControls.ThemedButton {
            objectName: "focusPoseTargetButton"
            Layout.fillWidth: true
            text: qsTr("Focus")
            enabled: root.targets.count > 0
            onClicked: root.controller.focusSelectedPoseTarget()
        }

        ThemeControls.ThemedButton {
            objectName: "removePoseTargetButton"
            Layout.fillWidth: true
            text: qsTr("Remove")
            enabled: !root.controller.running && root.targets.count > 1
            onClicked: root.targets.removeTarget(root.targets.selectedIndex)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        Label {
            text: qsTr("Move to")
            color: ThemeControls.AppTheme.textMuted
        }

        ThemeControls.ThemedButton {
            objectName: "movePoseTargetToCameraButton"
            Layout.fillWidth: true
            text: qsTr("Camera")
            enabled: !root.controller.running && root.viewport
            onClicked: {
                const position = root.viewport.sceneCameraPosition
                root.targets.moveSelectedTo(
                    position.x,
                    position.y,
                    position.z,
                    root.viewport.sceneCameraRotation)
            }
        }

        ThemeControls.ThemedButton {
            objectName: "movePoseTargetToCarButton"
            Layout.fillWidth: true
            text: qsTr("Car")
            enabled: !root.controller.running
                     && root.viewer && root.viewer.loaded
            onClicked: {
                const position = root.viewer.carPosition
                root.targets.moveSelectedTo(
                    position.x,
                    position.y,
                    position.z,
                    root.viewer.carRotation)
            }
        }
    }

    TextField {
        objectName: "poseTargetNameField"
        Layout.fillWidth: true
        text: root.selected.name ?? ""
        enabled: !root.controller.running
        selectByMouse: true
        maximumLength: 80
        placeholderText: qsTr("Pose target name")
        onEditingFinished: {
            root.targets.setName(root.targets.selectedIndex, text)
            text = root.targets.selectedTarget.name
        }
    }

    Vector3Settings {
        objectName: "poseTargetPositionSettings"
        title: qsTr("Target position")
        settings: root.selected
        running: root.controller.running
        minimum: -10000000
        maximum: 10000000
        updateSetting: (key, value) =>
            root.targets.setPositionComponent(
                root.targets.selectedIndex, key, value)
    }

    ColumnLayout {
        objectName: "poseTargetRotationSettings"
        Layout.fillWidth: true
        spacing: 4

        Label {
            text: qsTr("Target rotation (degrees)")
            font.weight: Font.Medium
        }

        SettingTextField {
            label: qsTr("Yaw")
            value: root.selected.yawDegrees ?? ""
            running: root.controller.running
            integer: false
            decimals: 3
            dragStep: 1
            minimum: -10000000
            maximum: 10000000
            onEdited: value =>
                root.targets.setRotationComponent(
                    root.targets.selectedIndex, "yaw", value)
        }

        SettingTextField {
            label: qsTr("Pitch")
            value: root.selected.pitchDegrees ?? ""
            running: root.controller.running
            integer: false
            decimals: 3
            dragStep: 1
            minimum: -10000000
            maximum: 10000000
            onEdited: value =>
                root.targets.setRotationComponent(
                    root.targets.selectedIndex, "pitch", value)
        }

        SettingTextField {
            label: qsTr("Roll")
            value: root.selected.rollDegrees ?? ""
            running: root.controller.running
            integer: false
            decimals: 3
            dragStep: 1
            minimum: -10000000
            maximum: 10000000
            onEdited: value =>
                root.targets.setRotationComponent(
                    root.targets.selectedIndex, "roll", value)
        }
    }

    TimeWindowSettings {
        settings: root.settings
        updateSetting: (key, value) =>
            root.controller.setBlockField(root.blockId, key, value)
        running: root.controller.running
    }

    SettingSlider {
        sliderObjectName: "rotationWeightSlider"
        label: qsTr("Rotation weight (%)")
        value: root.settings["rotationWeightPercent"] ?? ""
        running: root.controller.running
        from: 0
        to: 100
        stepSize: 1
        suffix: "%"
        onEdited: value =>
            root.controller.setBlockField(
                root.blockId, "rotationWeightPercent", value)
    }
}
