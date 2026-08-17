import QtQuick
import QtQuick.Layouts
import "." as Blocks

// One top-level canvas entry: the search script (rooted at its hat) or a
// loose stack the user parked anywhere. Position is synced imperatively
// from the entry so external updates (text interchange, reloads) land
// without fighting the binding; drags are handled by the canvas.
ColumnLayout {
    id: root

    property var controller
    property var viewer
    property var viewport
    property var canvas: null
    property var armedSlot: null
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

    function synchronize() {
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
        canvas: root.canvas
        blockId: root.rootBlockId
        armedSlot: root.armedSlot
        isScriptHat: root.entry ? root.entry.isScript : false
        canvasLoose: root.loose
        canvasIsLoose: root.loose
        onArmRequested: (blockId, key) => root.armRequested(blockId, key)
    }
}
