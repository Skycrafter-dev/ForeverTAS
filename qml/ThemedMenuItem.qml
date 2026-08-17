import QtQuick
import QtQuick.Controls

MenuItem {
    id: control

    readonly property bool themedControl: true
    readonly property color effectiveBackgroundColor:
        !enabled ? AppTheme.disabledSurface
        : down ? AppTheme.accentPressed
        : highlighted || hovered ? AppTheme.accent : AppTheme.surface
    readonly property color effectiveTextColor:
        !enabled ? AppTheme.disabledText
        : highlighted || hovered || down
          ? AppTheme.textOnAccent : AppTheme.text

    hoverEnabled: true
    implicitHeight: 34
    leftPadding: checkable ? 30 : 12
    rightPadding: 12

    indicator: Item {
        x: 8
        width: 16
        height: 16
        anchors.verticalCenter: parent.verticalCenter
        visible: control.checkable

        Label {
            anchors.centerIn: parent
            text: control.checked ? "\u2713" : ""
            color: control.effectiveTextColor
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }
    }

    contentItem: Label {
        text: control.text
        font: control.font
        color: control.effectiveTextColor
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        color: control.effectiveBackgroundColor
        border.width: control.activeFocus ? 2 : 0
        border.color: AppTheme.focus
    }
}
