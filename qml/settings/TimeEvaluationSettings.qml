import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root
    objectName: "timeEvaluationSettings"
    property var controller
    property var viewer
    readonly property var settings: controller.evaluationTargetSettings

    Layout.fillWidth: true
    spacing: 8

    Label {
        Layout.fillWidth: true
        text: qsTr("Finds the first moment when all your Conditions are true, "
                   + "then makes that moment happen as early or as late as "
                   + "possible. Set the conditions on the Search tab.")
        color: ThemeControls.AppTheme.textMuted
        wrapMode: Text.WordWrap
        font.pixelSize: 12
    }

    SettingCombo {
        comboObjectName: "timeGoalCombo"
        label: qsTr("Goal")
        options: [
            { label: qsTr("As early as possible"), value: "earliest" },
            { label: qsTr("As late as possible"), value: "latest" }
        ]
        value: root.settings["goal"] ?? "earliest"
        running: root.controller.running
        onSelected: value => root.controller.setEvaluationTargetSetting("goal", value)
    }

    TimeWindowSettings {
        settings: root.settings
        updateSetting: root.controller.setEvaluationTargetSetting
        running: root.controller.running
        viewer: root.viewer
    }

    Label {
        objectName: "timeNeedsConditionsHint"
        Layout.fillWidth: true
        visible: root.controller.conditionScript.trim().length === 0
        text: qsTr("No conditions yet. Add at least one on the Search tab, "
                   + "for example car.cps >= 2.")
        color: ThemeControls.AppTheme.warning
        wrapMode: Text.WordWrap
        font.pixelSize: 12
    }
}
