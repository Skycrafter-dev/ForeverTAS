import QtQuick
import QtQuick.Layouts

ColumnLayout {
    objectName: "stuntPointsEvaluationSettings"
    property var controller
    property var viewer
    readonly property var settings: controller.evaluationTargetSettings

    Layout.fillWidth: true
    spacing: 8

    SettingTextField {
        fieldObjectName: "stuntPointsTimeField"
        label: qsTr("Target time (ms)")
        value: parent.settings["targetTimeMs"] ?? ""
        running: controller.running
        dragStep: 10
        minimum: 0
        captureVisible: true
        captureEnabled: viewer && viewer.loaded
        captureValue: viewer
                      ? String(viewer.currentTick * viewer.tickDurationMs)
                      : ""
        captureToolTip: qsTr("Set deadline to now (10 ms tick)")
        onEdited: value =>
            controller.setEvaluationTargetSetting("targetTimeMs", value)
    }
}
