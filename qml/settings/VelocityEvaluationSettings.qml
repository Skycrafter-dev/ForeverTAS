import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root
    objectName: "velocityEvaluationSettings"
    property var controller
    property var viewer
    property var viewport
    readonly property var settings: controller.evaluationTargetSettings
    readonly property bool towardsPoint:
        (settings["directionMode"] ?? "fixed") === "point"

    Layout.fillWidth: true
    spacing: 8

    TimeWindowSettings {
        chainController: root.controller
        chainKey: "target"
        settings: parent.settings
        updateSetting: controller.setEvaluationTargetSetting
        running: controller.running
        viewer: root.viewer
    }

    SettingCombo {
        comboObjectName: "velocityModeCombo"
        label: qsTr("Velocity objective")
        options: [
            { label: qsTr("Total speed"), value: "total" },
            { label: qsTr("Projected direction"), value: "projected" }
        ]
        value: parent.settings["mode"] ?? "total"
        running: controller.running
        onSelected: value =>
            controller.setEvaluationTargetSetting("mode", value)
    }

    SettingSwitch {
        label: qsTr("Require direction alignment")
        checked: (parent.settings["alignmentEnabled"] ?? "false")
                 === "true"
        running: controller.running
        onToggled: checked =>
            controller.setEvaluationTargetSetting(
                "alignmentEnabled", checked ? "true" : "false")
    }

    SettingCombo {
        comboObjectName: "velocityDirectionModeCombo"
        label: qsTr("Direction")
        options: [
            { label: qsTr("Fixed"), value: "fixed" },
            { label: qsTr("Towards a point"), value: "point" }
        ]
        value: parent.settings["directionMode"] ?? "fixed"
        running: controller.running
        onSelected: value =>
            controller.setEvaluationTargetSetting("directionMode", value)
    }

    Vector3Settings {
        objectName: "velocityPointSettings"
        visible: root.towardsPoint
        title: qsTr("Point")
        settings: parent.settings
        updateSetting: controller.setEvaluationTargetSetting
        running: controller.running
    }

    RowLayout {
        Layout.fillWidth: true
        visible: root.towardsPoint

        Label { text: qsTr("Move to") }
        ThemeControls.ThemedButton {
            Layout.fillWidth: true
            text: qsTr("Car")
            enabled: !root.controller.running && root.viewer
                     && root.viewer.loaded
            onClicked: root.setPoint(root.viewer.carPosition)
        }
        ThemeControls.ThemedButton {
            Layout.fillWidth: true
            text: qsTr("Camera")
            enabled: !root.controller.running && root.viewport
            onClicked: root.setPoint(root.viewport.sceneCameraPosition)
        }
        ThemeControls.ThemedButton {
            objectName: "velocityPointPickButton"
            Layout.fillWidth: true
            text: qsTr("Pick in race")
            checkable: true
            checked: root.viewport && root.viewport.pointPlacementActive
            enabled: !root.controller.running && root.viewport
            onClicked: root.viewport.pointPlacementActive = checked
        }
    }

    Vector3Settings {
        visible: !root.towardsPoint
        title: qsTr("Direction vector")
        settings: parent.settings
        updateSetting: controller.setEvaluationTargetSetting
        running: controller.running
        xKey: "directionX"
        yKey: "directionY"
        zKey: "directionZ"
    }

    RowLayout {
        Layout.fillWidth: true
        visible: !root.towardsPoint
        ThemeControls.ThemedButton {
            objectName: "velocityCopyTravelButton"
            Layout.fillWidth: true
            text: qsTr("Car travel")
            enabled: !root.controller.running && root.viewer
                     && root.viewer.carVelocity.length() > 0.000001
            onClicked: root.setDirection(root.viewer.carVelocity)
        }
        ThemeControls.ThemedButton {
            objectName: "velocityCopyHeadingButton"
            Layout.fillWidth: true
            text: qsTr("Car heading")
            enabled: !root.controller.running && root.viewer
                     && root.viewer.loaded
            onClicked: root.setDirection(root.viewer.carHeading)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: !root.towardsPoint
        Repeater {
            model: [
                { label: qsTr("+X"), x: 1, y: 0, z: 0 },
                { label: qsTr("-X"), x: -1, y: 0, z: 0 },
                { label: qsTr("+Z"), x: 0, y: 0, z: 1 },
                { label: qsTr("-Z"), x: 0, y: 0, z: -1 }
            ]
            delegate: ThemeControls.ThemedButton {
                required property var modelData
                Layout.fillWidth: true
                text: modelData.label
                enabled: !root.controller.running
                onClicked: root.setDirection(Qt.vector3d(
                    modelData.x, modelData.y, modelData.z))
            }
        }
    }

    function setPoint(position) {
        controller.setEvaluationTargetSetting("x", String(position.x))
        controller.setEvaluationTargetSetting("y", String(position.y))
        controller.setEvaluationTargetSetting("z", String(position.z))
    }

    function setDirection(direction) {
        const length = direction.length()
        if (length <= 0.000001)
            return
        controller.setEvaluationTargetSetting(
            "directionX", String(direction.x / length))
        controller.setEvaluationTargetSetting(
            "directionY", String(direction.y / length))
        controller.setEvaluationTargetSetting(
            "directionZ", String(direction.z / length))
    }

    SettingSlider {
        sliderObjectName: "minimumAlignmentSlider"
        label: qsTr("Minimum alignment (%)")
        value: parent.settings["minAlignmentPercent"] ?? ""
        running: controller.running
        from: -100
        to: 100
        stepSize: 1
        suffix: "%"
        onEdited: value =>
            controller.setEvaluationTargetSetting(
                "minAlignmentPercent", value)
    }
}
