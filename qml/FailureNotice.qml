import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// A failure in plain words: what went wrong and what to do. The exact
// technical text stays one click away and is what "Copy details" copies.
ColumnLayout {
    id: root

    // A map from SearchDiagnostic: reason, guidance, details and text.
    property var failure: ({})
    // Names the action that failed when the surrounding panel does not.
    property string title: ""
    property color reasonColor: AppTheme.error
    property color textColor: AppTheme.text
    property color detailsColor: AppTheme.textMuted
    property bool detailsShown: false
    readonly property string reason: failure ? (failure.reason ?? "") : ""

    visible: reason.length > 0
    spacing: 4
    onFailureChanged: detailsShown = false

    Label {
        objectName: root.objectName + "Title"
        Layout.fillWidth: true
        visible: root.title.length > 0
        text: root.title
        color: root.reasonColor
        font.pixelSize: 12
        font.weight: Font.DemiBold
        wrapMode: Text.WordWrap
    }

    Label {
        objectName: root.objectName + "Reason"
        Layout.fillWidth: true
        text: root.reason
        color: root.reasonColor
        font.pixelSize: 12
        font.weight: root.title.length > 0 ? Font.Normal : Font.DemiBold
        wrapMode: Text.WordWrap
    }

    Label {
        objectName: root.objectName + "Guidance"
        Layout.fillWidth: true
        text: root.failure ? (root.failure.guidance ?? "") : ""
        color: root.textColor
        font.pixelSize: 12
        wrapMode: Text.WordWrap
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        ThemedButton {
            objectName: root.objectName + "DetailsToggle"
            text: root.detailsShown ? qsTr("Hide details") : qsTr("Show details")
            onClicked: root.detailsShown = !root.detailsShown
        }

        ThemedButton {
            objectName: root.objectName + "Copy"
            text: qsTr("Copy details")
            onClicked: {
                clipboardSource.text = root.failure ? (root.failure.text ?? "") : ""
                clipboardSource.selectAll()
                clipboardSource.copy()
                clipboardSource.deselect()
            }
        }

        Item { Layout.fillWidth: true }
    }

    TextArea {
        objectName: root.objectName + "Details"
        Layout.fillWidth: true
        visible: root.detailsShown
        readOnly: true
        selectByMouse: true
        wrapMode: TextEdit.Wrap
        textFormat: TextEdit.PlainText
        text: root.failure
              ? qsTr("Stage: %1\n%2").arg(root.failure.stage ?? "").arg(root.failure.details ?? "")
              : ""
        color: root.detailsColor
        font.family: "monospace"
        font.pixelSize: 11
        background: null
        padding: 0
    }

    TextEdit { id: clipboardSource; visible: false }
}
