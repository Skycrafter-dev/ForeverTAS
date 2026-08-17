import QtQuick
import QtQuick.Controls

ScrollBar {
    id: control

    implicitWidth: 10
    minimumSize: 0.05
    margins: 2

    contentItem: Rectangle {
        implicitWidth: 6
        radius: 3
        color: control.pressed
               ? AppTheme.borderStrong
               : control.interactive || control.hovered
                 ? AppTheme.borderStrong : AppTheme.border
        opacity: 0.9
    }

    background: null
}
