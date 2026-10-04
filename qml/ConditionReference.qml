import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    property var controller
    property var editor
    property bool customTarget: false
    property bool expanded: false
    property int replaceStart: 0
    property int replaceEnd: 0
    property int replaceCursor: 0
    property string originalText: ""
    readonly property var entries: {
        const query = filter.text.toLowerCase()
        return (controller ? controller.conditionReference : []).filter(entry =>
            (!customTarget || !entry.conditionsOnly) &&
            (!entry.needsPointTarget || controller.evaluationTargetId === "point-target") &&
            (entry.name.toLowerCase().includes(query) ||
             entry.aliases.join(" ").toLowerCase().includes(query)))
    }
    Layout.fillWidth: true
    spacing: 4

    function complete() {
        if (!editor || !editor.enabled) return
        replaceCursor = editor.cursorPosition
        originalText = editor.text
        replaceEnd = replaceStart = replaceCursor
        while (replaceStart > 0 && /[A-Za-z0-9_.]/.test(editor.text[replaceStart - 1])) --replaceStart
        while (replaceEnd < editor.text.length && /[A-Za-z0-9_.]/.test(editor.text[replaceEnd])) ++replaceEnd
        filter.text = editor.text.slice(replaceStart, replaceCursor)
        expanded = true
        filter.forceActiveFocus()
    }

    function insertEntry(entry) {
        if (!editor || !editor.enabled) return
        if (editor.text !== originalText || editor.cursorPosition !== replaceCursor)
            replaceStart = replaceEnd = editor.cursorPosition
        editor.remove(replaceStart, replaceEnd)
        editor.insert(replaceStart, entry.insertion)
        editor.cursorPosition = replaceStart + entry.insertion.length
        expanded = false
        editor.forceActiveFocus()
    }

    RowLayout {
        Layout.fillWidth: true
        ThemedToolButton {
            objectName: "conditionReferenceToggle"
            icon.source: root.expanded ? "qrc:/icons/chevron-down.svg" : "qrc:/icons/chevron-right.svg"
            Accessible.name: qsTr("Properties and functions")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: {
                root.replaceCursor = root.editor ? root.editor.cursorPosition : 0
                root.replaceStart = root.replaceEnd = root.replaceCursor
                root.originalText = root.editor ? root.editor.text : ""
                filter.text = ""
                root.expanded = !root.expanded
            }
        }
        Label { text: qsTr("Properties and functions"); Layout.fillWidth: true; wrapMode: Text.WordWrap }
    }
    TextField {
        id: filter
        objectName: "conditionReferenceFilter"
        visible: root.expanded
        Layout.fillWidth: true
        placeholderText: qsTr("Filter properties")
        selectByMouse: true
        onAccepted: if (root.entries.length === 1) root.insertEntry(root.entries[0])
        Keys.onEscapePressed: root.expanded = false
    }
    ListView {
        visible: root.expanded
        Layout.fillWidth: true
        Layout.preferredHeight: 220
        clip: true
        model: root.entries
        spacing: 2
        ScrollBar.vertical: ScrollBar {}
        delegate: ItemDelegate {
            required property var modelData
            width: ListView.view.width
            enabled: root.editor && root.editor.enabled
            contentItem: Column {
                spacing: 2
                Label { width: parent.width; text: modelData.name; font.family: "monospace"; wrapMode: Text.WrapAnywhere; color: AppTheme.text }
                Label { width: parent.width; text: modelData.type + " | " + modelData.units; color: AppTheme.textMuted; wrapMode: Text.WordWrap; font.pixelSize: 11 }
                Label { width: parent.width; text: modelData.description; color: AppTheme.textMuted; wrapMode: Text.WordWrap; font.pixelSize: 11 }
                Label { width: parent.width; visible: modelData.aliases.length > 0; text: modelData.aliases.join(", "); color: AppTheme.textMuted; wrapMode: Text.WrapAnywhere; font.pixelSize: 11 }
            }
            onClicked: root.insertEntry(modelData)
        }
    }
}
