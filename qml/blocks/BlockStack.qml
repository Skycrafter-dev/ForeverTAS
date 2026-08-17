import QtQuick
import QtQuick.Layouts
import "." as Blocks

// One top-level canvas entry: the search script (rooted at its hat) or a
// loose stack the user parked anywhere. Loose stacks drag by their root
// block's card; the position is synced imperatively so dragging never
// fights the entry binding (the same pattern the slot editors use).
ColumnLayout {
    id: root

    property var controller
    property var viewer
    property var viewport
    property var armedSlot: null
    property var canvas: null
    property var entry: null

    readonly property int rootBlockId: entry ? entry.blockId : 0
    readonly property bool loose: entry ? !entry.isScript : false

    signal armRequested(int blockId, string key)

    x: position.x
    y: position.y
    width: implicitWidth
    height: implicitHeight

    QtObject {
        id: position

        property real x: 0
        property real y: 0
    }

    property point grabOffset

    function synchronize() {
        if (rootView.dragging)
            return
        position.x = root.entry ? root.entry.x : 0
        position.y = root.entry ? root.entry.y : 0
    }

    Component.onCompleted: synchronize()
    onEntryChanged: synchronize()

    Blocks.BlockView {
        id: rootView

        objectName: root.loose ? "blockView" + root.rootBlockId
                               : "blockViewHat"
        Layout.fillWidth: true
        Layout.preferredWidth: 300
        controller: root.controller
        viewer: root.viewer
        viewport: root.viewport
        blockId: root.rootBlockId
        armedSlot: root.armedSlot
        isScriptHat: root.entry ? root.entry.isScript : false
        canvasLoose: root.loose
        canvasDraggable: root.loose
        onArmRequested: (blockId, key) => root.armRequested(blockId, key)
        onCanvasDragPressed: (area, mouse) => {
            const cursor = root.canvas.mapPosition(area, mouse.x, mouse.y)
            root.grabOffset = Qt.point(cursor.x - position.x,
                                       cursor.y - position.y)
        }
        onCanvasDragMoved: (area, mouse) => {
            const cursor = root.canvas.mapPosition(area, mouse.x, mouse.y)
            position.x = cursor.x - root.grabOffset.x
            position.y = cursor.y - root.grabOffset.y
        }
        onCanvasDragReleased:
            root.canvas.commitLoosePosition(root.rootBlockId,
                                            position.x, position.y)
    }
}
