import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root
    objectName: "pointTargetEvaluationSettings"
    property var controller
    property var viewer
    property var viewport

    Layout.fillWidth: true
    spacing: 8

    TimeWindowSettings {
        settings: controller.evaluationTargetSettings
        updateSetting: controller.setEvaluationTargetSetting
        running: controller.running
        viewer: root.viewer
    }

    Vector3Settings {
        title: qsTr("Target point")
        settings: controller.evaluationTargetSettings
        updateSetting: controller.setEvaluationTargetSetting
        running: controller.running
    }

    RowLayout {
        Layout.fillWidth: true

        Label { text: qsTr("Move to") }
        ThemeControls.ThemedButton {
            objectName: "pointTargetCopyCarButton"
            Layout.fillWidth: true
            text: qsTr("Car")
            enabled: !root.controller.running && root.viewer
                     && root.viewer.loaded
            onClicked: root.setPoint(root.viewer.carPosition)
        }
        ThemeControls.ThemedButton {
            objectName: "pointTargetCopyCameraButton"
            Layout.fillWidth: true
            text: qsTr("Camera")
            enabled: !root.controller.running && root.viewport
            onClicked: root.setPoint(root.viewport.sceneCameraPosition)
        }
        ThemeControls.ThemedButton {
            objectName: "pointTargetPickButton"
            Layout.fillWidth: true
            text: qsTr("Pick in race")
            checkable: true
            checked: root.viewport && root.viewport.pointPlacementActive
            enabled: !root.controller.running && root.viewport
            onClicked: root.viewport.pointPlacementActive = checked
        }
    }

    function setPoint(position) {
        controller.setEvaluationTargetSetting("x", String(position.x))
        controller.setEvaluationTargetSetting("y", String(position.y))
        controller.setEvaluationTargetSetting("z", String(position.z))
    }
}
