import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Always-visible property browser for condition and custom-target editors.
// Names are browsed as a tree: the search text "car.prev.sp" opens the
// "car" branch, then "prev", and lists its children that contain "sp".
// While the editor has focus, the search follows the word at the caret.
ColumnLayout {
    id: root
    property var controller
    property var editor
    property bool customTarget: false
    // Every entry available in this editor's context.
    readonly property var entries: (controller ? controller.conditionReference : []).filter(entry =>
            (!customTarget || !entry.conditionsOnly) &&
            (!entry.needsPointTarget || controller.evaluationTargetId === "point-target"))
    readonly property var tree: buildTree(entries)
    readonly property var location: resolveLocation(filter.text.trim())
    // Rows shown for the current branch and search text.
    readonly property var rows: buildRows(location)
    readonly property string currentPath: location.node ? location.node.path : ""
    Layout.fillWidth: true
    spacing: 6

    function buildTree(list) {
        const top = { segment: "", path: "", entry: null, children: [], index: Object.create(null),
                      parent: null, entryCount: 0, localAliases: [] }
        const aliases = Object.create(null)
        for (const entry of list) {
            let node = top
            for (const segment of entry.name.split(".")) {
                const key = segment.toLowerCase()
                let child = node.index[key]
                if (!child) {
                    child = { segment: segment, path: node.path ? node.path + "." + segment : segment,
                              entry: null, children: [], index: Object.create(null), parent: node,
                              entryCount: 0, localAliases: [] }
                    node.index[key] = child
                    node.children.push(child)
                }
                node = child
            }
            node.entry = entry
            for (let counted = node; counted; counted = counted.parent) ++counted.entryCount
            for (const alias of entry.aliases) aliases[alias.toLowerCase()] = node
        }
        // An alias is listed in the branch that contains its last segment,
        // e.g. "car.x" in "car" although it means car.position.x.
        for (const alias in aliases) {
            const dot = alias.lastIndexOf(".")
            const parent = dot < 0 ? top : findNode(top, aliases, alias.slice(0, dot))
            if (parent) parent.localAliases.push({ segment: alias.slice(dot + 1), node: aliases[alias] })
        }
        top.aliases = aliases
        return top
    }

    function findNode(top, aliases, path) {
        let node = top
        for (const segment of path.split(".")) {
            const key = segment.toLowerCase()
            const next = node.index[key] ?? aliases[node.path ? node.path.toLowerCase() + "." + key : key]
            if (!next) return null
            node = next
        }
        return node
    }

    function resolveLocation(query) {
        const dot = query.lastIndexOf(".")
        const node = dot < 0 ? tree : findNode(tree, tree.aliases, query.slice(0, dot))
        return { node: node, partial: (dot < 0 ? query : query.slice(dot + 1)).toLowerCase(),
                 unknownPath: dot < 0 ? "" : query.slice(0, dot) }
    }

    function makeRow(node, label, aliasOf) {
        return { label: label, path: node.path, entry: node.entry, branch: node.children.length > 0,
                 entryCount: node.entryCount - (node.entry ? 1 : 0), aliasOf: aliasOf }
    }

    function buildRows(where) {
        if (!where.node) return []
        const partial = where.partial
        const result = []
        const listed = new Set()
        // Names starting with the search text come first, then names
        // containing it, so Enter picks the most likely completion.
        for (const prefix of [true, false]) {
            for (const child of where.node.children) {
                const segment = child.segment.toLowerCase()
                if (listed.has(child) || (prefix ? !segment.startsWith(partial) : !segment.includes(partial)))
                    continue
                result.push(makeRow(child, child.segment, ""))
                listed.add(child)
            }
        }
        if (partial.length === 0) return result
        for (const alias of where.node.localAliases) {
            if (!alias.segment.includes(partial)) continue
            if (alias.node.parent === where.node) {
                if (listed.has(alias.node)) continue
                result.push(makeRow(alias.node, alias.node.segment, ""))
                listed.add(alias.node)
            } else {
                result.push(makeRow(alias.node, alias.segment, alias.node.path))
            }
        }
        if (result.length > 0) return result
        // Nothing in this branch matches: offer deeper matches by full name.
        const deeper = node => {
            for (const child of node.children) {
                if (child.entry && (child.segment.toLowerCase().includes(partial) ||
                        child.entry.aliases.some(alias => alias.toLowerCase().includes(partial))))
                    result.push(makeRow(child, child.path, ""))
                deeper(child)
            }
        }
        deeper(where.node)
        return result
    }

    function crumbs() {
        const result = []
        for (let node = location.node; node && node.parent; node = node.parent)
            result.unshift({ label: node.segment, path: node.path })
        return result
    }

    function openPath(path) {
        filter.text = path.length > 0 ? path + "." : ""
        list.currentIndex = 0
    }

    function tokenBeforeCursor() {
        if (!editor) return ""
        let start = editor.cursorPosition
        while (start > 0 && /[A-Za-z0-9_.]/.test(editor.text[start - 1])) --start
        return editor.text.slice(start, editor.cursorPosition)
    }

    function followEditor() {
        if (!editor || !editor.activeFocus) return
        if (filter.text !== tokenBeforeCursor()) {
            filter.text = tokenBeforeCursor()
            list.currentIndex = 0
        }
    }

    // Ctrl+Space: search for the word at the caret and move focus to the
    // results so they can be picked with the keyboard.
    function complete() {
        if (!editor || !editor.enabled) return
        filter.text = tokenBeforeCursor()
        list.currentIndex = 0
        filter.forceActiveFocus()
    }

    // Replaces the word at the editor caret with the entry's canonical text.
    function insertEntry(entry) {
        if (!editor || !editor.enabled) return
        const text = editor.text
        let start = editor.cursorPosition
        let end = start
        while (start > 0 && /[A-Za-z0-9_.]/.test(text[start - 1])) --start
        while (end < text.length && /[A-Za-z0-9_.]/.test(text[end])) ++end
        editor.remove(start, end)
        editor.insert(start, entry.insertion)
        editor.cursorPosition = start + entry.insertion.length
        editor.forceActiveFocus()
        followEditor()
    }

    function activate(row) {
        if (!row) return
        if (row.entry) insertEntry(row.entry)
        else openPath(row.path)
    }

    Connections {
        target: root.editor
        function onCursorPositionChanged() { root.followEditor() }
        function onTextChanged() { root.followEditor() }
        function onActiveFocusChanged() { root.followEditor() }
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("Properties and functions")
        font.weight: Font.Medium
        wrapMode: Text.WordWrap
    }

    TextField {
        id: filter
        objectName: "conditionReferenceFilter"
        Layout.fillWidth: true
        placeholderText: qsTr("Search, e.g. car.speed")
        selectByMouse: true
        color: AppTheme.text
        placeholderTextColor: AppTheme.textFaint
        selectionColor: AppTheme.selection
        selectedTextColor: AppTheme.text
        Accessible.name: qsTr("Search properties and functions")
        onTextEdited: list.currentIndex = 0
        onAccepted: root.activate(root.rows[list.currentIndex] ?? root.rows[0])
        Keys.onDownPressed: list.currentIndex = Math.min(list.currentIndex + 1, list.count - 1)
        Keys.onUpPressed: list.currentIndex = Math.max(list.currentIndex - 1, 0)
        Keys.onEscapePressed: if (root.editor) root.editor.forceActiveFocus()
        background: Rectangle {
            color: AppTheme.surface
            border.width: 1
            border.color: filter.activeFocus ? AppTheme.focus : AppTheme.border
            radius: 6
        }
    }

    Flow {
        objectName: "conditionReferencePath"
        Layout.fillWidth: true
        visible: root.location.node !== null && root.currentPath.length > 0
        spacing: 2

        ThemedButton {
            objectName: "conditionReferenceAllButton"
            flat: true
            text: qsTr("All")
            implicitHeight: 26
            topPadding: 2
            bottomPadding: 2
            onClicked: root.openPath("")
        }
        Repeater {
            model: root.crumbs()
            delegate: Row {
                required property var modelData
                spacing: 2
                Label {
                    height: 26
                    verticalAlignment: Text.AlignVCenter
                    text: "›"
                    color: AppTheme.textMuted
                }
                ThemedButton {
                    flat: true
                    text: modelData.label
                    font.family: "monospace"
                    implicitHeight: 26
                    topPadding: 2
                    bottomPadding: 2
                    onClicked: root.openPath(modelData.path)
                }
            }
        }
    }

    Label {
        objectName: "conditionReferenceEmpty"
        Layout.fillWidth: true
        visible: root.rows.length === 0
        text: root.location.node === null
              ? qsTr("Nothing is named “%1”.").arg(root.location.unknownPath)
              : qsTr("No matches.")
        color: AppTheme.textMuted
        wrapMode: Text.WordWrap
        font.pixelSize: 12
    }

    ListView {
        id: list
        objectName: "conditionReferenceList"
        visible: count > 0
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, 240)
        clip: true
        model: root.rows
        spacing: 1
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        delegate: ThemedItemDelegate {
            id: row
            required property var modelData
            required property int index
            width: ListView.view.width
            highlighted: ListView.isCurrentItem && filter.activeFocus
            // Branches stay browsable while the editor is read-only.
            enabled: modelData.branch || (root.editor && root.editor.enabled)
            topPadding: 4
            bottomPadding: 4
            rightPadding: modelData.branch ? 36 : 8
            Accessible.name: modelData.entry ? qsTr("Insert %1").arg(modelData.entry.name)
                                             : qsTr("Open %1").arg(modelData.path)
            contentItem: Column {
                spacing: 1
                RowLayout {
                    width: parent.width
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: row.modelData.label
                        font.family: "monospace"
                        color: row.effectiveTextColor
                        elide: Text.ElideRight
                    }
                    Label {
                        text: row.modelData.entry
                              ? row.modelData.entry.type + " · " + row.modelData.entry.units
                              : row.modelData.entryCount === 1 ? qsTr("1 property")
                              : qsTr("%1 properties").arg(row.modelData.entryCount)
                        color: AppTheme.textMuted
                        font.pixelSize: 11
                    }
                }
                Label {
                    width: parent.width
                    visible: text.length > 0
                    text: !row.modelData.entry ? ""
                          : row.modelData.aliasOf.length > 0
                            ? qsTr("Short for %1. %2").arg(row.modelData.aliasOf).arg(row.modelData.entry.description)
                            : row.modelData.entry.description
                              + (row.modelData.entry.aliases.length > 0
                                 ? " " + qsTr("Also written %1.").arg(row.modelData.entry.aliases.join(", "))
                                 : "")
                    color: AppTheme.textMuted
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                }
            }
            onClicked: root.activate(modelData)

            ThemedIconButton {
                objectName: "conditionReferenceOpenButton"
                visible: row.modelData.branch
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: 26
                implicitHeight: 26
                padding: 5
                icon.source: "qrc:/icons/chevron-right.svg"
                icon.width: 16
                icon.height: 16
                Accessible.name: qsTr("Open %1").arg(row.modelData.path)
                ToolTip.visible: hovered
                ToolTip.delay: 350
                ToolTip.text: qsTr("Show what is inside %1").arg(row.modelData.path)
                onClicked: root.openPath(row.modelData.path)
            }
        }
    }
}
