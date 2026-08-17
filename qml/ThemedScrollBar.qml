import QtQuick
import QtQuick.Controls

ScrollBar {
    id: control

    minimumSize: 0.05

    contentItem: Rectangle {
        implicitWidth: control.interactive ? 6 : 4
        implicitHeight: control.interactive ? 6 : 4
        radius: Math.min(width, height) / 2
        color: control.pressed || control.hovered
               ? AppTheme.borderStrong
               : AppTheme.border
        opacity: 0.9
    }
}
