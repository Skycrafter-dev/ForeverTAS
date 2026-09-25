import QtQuick
import QtQuick.Controls

Button {
    id: control

    readonly property bool themedControl: true
    property bool selected: false
    readonly property color effectiveBackgroundColor:
        !enabled ? AppTheme.disabledSurface
        : down ? AppTheme.controlPressed
        : selected || (checkable && checked) ? AppTheme.selection
        : hovered ? AppTheme.controlHover : AppTheme.control
    readonly property color effectiveBorderColor:
        !enabled ? AppTheme.border
        : activeFocus ? AppTheme.focus
        : selected || (checkable && checked) ? AppTheme.accent
        : AppTheme.borderStrong

    hoverEnabled: true
    display: AbstractButton.IconOnly
    implicitWidth: 30
    implicitHeight: 30
    leftPadding: 6
    rightPadding: 6
    topPadding: 6
    bottomPadding: 6
    icon.color: enabled ? AppTheme.text : AppTheme.disabledText

    background: Rectangle {
        radius: 5
        color: control.effectiveBackgroundColor
        border.width: control.activeFocus ? 2 : 1
        border.color: control.effectiveBorderColor
    }
}
