import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

Rectangle {
    id: root

    property string title
    property string description
    property bool collapsible: true
    property bool expanded: true
    default property alias sectionContent: contentColumn.data

    Layout.fillWidth: true
    implicitHeight: sectionLayout.implicitHeight + 28
    radius: 10
    color: ThemeControls.AppTheme.panelAlternate
    border.width: 1
    border.color: ThemeControls.AppTheme.border

    ColumnLayout {
        id: sectionLayout
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        // The whole header strip toggles the section; the chevron just
        // makes the state legible.
        Item {
            id: headerStrip

            Layout.fillWidth: true
            implicitHeight: headerRow.implicitHeight

            RowLayout {
                id: headerRow

                anchors.fill: parent
                spacing: 8

                ColumnLayout {
                    id: headerText

                    Layout.fillWidth: true
                    spacing: 2

                    Label {
                        Layout.fillWidth: true
                        text: root.title
                        color: ThemeControls.AppTheme.text
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: root.description
                        color: ThemeControls.AppTheme.textMuted
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }
                }

                Label {
                    visible: root.collapsible
                    Layout.alignment: Qt.AlignTop
                    text: root.expanded ? "▾" : "▸"
                    color: ThemeControls.AppTheme.textMuted
                    font.pixelSize: 14
                }
            }

            MouseArea {
                anchors.fill: parent
                enabled: root.collapsible
                cursorShape: Qt.PointingHandCursor
                onClicked: root.expanded = !root.expanded
            }
        }

        ColumnLayout {
            id: contentColumn

            Layout.fillWidth: true
            spacing: 8
            visible: root.expanded || !root.collapsible
        }
    }
}
