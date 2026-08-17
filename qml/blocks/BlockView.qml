import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls
import "." as Blocks

// Renders one placed block from the controller's block data: header
// (label, category color, per-shape controls), typed value slots, and the
// block's structure — the script hat owns the evaluator socket and the
// mutation windows, containers own their atoms. Data refreshes in place
// on controller.blockUpdated so delegates and focus survive field edits.
ColumnLayout {
    id: root

    property var controller: null
    property var viewer
    property var viewport
    property int blockId: 0
    property var armedSlot: null
    property int moveIndex: -1
    property int moveCount: 0
    // Canvas state: only the script hat renders the evaluator socket;
    // loose blocks (parked on the canvas, not wired into the script)
    // render muted and drag by their card.
    property bool isScriptHat: false
    property bool canvasLoose: false
    property bool canvasDraggable: false

    // Emitted when a slot wants to (dis)arm for reporter attachment.
    signal armRequested(int blockId, string key)
    // Emitted by the card drag underlay while the owning stack drags.
    signal canvasDragPressed(var area, var mouse)
    signal canvasDragMoved(var area, var mouse)
    signal canvasDragReleased()

    property var blockInformation: null

    function refresh() {
        if (!controller || !blockId) {
            blockInformation = null
            return
        }
        blockInformation = controller.blockData(blockId)
    }

    readonly property string shape: blockInformation
                                     ? blockInformation.shape : ""
    readonly property bool isStack: shape === "stack"
    readonly property bool isContainer: shape === "container"
    readonly property bool movable: isStack || isContainer
    readonly property int evaluatorId: blockInformation
                                      ? blockInformation.evaluator : 0
    readonly property var substackIds: blockInformation
                                      ? blockInformation.substack : []
    readonly property var groupedFields: {
        const fields = blockInformation ? blockInformation.fields : []
        let groups = []
        let current = null
        for (let index = 0; index < fields.length; ++index) {
            const field = fields[index]
            const groupName = field.group ? field.group : ""
            if (!current || current.name !== groupName) {
                current = { "name": groupName, "fields": [] }
                groups.push(current)
            }
            current.fields.push(field)
        }
        return groups
    }

    spacing: 6

    Component.onCompleted: refresh()

    Connections {
        target: root.controller
        enabled: root.controller !== null
                 && root.controller !== undefined

        function onBlockUpdated(updatedBlockId) {
            if (updatedBlockId === root.blockId)
                root.refresh()
        }
    }

    onBlockIdChanged: refresh()

    function requestArm(blockId, key) {
        root.armRequested(blockId, key)
    }

    Rectangle {
        id: blockCard

        Layout.fillWidth: true
        implicitHeight: headerRow.implicitHeight + 12
        radius: 10
        opacity: root.canvasLoose ? 0.85 : 1.0
        color: ThemeControls.AppTheme.surface
        border.width: 1
        border.color: {
            if (root.canvasLoose)
                return ThemeControls.AppTheme.border
            return root.blockInformation
                    ? root.blockInformation.color
                    : ThemeControls.AppTheme.border
        }

        // Drag underlay: sits beneath the header row, so labels and empty
        // card areas drag the whole stack while buttons and slots keep
        // their own clicks.
        MouseArea {
            id: cardDragArea

            anchors.fill: parent
            enabled: root.canvasDraggable && root.controller
                      && !root.controller.running
            preventStealing: true
            cursorShape: enabled ? Qt.SizeAllCursor : Qt.ArrowCursor
            hoverEnabled: enabled

            property bool moved: false

            onPressed: (mouse) => {
                moved = false
                root.canvasDragPressed(cardDragArea, mouse)
            }
            onPositionChanged: (mouse) => {
                if (!pressed)
                    return
                moved = true
                root.canvasDragMoved(cardDragArea, mouse)
            }
            onReleased: {
                if (moved)
                    root.canvasDragReleased()
                moved = false
            }
            onCanceled: moved = false
        }

        HoverHandler {
            id: cardHover
        }
        ToolTip.visible: cardHover.hovered && root.canvasLoose
        ToolTip.delay: 350
        ToolTip.text: qsTr("Not wired into the search — drag it into the "
                           + "script, or park it here for later.")

        Rectangle {
            anchors.left: parent.left
            anchors.leftMargin: 1
            anchors.top: parent.top
            anchors.topMargin: 8
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 8
            width: 4
            radius: 2
            color: root.blockInformation ? root.blockInformation.color
                             : ThemeControls.AppTheme.border
        }

        RowLayout {
            id: headerRow

            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 4
            anchors.topMargin: 4
            anchors.bottomMargin: 4
            spacing: 4

            Label {
                id: blockHeaderLabel

                Layout.fillWidth: true
                text: {
                    if (!root.blockInformation)
                        return ""
                    if (root.shape === "hat")
                        return qsTr("search: %1").arg(root.blockInformation.label)
                    if (root.shape === "container")
                        return qsTr("mutate %1").arg(root.blockInformation.label)
                    if (root.shape === "stack")
                        return qsTr("op: %1").arg(root.blockInformation.label)
                    return qsTr("evaluate: %1").arg(root.blockInformation.label)
                }
                font.weight: Font.DemiBold
                elide: Text.ElideRight

                HoverHandler {
                    id: blockLabelHover
                }
                ToolTip.visible: blockLabelHover.hovered && truncated
                ToolTip.delay: 350
                ToolTip.text: root.blockInformation
                              ? root.blockInformation.label : ""
            }

            ThemeControls.ThemedToolButton {
                objectName: "blockUp" + root.blockId
                visible: root.movable
                text: "↑"
                enabled: !root.controller.running && root.moveIndex > 0
                onClicked: root.controller.moveBlock(root.blockId,
                                                     root.moveIndex - 1)
                ToolTip.visible: hovered
                ToolTip.text: root.isContainer
                              ? qsTr("Run this window earlier")
                              : qsTr("Run this operation earlier")
            }

            ThemeControls.ThemedToolButton {
                objectName: "blockDown" + root.blockId
                visible: root.movable
                text: "↓"
                enabled: !root.controller.running
                         && root.moveIndex + 1 < root.moveCount
                onClicked: root.controller.moveBlock(root.blockId,
                                                     root.moveIndex + 1)
                ToolTip.visible: hovered
                ToolTip.text: root.isContainer
                              ? qsTr("Run this window later")
                              : qsTr("Run this operation later")
            }

            ThemeControls.ThemedToolButton {
                objectName: "blockRemove" + root.blockId
                visible: root.shape !== "hat"
                text: "×"
                enabled: !root.controller.running
                onClicked: root.controller.removeBlock(root.blockId)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Remove this block")
            }
        }
    }

    // Typed value slots, grouped by the schema's group labels.
    ColumnLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 10
        spacing: 4

        Repeater {
            model: root.groupedFields

            delegate: ColumnLayout {
                required property var modelData

                Layout.fillWidth: true
                spacing: 4

                Label {
                    visible: modelData.name.length > 0
                    text: modelData.name
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    color: ThemeControls.AppTheme.textMuted
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 6

                    Repeater {
                        model: modelData.fields

                        delegate: Blocks.BlockSlot {
                            required property var modelData

                            controller: root.controller
                            blockId: root.blockId
                            fieldData: modelData
                            armedSlot: root.armedSlot
                            onArmRequested: (blockId, key) =>
                                root.requestArm(blockId, key)
                        }
                    }
                }
            }
        }
    }

    // Evaluator socket: the search hat scores runs with exactly one
    // evaluation block. A separate component so this file never
    // instantiates itself.
    Blocks.HatSockets {
        visible: root.isScriptHat
        Layout.fillWidth: true
        Layout.leftMargin: 12
        controller: root.controller
        viewer: root.viewer
        viewport: root.viewport
        armedSlot: root.armedSlot
        hatInformation: root.blockInformation
        onArmRequested: (blockId, key) => root.requestArm(blockId, key)
    }

    Label {
        visible: root.isScriptHat
        Layout.leftMargin: 12
        text: qsTr("apply input mutations")
        color: ThemeControls.AppTheme.textMuted
        font.italic: true
        font.pixelSize: 11
    }

    Blocks.BlockSequence {
        visible: root.isScriptHat || root.isContainer
        Layout.fillWidth: true
        Layout.leftMargin: root.isScriptHat ? 12 : 28
        controller: root.controller
        viewer: root.viewer
        viewport: root.viewport
        armedSlot: root.armedSlot
        blockIds: root.substackIds
        emptyHint: root.isScriptHat
                    ? qsTr("Drag a mutation window here, then put mutation "
                           + "blocks inside it to define how inputs change "
                           + "each iteration.")
                    : qsTr("Drag mutation blocks here to run inside this "
                           + "window.")
        onArmRequested: (blockId, key) => root.requestArm(blockId, key)
    }
}
