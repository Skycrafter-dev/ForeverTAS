import QtQuick
import QtQuick.Controls

MenuItem {
    id: control

    readonly property bool themedControl: true
    // Shown on the right; the shortcut itself is defined elsewhere.
    property string shortcutText: ""
    // A check mark that follows state owned elsewhere (unlike `checked`,
    // which a click would overwrite).
    property bool marked: false
    property bool showMark: false
    readonly property color effectiveBackgroundColor:
        !enabled ? "transparent"
        : down ? Qt.rgba(0.27, 0.72, 0.47, 0.42)
        : highlighted || hovered ? Qt.rgba(0.27, 0.72, 0.47, 0.30) : "transparent"
    readonly property color effectiveTextColor:
        !enabled ? AppTheme.disabledText : AppTheme.text

    hoverEnabled: true
    implicitHeight: 30
    leftPadding: 30
    rightPadding: 12

    contentItem: Item {
        implicitWidth: label.implicitWidth + (shortcut.text.length > 0 ? shortcut.implicitWidth + 28 : 0)
        implicitHeight: label.implicitHeight
        Label {
            id: label
            anchors.left: parent.left
            anchors.right: shortcut.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font: control.font
            color: control.effectiveTextColor
            elide: Text.ElideRight
        }
        Label {
            id: shortcut
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: control.shortcutText
            color: control.enabled ? AppTheme.textMuted : AppTheme.disabledText
            font.pixelSize: control.font.pixelSize - 1
        }
    }

    indicator: Label {
        x: 11
        anchors.verticalCenter: parent.verticalCenter
        visible: control.showMark ? control.marked : (control.checkable && control.checked)
        text: "\u2713"
        color: control.enabled ? AppTheme.accentHover : AppTheme.disabledText
        font.pixelSize: 13
        font.bold: true
    }

    background: Rectangle {
        implicitWidth: 220
        radius: 7 * AppTheme.cornerScale
        color: control.effectiveBackgroundColor
        border.width: control.activeFocus && !control.highlighted ? 1 : 0
        border.color: AppTheme.focus
    }
}
