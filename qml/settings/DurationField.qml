import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

ColumnLayout {
    id: root
    objectName: fieldObjectName + "Editor"
    property string value: ""
    property alias label: editor.label
    property alias running: editor.running
    property alias fieldObjectName: editor.fieldObjectName
    property alias captureVisible: editor.captureVisible
    property alias captureEnabled: editor.captureEnabled
    property alias captureValue: editor.captureValue
    property alias captureToolTip: editor.captureToolTip
    property alias dragStep: editor.dragStep
    property alias minimum: editor.minimum
    property alias maximum: editor.maximum
    property alias liveScrub: editor.liveScrub
    property alias info: editor.info
    readonly property var parsed: ThemeControls.DurationDisplay.parse(value)
    readonly property string errorText: /^\d+$/.test(value) && parsed.valid
        ? "" : parsed.error || qsTr("Invalid millisecond value.")
    signal edited(string value)
    Layout.fillWidth: true
    spacing: 2

    SettingTextField {
        id: editor
        value: ThemeControls.DurationDisplay.format(root.value)
        dragStep: 10
        minimum: 0
        valueParser: function(text) {
            const parsed = ThemeControls.DurationDisplay.parse(text)
            return parsed.valid ? Number(parsed.milliseconds) : NaN
        }
        valueFormatter: function(value) {
            return ThemeControls.DurationDisplay.format(String(Math.round(value / 10) * 10))
        }
        onEdited: value => {
            const result = ThemeControls.DurationDisplay.parse(value)
            root.edited(result.valid ? result.milliseconds : value)
        }
    }
    Label {
        Layout.fillWidth: true
        visible: text.length > 0
        text: root.errorText
        color: ThemeControls.AppTheme.error
        wrapMode: Text.WordWrap
        font.pixelSize: 11
    }
}
