import QtQuick
import QtQuick.Controls

Button {
    id: control

    readonly property bool themedControl: true
    property string contentObjectName: ""
    property bool elideText: true
    readonly property color effectiveBackgroundColor:
        !enabled ? AppTheme.disabledSurface
        : (highlighted || checked)
          ? (down ? AppTheme.accentPressed
             : hovered ? AppTheme.accentHover : AppTheme.accent)
        : down ? AppTheme.controlPressed
        : hovered ? AppTheme.controlHover
        : flat ? "transparent" : AppTheme.control
    readonly property color effectiveBorderColor:
        !enabled ? AppTheme.border
        : activeFocus ? AppTheme.focus
        : (highlighted || checked) ? AppTheme.accentBorder
        : flat && !hovered && !down ? "transparent" : AppTheme.borderStrong
    readonly property color effectiveTextColor:
        !enabled ? AppTheme.disabledText
        : (highlighted || checked) ? AppTheme.textOnAccent : AppTheme.text

    hoverEnabled: true
    implicitHeight: 34
    leftPadding: 12
    rightPadding: 12
    topPadding: 6
    bottomPadding: 6
    icon.color: effectiveTextColor

    contentItem: Label {
        objectName: control.contentObjectName
        text: control.text
        font: control.font
        color: control.effectiveTextColor
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: control.elideText ? Text.ElideRight : Text.ElideNone
    }

    background: Rectangle {
        radius: 5 * AppTheme.cornerScale
        color: control.effectiveBackgroundColor
        border.width: control.activeFocus ? 2 : 1
        border.color: control.effectiveBorderColor
    }
}
