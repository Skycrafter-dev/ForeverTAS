import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

RowLayout {
    id: root

    property string label
    property string value
    property bool running: false
    property string fieldObjectName: ""
    property real dragStep: 1
    property bool liveScrub: true
    property var valueParser: null
    property var valueFormatter: null
    property int decimals: 0
    property bool integer: true
    property real minimum: -Number.MAX_VALUE
    property real maximum: Number.MAX_VALUE
    property bool captureVisible: false
    property bool captureEnabled: true
    property string captureValue: ""
    property string captureToolTip: qsTr("Set to now")
    readonly property bool scrubbable: true
    signal edited(string value)

    Layout.fillWidth: true
    spacing: 12

    Label {
        Layout.fillWidth: true
        text: root.label
        wrapMode: Text.WordWrap
    }

    ScrubNumberField {
        objectName: root.fieldObjectName
        Layout.preferredWidth: 126
        value: root.value
        enabled: !root.running
        dragStep: root.dragStep
        liveScrub: root.liveScrub
        valueParser: root.valueParser
        valueFormatter: root.valueFormatter
        decimals: root.decimals
        integer: root.integer
        minimum: root.minimum
        maximum: root.maximum
        onEdited: value => root.edited(value)
    }

    ThemeControls.ThemedIconButton {
        visible: root.captureVisible
        objectName: root.fieldObjectName + "CaptureButton"
        Layout.preferredWidth: 30
        Layout.preferredHeight: 30
        enabled: !root.running && root.captureEnabled
        icon.source: "qrc:/icons/clock.svg"
        icon.width: 16
        icon.height: 16
        ToolTip.visible: hovered
        ToolTip.text: root.captureToolTip
        Accessible.name: root.captureToolTip
        onClicked: root.edited(root.captureValue)
    }
}
