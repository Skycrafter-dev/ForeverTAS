import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

// The ordered substack below a hat (mutation windows) or a container
// (mutation atoms). Children are full BlockViews loaded at runtime —
// static instantiation would create a compile-time type cycle with
// BlockView, which renders this sequence for containers and hats. The
// canvas reads gapList() to place the insertion caret while a compatible
// statement block is dragged over it.
Item {
    id: root

    property var controller
    property var viewer
    property var viewport
    property var canvas: null
    property var armedSlot: null
    property var blockIds: []
    property string emptyHint: ""
    property int ownerBlockId: 0
    property bool ownerIsHat: false

    objectName: "blockSequence"
    implicitWidth: sequenceColumn.implicitWidth
    implicitHeight: sequenceColumn.implicitHeight

    signal armRequested(int blockId, string key)

    // Local y of every insertion gap: before each child and after the
    // last; index i means "insert at position i".
    function gapList() {
        const gaps = []
        let lastChild = null
        for (let index = 0; index < sequenceColumn.children.length;
             ++index) {
            const child = sequenceColumn.children[index]
            if (child.objectName !== "sequenceChild")
                continue
            gaps.push(child.y - sequenceColumn.spacing / 2)
            lastChild = child
        }
        if (lastChild !== null)
            gaps.push(lastChild.y + lastChild.height
                      + sequenceColumn.spacing / 2)
        else
            gaps.push(0)
        return gaps
    }

    ColumnLayout {
        id: sequenceColumn

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 4

        Repeater {
            model: root.blockIds

            delegate: Loader {
                id: blockLoader

                required property var modelData
                required property int index

                objectName: "sequenceChild"
                Layout.fillWidth: true
                source: "BlockView.qml"

                onLoaded: {
                    item.objectName = "blockView" + blockLoader.modelData
                    item.controller = root.controller
                    item.viewer = root.viewer
                    item.viewport = root.viewport
                    item.canvas = root.canvas
                    item.blockId = blockLoader.modelData
                    item.armedSlot = Qt.binding(() => root.armedSlot)
                    item.width = Qt.binding(() => blockLoader.width)
                    item.armRequested.connect((blockId, key) =>
                        root.armRequested(blockId, key))
                    item.refresh()
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.blockIds.length === 0
                     && root.emptyHint.length > 0
            wrapMode: Text.WordWrap
            text: root.emptyHint
            color: ThemeControls.AppTheme.textMuted
            font.italic: true
            font.pixelSize: 11
        }
    }

    // Insertion caret shown while a statement block is dragged over a
    // gap in this sequence.
    Rectangle {
        id: insertCaret

        anchors.left: parent.left
        anchors.right: parent.right
        height: 4
        radius: 2
        z: 5
        visible: root.canvas !== null
                 && root.canvas.insertHighlight !== null
                 && root.canvas.insertHighlight.sequence === root
        y: visible ? root.canvas.insertHighlight.localY - height / 2 : 0
        color: visible ? root.canvas.insertHighlight.color
                       : ThemeControls.AppTheme.accent
    }
}
