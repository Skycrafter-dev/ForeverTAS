import QtQuick
import QtQuick.Controls

// A drop-down menu on glass.
Menu {
    id: menu

    topPadding: 6
    bottomPadding: 6
    leftPadding: 6
    rightPadding: 6
    implicitWidth: Math.max(240, contentItem.implicitWidth + leftPadding + rightPadding)
    delegate: ThemedMenuItem {}

    background: GlassPopupBackground {
        implicitWidth: 240
        radius: 12 * AppTheme.cornerScale
    }
}
