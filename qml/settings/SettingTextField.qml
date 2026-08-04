import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: root

    property string label
    property string value
    property bool running: false
    property string fieldObjectName: ""
    property real dragStep: 1
    property bool liveScrub: true
    property int decimals: 0
    property bool integer: true
    property real minimum: -Number.MAX_VALUE
    property real maximum: Number.MAX_VALUE
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
        decimals: root.decimals
        integer: root.integer
        minimum: root.minimum
        maximum: root.maximum
        onEdited: value => root.edited(value)
    }
}
