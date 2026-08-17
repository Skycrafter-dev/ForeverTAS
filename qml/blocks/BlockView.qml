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
    // Canvas state: only the script hat renders the evaluator socket;
    // loose blocks (parked on the canvas, not wired into the script)
    // render muted and drag by their card.
    property bool isScriptHat: false
    property bool canvasLoose: false
    property bool canvasIsLoose: false
    // The owning canvas; wired through sequences, sockets, and slots so
    // drags and drop highlighting work at any depth.
    property var canvas: null

    // Emitted when a slot wants to (dis)arm for reporter attachment.
    signal armRequested(int blockId, string key)

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
    readonly property string headerText: {
        if (!blockInformation)
            return ""
        if (shape === "hat")
            return qsTr("search: %1").arg(blockInformation.label)
        if (shape === "container")
            return qsTr("mutate %1").arg(blockInformation.label)
        if (shape === "stack")
            return qsTr("op: %1").arg(blockInformation.label)
        return qsTr("evaluate: %1").arg(blockInformation.label)
    }
    readonly property bool isContainer: shape === "container"
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

    readonly property real dragOpacity:
        canvas && canvas.draggingBlockId === root.blockId ? 0.0 : 1.0

    Rectangle {
        id: blockCard

        Layout.fillWidth: true
        implicitHeight: headerRow.implicitHeight + 12
        radius: 10
        opacity: (root.canvasLoose ? 0.85 : 1.0) * root.dragOpacity
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
            enabled: root.canvas !== null && root.controller
                      && !root.controller.running && root.blockId !== 0
            preventStealing: true
            cursorShape: enabled ? Qt.SizeAllCursor : Qt.ArrowCursor
            hoverEnabled: enabled

            property bool moved: false

            onPressed: (mouse) => {
                moved = false
                if (root.canvas)
                    root.canvas.dragPressed(root, cardDragArea, mouse)
            }
            onPositionChanged: (mouse) => {
                if (!pressed || !root.canvas)
                    return
                moved = true
                root.canvas.dragMoved(cardDragArea, mouse)
            }
            onReleased: {
                if (moved && root.canvas)
                    root.canvas.dragReleased()
                moved = false
            }
            onCanceled: {
                if (moved && root.canvas)
                    root.canvas.dragReleased()
                moved = false
            }
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
                text: root.headerText
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
                            canvas: root.canvas
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
        canvas: root.canvas
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
        canvas: root.canvas
        ownerBlockId: root.blockId
        ownerIsHat: root.isScriptHat
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
