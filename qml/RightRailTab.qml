import QtQuick
import QtQuick.Controls

Button {
    id: control

    readonly property bool themedControl: true
    property bool active: false

    hoverEnabled: true
    implicitWidth: 82
    implicitHeight: 46
    leftPadding: 10
    rightPadding: 10
    topPadding: 6
    bottomPadding: 6

    contentItem: Label {
        text: control.text
        color: !control.enabled ? AppTheme.disabledText
                               : control.active ? AppTheme.text
                                                : AppTheme.textMuted
        font.weight: control.active ? Font.DemiBold : Font.Normal
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        color: !control.enabled ? "transparent"
                                : control.active ? AppTheme.panel
                                : control.down ? AppTheme.controlPressed
                                : control.hovered ? AppTheme.surfaceAlternate
                                                  : "transparent"

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 3
            visible: control.active
            color: AppTheme.accent
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: AppTheme.border
            opacity: 0.7
        }

        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.width: control.activeFocus ? 2 : 0
            border.color: AppTheme.focus
        }
    }
}
