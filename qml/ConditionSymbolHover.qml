pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    property var documentation: ({})
    readonly property string definitionText:
        documentation.signature || documentation.symbol || ""
    readonly property string typeText: {
        const type = documentation.type || documentation.kind || ""
        let result = documentation.kind === "function"
                ? qsTr("function \u00b7 returns %1").arg(type) : type
        if (documentation.unit)
            result += " \u00b7 " + documentation.unit
        return result
    }

    objectName: "conditionSymbolHoverPopup"
    modal: false
    focus: false
    closePolicy: Popup.NoAutoClose
    padding: 8
    width: Math.min(300, parent ? parent.width - 24 : 300)
    height: Math.max(64, hoverContent.implicitHeight + 16)

    background: Rectangle {
        color: AppTheme.surfaceAlternate
        border.width: 1
        border.color: AppTheme.borderStrong
        radius: 5
    }

    contentItem: ColumnLayout {
        id: hoverContent

        spacing: 4

        Label {
            objectName: "conditionSymbolHoverDefinition"
            Layout.fillWidth: true
            text: root.definitionText
            color: AppTheme.accent
            font.family: "monospace"
            font.pixelSize: 11
            font.weight: Font.DemiBold
            wrapMode: Text.WrapAnywhere
        }

        Label {
            objectName: "conditionSymbolHoverType"
            Layout.fillWidth: true
            text: root.typeText
            color: AppTheme.textMuted
            font.pixelSize: 9
            elide: Text.ElideRight
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: AppTheme.border
        }

        Label {
            objectName: "conditionSymbolHoverDescription"
            Layout.fillWidth: true
            text: root.documentation.description || ""
            color: AppTheme.text
            font.pixelSize: 10
            wrapMode: Text.WordWrap
        }
    }
}
