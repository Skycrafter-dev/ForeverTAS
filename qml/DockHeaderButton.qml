import QtQuick
import QtQuick.Controls

// Small icon button in a panel header.
Button {
    id: control

    readonly property bool themedControl: true
    property string iconName
    property string tip

    implicitWidth: 26
    implicitHeight: 24
    padding: 5
    hoverEnabled: true
    focusPolicy: Qt.NoFocus
    display: AbstractButton.IconOnly
    Accessible.name: tip

    icon.source: "qrc:/icons/" + iconName + ".svg"
    icon.width: 14
    icon.height: 14
    icon.color: hovered ? AppTheme.text : AppTheme.textMuted

    background: Gloss {
        anchors.fill: parent
        anchors.margins: -1
        fill: control.down ? Qt.rgba(1, 1, 1, 0.06)
              : control.hovered ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(1, 1, 1, 0)
        rimLight: control.hovered ? 0.45 : 0
        radius: 7 * AppTheme.cornerScale
    }

    ToolTip.visible: hovered && tip.length > 0
    ToolTip.delay: 600
    ToolTip.text: tip
}
