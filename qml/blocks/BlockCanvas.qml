import QtQuick
import QtQuick.Controls
import ".." as ThemeControls
import "." as Blocks

// Free-form block canvas. The search script and every loose block live
// at positions the user chooses: drag the background to pan, Ctrl+wheel
// to zoom, and drag any block by its card to move it. Statements snap
// into compatible stacks, number reporters snap into number slots, and
// evaluation reporters snap into the hat's evaluator socket; dropping on
// empty space parks the block as a loose draft. The model is only
// touched when a drag ends, so a gesture never destroys the item that
// started it.
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

    // Active drag state (JS object), plus published highlights the
    // targets render: the insertion caret, the glowing slot, and the
    // evaluator socket.
    property var dragState: null
    property int draggingBlockId: 0
    property var insertHighlight: null
    property var slotHighlight: null
    property bool evaluatorHighlight: false

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

    // ---- Drag lifecycle (driven by every block's card drag area) ----

    function dragPressed(view, area, mouse) {
        if (!editingEnabled || !view.blockInformation)
            return
        const origin = mapPosition(area, 0, 0)
        const cursor = mapPosition(area, mouse.x, mouse.y)
        dragState = {
            blockId: view.blockId,
            shape: view.blockInformation.shape,
            optionKind: view.blockInformation.optionKind,
            label: view.headerText,
            color: view.blockInformation.color,
            width: Math.max(170, view.width),
            originX: origin.x,
            originY: origin.y,
            grabDX: cursor.x - origin.x,
            grabDY: cursor.y - origin.y,
            cursorX: cursor.x,
            cursorY: cursor.y,
            wasLoose: view.canvasIsLoose,
            started: false,
            target: null
        }
    }

    function dragMoved(area, mouse) {
        if (!dragState)
            return
        const cursor = mapPosition(area, mouse.x, mouse.y)
        if (!dragState.started) {
            if (Math.abs(cursor.x - dragState.cursorX) < 5
                    && Math.abs(cursor.y - dragState.cursorY) < 5)
                return
            dragState.started = true
            draggingBlockId = dragState.blockId
            dragGhost.label = dragState.label
            dragGhost.accent = dragState.color
            dragGhost.width = dragState.width
            dragGhost.x = dragState.originX
            dragGhost.y = dragState.originY
            dragGhost.visible = true
        }
        dragGhost.x = cursor.x - dragState.grabDX
        dragGhost.y = cursor.y - dragState.grabDY
        updateSnapTarget()
    }

    function dragReleased() {
        if (!dragState)
            return
        const state = dragState
        dragState = null
        draggingBlockId = 0
        clearHighlights()
        dragGhost.visible = false
        if (!state.started)
            return
        if (state.target) {
            if (state.target.type === "insert") {
                // The gap index counts the substack as currently shown;
                // attachBlock indexes it after the child is detached.
                let index = state.target.index
                const ids = state.target.sequence.blockIds
                const current = ids.indexOf(state.blockId)
                if (current !== -1 && current < index)
                    index -= 1
                controller.attachBlock(state.target.parentId, index,
                                       state.blockId)
            } else if (state.target.type === "slot") {
                // A displaced chip is parked beside the slot.
                controller.graftReporterBlock(
                            state.target.blockId, state.target.key,
                            state.blockId,
                            dragGhost.x + dragGhost.width + 16, dragGhost.y)
            } else if (state.target.type === "evaluator") {
                controller.setEvaluatorBlockId(state.blockId)
            }
            return
        }
        if (state.shape === "hat" || state.wasLoose)
            controller.setBlockPosition(state.blockId, dragGhost.x,
                                        dragGhost.y)
        else
            controller.detachBlockToCanvas(state.blockId, dragGhost.x,
                                           dragGhost.y)
    }

    function clearHighlights() {
        insertHighlight = null
        slotHighlight = null
        evaluatorHighlight = false
    }

    // ---- Snap detection: walk the live item tree ----

    function walkItems(item, visit) {
        visit(item)
        for (let index = 0; index < item.children.length; ++index)
            walkItems(item.children[index], visit)
    }

    function updateSnapTarget() {
        clearHighlights()
        dragState.target = null
        if (!dragState)
            return
        const centerX = dragGhost.x + dragGhost.width / 2
        const centerY = dragGhost.y + dragGhost.height / 2
        if (dragState.shape === "reporter") {
            updateReporterTarget(centerX, centerY)
            return
        }
        updateStatementTarget(centerX, centerY)
    }

    function updateReporterTarget(centerX, centerY) {
        let best = null
        if (dragState.optionKind === "evaluation") {
            walkItems(stackLayer, (item) => {
                if (item.objectName !== "hatSockets" || best !== null)
                    return
                const origin = mapPosition(item, 0, 0)
                if (centerX >= origin.x - 8
                        && centerX <= origin.x + item.width + 8
                        && centerY >= origin.y - 8
                        && centerY <= origin.y + item.height + 8)
                    best = { type: "evaluator" }
            })
            if (best !== null)
                evaluatorHighlight = true
            dragState.target = best
            return
        }
        if (dragState.optionKind !== "")
            return
        walkItems(stackLayer, (item) => {
            if (!item.objectName
                    || item.objectName.substring(0, 9) !== "blockSlot"
                    || item.canvas !== root
                    || item.fieldKind !== "number"
                    || item.blockId === dragState.blockId)
                return
            const origin = mapPosition(item, 0, 0)
            if (centerX < origin.x - 6
                    || centerX > origin.x + item.width + 6
                    || centerY < origin.y - 6
                    || centerY > origin.y + item.height + 6)
                return
            const distance = (centerX - (origin.x + item.width / 2)) ** 2
                    + (centerY - (origin.y + item.height / 2)) ** 2
            if (best === null || distance < best.distance)
                best = {
                    type: "slot",
                    blockId: item.blockId,
                    key: item.fieldKey,
                    distance: distance
                }
        })
        if (best !== null)
            slotHighlight = { blockId: best.blockId, key: best.key }
        dragState.target = best
    }

    function updateStatementTarget(centerX, centerY) {
        const containerDrag = dragState.shape === "container"
        let best = null
        walkItems(stackLayer, (item) => {
            if (item.objectName !== "blockSequence" || item.canvas !== root)
                return
            if (item.ownerIsHat !== containerDrag)
                return
            if (item.ownerBlockId === dragState.blockId)
                return
            const origin = mapPosition(item, 0, 0)
            const overlap = Math.min(dragGhost.x + dragGhost.width,
                                     origin.x + item.width)
                    - Math.max(dragGhost.x, origin.x)
            if (overlap < 24)
                return
            const gaps = item.gapList()
            for (let index = 0; index < gaps.length; ++index) {
                const worldGap = mapPosition(item, 0, gaps[index])
                const distance = Math.abs(centerY - worldGap.y)
                if (distance > 44)
                    continue
                const score = distance - overlap * 0.1
                if (best === null || score < best.score)
                    best = {
                        type: "insert",
                        parentId: item.ownerBlockId,
                        index: index,
                        sequence: item,
                        localY: gaps[index],
                        score: score
                    }
            }
        })
        if (best !== null) {
            insertHighlight = {
                sequence: best.sequence,
                localY: best.localY,
                color: dragState.color
            }
            dragState.target = {
                type: "insert",
                parentId: best.parentId,
                index: best.index,
                sequence: best.sequence
            }
        }
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

            objectName: "blockWorld"
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

            // Drag ghost: follows the cursor while a block is lifted.
            Rectangle {
                id: dragGhost

                objectName: "blockDragGhost"
                visible: false
                z: 1000
                height: 36
                radius: 8
                opacity: 0.92
                color: ThemeControls.AppTheme.surface
                border.width: 1

                property string label: ""
                property color accent: ThemeControls.AppTheme.border

                Rectangle {
                    anchors.left: parent.left
                    anchors.leftMargin: 1
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 4
                    radius: 2
                    color: dragGhost.accent
                }

                Label {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 8
                    verticalAlignment: Text.AlignVCenter
                    text: dragGhost.label
                    elide: Text.ElideRight
                    font.weight: Font.DemiBold
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
