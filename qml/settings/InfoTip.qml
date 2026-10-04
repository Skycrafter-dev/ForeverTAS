import QtQuick
import QtQuick.Controls
import ".." as ThemeControls

// Small (i) icon that explains a setting. Hover shows the explanation;
// clicking or pressing Space keeps it open until focus moves elsewhere.
Button {
    id: root

    property string info
    readonly property bool tipShown: tip.visible

    flat: true
    checkable: true
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    display: AbstractButton.IconOnly
    implicitWidth: 20
    implicitHeight: 20
    padding: 2
    icon.source: "qrc:/icons/info.svg"
    icon.width: 15
    icon.height: 15
    icon.color: root.hovered || root.checked || root.visualFocus
                ? ThemeControls.AppTheme.info
                : ThemeControls.AppTheme.textMuted
    Accessible.name: qsTr("More information")
    Accessible.description: root.info
    onActiveFocusChanged: if (!activeFocus) checked = false

    background: Rectangle {
        radius: width / 2
        color: "transparent"
        border.width: root.visualFocus ? 1 : 0
        border.color: ThemeControls.AppTheme.focus
    }

    ToolTip {
        id: tip
        x: (root.width - width) / 2
        y: root.height + 4
        width: Math.min(implicitWidth, 320)
        delay: root.checked ? 0 : 150
        visible: root.info.length > 0 && (root.hovered || root.checked)
        text: root.info
    }
}
