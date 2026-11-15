import QtQuick
import QtQuick.Controls

// The window's menu bar: a thin glass strip along the top with the menu
// titles on the left.
MenuBar {
    id: bar

    // Where this bar's top-left sits over the race view (race view px).
    property point backdropOrigin: Qt.point(0, 0)

    spacing: 2
    leftPadding: 8
    topPadding: 3
    bottomPadding: 3

    delegate: MenuBarItem {
        id: item
        implicitHeight: 24
        leftPadding: 10
        rightPadding: 10
        font.pixelSize: 13
        contentItem: Text {
            text: item.text
            font: item.font
            color: item.highlighted || item.menu?.visible ? AppTheme.text : AppTheme.textMuted
            verticalAlignment: Text.AlignVCenter
        }
        background: Gloss {
            anchors.fill: parent
            anchors.margins: -2
            radius: 7 * AppTheme.cornerScale
            fill: item.highlighted || item.menu?.visible
                  ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(1, 1, 1, 0)
            rimLight: item.highlighted || item.menu?.visible ? 0.5 : 0
        }
    }

    background: Glass {
        origin: bar.backdropOrigin
        radius: 0
        bevel: 6
        refraction: AppTheme.edgeBend * 0.5
        tintAmount: Math.min(0.85, AppTheme.darkening + 0.12)
        shadowAlpha: 0.3
        shadowRadius: 10
        shadowOffset: 2
    }
}
