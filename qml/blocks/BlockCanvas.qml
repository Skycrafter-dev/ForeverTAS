import QtQuick
import QtQuick.Controls
import ".." as ThemeControls
import "." as Blocks

// Free-form block canvas. The search script and every loose block live
// at positions the user chooses: drag the background to pan, Ctrl+wheel
// to zoom, and drag a loose block by its card to move it.
Rectangle {
    id: root

    property var controller
    property var viewer
    property var viewport
    property var armedSlot: null

    signal armRequested(int blockId, string key)

    objectName: "blockScript"
    radius: 10
    color: ThemeControls.AppTheme.surfaceAlternate
    border.width: 1
    border.color: ThemeControls.AppTheme.border

    readonly property var entries: controller ? controller.blockCanvas : []
    readonly property bool editingEnabled:
        controller !== null && !controller.running

    // New loose blocks cascade so repeated drops stay distinguishable.
    property int dropCascade: 0

    function mapPosition(item, x, y) {
        return world.mapFromItem(item, x, y)
    }

    function commitLoosePosition(blockId, x, y) {
        if (controller)
            controller.setBlockPosition(blockId, x, y)
    }

    function nextDropPosition() {
        const cascade = dropCascade % 6
        dropCascade = dropCascade + 1
        const x = (flick.contentX + Math.min(flick.width, 360) * 0.6)
                / world.scale + cascade * 26
        const y = (flick.contentY + 48) / world.scale + cascade * 22
        return Qt.point(Math.max(24, x), Math.max(16, y))
    }

    function clamp(value, minimum, maximum) {
        return Math.max(minimum, Math.min(maximum, value))
    }

    function recomputeBounds() {
        let right = 480
        let bottom = 360
        for (let index = 0; index < stackLayer.children.length; ++index) {
            const child = stackLayer.children[index]
            right = Math.max(right, child.x + child.width)
            bottom = Math.max(bottom, child.y + child.height)
        }
        world.contentRight = right + 48
        world.contentBottom = bottom + 48
    }

    onEntriesChanged: boundsTimer.restart()

    Timer {
        id: boundsTimer

        interval: 0
        onTriggered: root.recomputeBounds()
    }

    Flickable {
        id: flick

        anchors.fill: parent
        anchors.margins: 6
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentWidth: world.contentRight * world.scale
        contentHeight: world.contentBottom * world.scale

        ScrollBar.vertical: ThemeControls.ThemedScrollBar {
            policy: flick.contentHeight > flick.height
                    ? ScrollBar.AsNeeded
                    : ScrollBar.AlwaysOff
        }

        ScrollBar.horizontal: ThemeControls.ThemedScrollBar {
            policy: flick.contentWidth > flick.width
                    ? ScrollBar.AsNeeded
                    : ScrollBar.AlwaysOff
        }

        WheelHandler {
            acceptedModifiers: Qt.NoModifier
            onWheel: (event) => {
                flick.contentY = root.clamp(
                            flick.contentY - event.angleDelta.y, 0,
                            flick.contentHeight - flick.height)
            }
        }

        WheelHandler {
            acceptedModifiers: Qt.ShiftModifier
            onWheel: (event) => {
                flick.contentX = root.clamp(
                            flick.contentX - event.angleDelta.y, 0,
                            flick.contentWidth - flick.width)
            }
        }

        WheelHandler {
            acceptedModifiers: Qt.ControlModifier
            onWheel: (event) => {
                const anchor = world.mapFromItem(flick, event.position.x,
                                                 event.position.y)
                const factor = event.angleDelta.y > 0 ? 1.12 : 1 / 1.12
                const next = root.clamp(world.scale * factor, 0.5, 1.6)
                world.scale = next
                flick.contentX = root.clamp(
                            anchor.x * next - event.position.x, 0,
                            flick.contentWidth - flick.width)
                flick.contentY = root.clamp(
                            anchor.y * next - event.position.y, 0,
                            flick.contentHeight - flick.height)
            }
        }

        Item {
            id: world

            property real contentRight: 480
            property real contentBottom: 360

            width: contentRight
            height: contentBottom
            transformOrigin: Item.TopLeft
            scale: 1

            Image {
                anchors.fill: parent
                fillMode: Image.Tile
                source: "data:image/svg+xml;utf8,"
                        + (ThemeControls.AppTheme.dark
                           ? "<svg xmlns='http://www.w3.org/2000/svg' "
                             + "width='26' height='26'><circle cx='1.4' "
                             + "cy='1.4' r='1.1' fill='%23485568'/></svg>"
                           : "<svg xmlns='http://www.w3.org/2000/svg' "
                             + "width='26' height='26'><circle cx='1.4' "
                             + "cy='1.4' r='1.1' fill='%23c3cbd8'/></svg>")
            }

            Item {
                id: stackLayer

                anchors.fill: parent

                Repeater {
                    model: root.entries

                    delegate: Blocks.BlockStack {
                        id: canvasStack

                        required property var modelData

                        canvas: root
                        controller: root.controller
                        viewer: root.viewer
                        viewport: root.viewport
                        armedSlot: root.armedSlot
                        entry: canvasStack.modelData
                        onWidthChanged: root.recomputeBounds()
                        onHeightChanged: root.recomputeBounds()
                        onArmRequested: (blockId, key) =>
                            root.armRequested(blockId, key)
                    }
                }
            }
        }
    }

    ThemeControls.ThemedToolButton {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 4
        z: 6
        text: Math.round(world.scale * 100) + "%"
        onClicked: {
            world.scale = 1
            flick.contentX = 0
            flick.contentY = 0
        }
        ToolTip.visible: hovered
        ToolTip.text: qsTr("Reset the canvas view")
    }
}
