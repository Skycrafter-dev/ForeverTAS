import QtQuick
import QtQuick.Controls

CheckBox {
    id: control

    readonly property bool themedControl: true
    readonly property color effectiveIndicatorColor:
        !enabled ? AppTheme.disabledSurface
        : checked
          ? (down ? AppTheme.accentPressed
             : hovered ? AppTheme.accentHover : AppTheme.accent)
        : down ? AppTheme.controlPressed
        : hovered ? AppTheme.controlHover : AppTheme.surface
    readonly property color effectiveBorderColor:
        !enabled ? AppTheme.border
        : activeFocus ? AppTheme.focus
        : checked ? AppTheme.accentBorder : AppTheme.borderStrong
    readonly property color effectiveTextColor:
        enabled ? AppTheme.text : AppTheme.disabledText

    hoverEnabled: true
    spacing: 8
    implicitHeight: 30

    indicator: Rectangle {
        implicitWidth: 20
        implicitHeight: 20
        x: control.leftPadding
        y: Math.round((control.height - height) / 2)
        radius: 4 * AppTheme.cornerScale
        color: control.effectiveIndicatorColor
        border.width: control.activeFocus ? 2 : 1
        border.color: control.effectiveBorderColor

        Item {
            anchors.centerIn: parent
            width: 12
            height: 10
            visible: control.checked

            Rectangle {
                x: 1
                y: 5
                width: 5
                height: 2
                radius: 1
                rotation: 42
                color: control.enabled
                       ? AppTheme.textOnAccent : AppTheme.disabledText
            }

            Rectangle {
                x: 4
                y: 4
                width: 9
                height: 2
                radius: 1
                rotation: -48
                color: control.enabled
                       ? AppTheme.textOnAccent : AppTheme.disabledText
            }
        }
    }

    contentItem: Label {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        font: control.font
        color: control.effectiveTextColor
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
    }
}
