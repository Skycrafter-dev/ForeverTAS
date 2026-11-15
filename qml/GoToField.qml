import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// "Go to": where on the race's timeline, typed as it comes to mind (30,
// 1:02.35, +2, -0.5, cp 3, next, finish, 50%, tick 120). Enter goes there;
// a place it can't find says why under the field.
ColumnLayout {
    id: root
    required property var raceControls
    property alias field: input
    property string error: ""
    // Went there (the box over the race view then closes).
    signal went()
    spacing: 2

    TextField {
        id: input
        objectName: "goToField"
        Layout.fillWidth: true
        placeholderText: qsTr("Go to: 30, 1:02.35, +2, cp 3, finish...")
        selectByMouse: true
        onTextEdited: root.error = ""
        onAccepted: {
            root.error = root.raceControls.goTo(text)
            if (!root.error.length) {
                selectAll()
                root.went()
            }
        }
        ToolTip.visible: hovered && !activeFocus
        ToolTip.delay: 600
        ToolTip.text: qsTr("A time (30, 1:02.35, 1500ms), from here (+2, -0.5), a checkpoint (cp 3, next, prev, finish), a share of the race (50%) or a tick (tick 120). Enter goes there.")
    }
    Label {
        objectName: "goToError"
        Layout.fillWidth: true
        visible: root.error.length > 0
        text: root.error
        color: AppTheme.error
        wrapMode: Text.Wrap
        font.pixelSize: 11
    }
}
