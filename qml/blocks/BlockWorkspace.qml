import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls
import "." as Blocks

// Scratch-like composition editor: a palette of categorized blocks above
// a free canvas. The script and any loose blocks live wherever the user
// puts them; palette clicks keep the smart-attach policy (clicking a
// search block replaces the script hat, an evaluation block replaces the
// evaluator, a mutation window appends to the script, a mutation block
// snaps into the last window) while value blocks are placed on the
// canvas to be dragged into number slots.
ColumnLayout {
    id: root

    property var controller
    property var viewer
    property var viewport
    // Full-window composition mode; the workspace reparents over the
    // whole window while expanded.
    property bool expanded: false

    signal expansionRequested(bool expanded)

    readonly property var paletteModel: controller ? controller.blockPalette : []
    property var armedSlot: null
    property string paletteHint: ""

    spacing: 8

    Timer {
        id: paletteHintTimer
        interval: 6000
        onTriggered: root.paletteHint = ""
    }

    function showPaletteHint(text) {
        paletteHint = text
        paletteHintTimer.restart()
    }

    function sanitizedId(definitionId) {
        return definitionId.split("/").join("_")
    }

    function requestArm(blockId, key) {
        paletteHint = ""
        paletteHintTimer.stop()
        if (armedSlot && armedSlot.blockId === blockId
                && armedSlot.key === key)
            armedSlot = null
        else
            armedSlot = { "blockId": blockId, "key": key }
    }

    Connections {
        target: root.controller

        function onBlockStructureChanged() {
            root.armedSlot = null
        }

        function onRunningChanged() {
            if (root.controller.running)
                root.showPaletteHint(qsTr(
                    "The search is running; block editing resumes "
                    + "after it stops."))
        }
    }

    // ---- Palette ----

    ColumnLayout {
        id: paletteBar

        objectName: "blockPalette"
        Layout.fillWidth: true
        spacing: 6

        RowLayout {
            id: paletteCategories

            objectName: "paletteCategories"
            Layout.fillWidth: true
            spacing: 4

            Repeater {
                model: root.paletteModel

                delegate: ThemeControls.ThemedTabButton {
                    id: paletteTab

                    required property var modelData
                    required property int index

                    objectName: "paletteCategory_" + modelData.id
                    text: modelData.label
                    enabled: !root.controller.running

                    // Re-clicking the active tab severs a `checked:`
                    // binding and would leave it looking deselected;
                    // sync imperatively instead.
                    function synchronize() {
                        checked = paletteStack.currentIndex === index
                    }

                    Component.onCompleted: synchronize()
                    Connections {
                        target: paletteStack
                        function onCurrentIndexChanged() {
                            paletteTab.synchronize()
                        }
                    }
                    onClicked: paletteStack.currentIndex = index
                }
            }

            ThemeControls.ThemedToolButton {
                objectName: "blockWorkspaceExpandButton"
                text: root.expanded ? "\u2715" : "\u26f6"
                enabled: !root.controller.running
                onClicked: root.expansionRequested(!root.expanded)
                ToolTip.visible: hovered
                ToolTip.text: root.expanded
                                ? qsTr("Exit full-screen composition")
                                : qsTr("Compose on the full window")
            }
        }

        StackLayout {
            id: paletteStack

            objectName: "paletteStack"
            Layout.fillWidth: true
            currentIndex: 0

            Repeater {
                model: root.paletteModel

                delegate: Flow {
                    id: paletteCategoryFlow

                    required property var modelData

                    spacing: 4

                    Repeater {
                        model: modelData.blocks

                        delegate: ThemeControls.ThemedButton {
                            id: paletteDragButton

                            required property var modelData

                            objectName: "paletteBlock_"
                                        + root.sanitizedId(modelData.id)
                            text: modelData.label
                            flat: true
                            enabled: !root.controller.running
                            onClicked: root.activatePaletteBlock(
                                           modelData)

                            // Press-drag drops the block straight onto
                            // the canvas; a plain click keeps the
                            // smart-attach behavior.
                            MouseArea {
                                id: paletteDragArea

                                anchors.fill: parent
                                enabled: !root.controller.running
                                preventStealing: true
                                hoverEnabled: enabled

                                property point pressPoint
                                property bool moved: false

                                onPressed: (mouse) => {
                                    pressPoint = Qt.point(mouse.x, mouse.y)
                                    moved = false
                                }
                                onPositionChanged: (mouse) => {
                                    if (!pressed)
                                        return
                                    if (!moved) {
                                        if (Math.abs(mouse.x - pressPoint.x) < 5
                                                && Math.abs(mouse.y - pressPoint.y) < 5)
                                            return
                                        moved = true
                                        canvas.beginPaletteDrag(
                                                    paletteDragButton
                                                            .modelData,
                                                    paletteCategoryFlow
                                                            .modelData.color,
                                                    paletteDragArea)
                                    }
                                    canvas.movePaletteDrag(
                                                paletteDragArea, mouse.x,
                                                mouse.y)
                                }
                                onReleased: {
                                    if (moved)
                                        canvas.endPaletteDrag()
                                    else
                                        root.activatePaletteBlock(
                                                    paletteDragButton
                                                            .modelData)
                                }
                                onCanceled: {
                                    if (moved)
                                        canvas.endPaletteDrag()
                                }
                            }

                            Rectangle {
                                anchors.left: parent.left
                                anchors.leftMargin: 2
                                anchors.top: parent.top
                                anchors.topMargin: 4
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 4
                                width: 3
                                radius: 1
                                color: paletteCategoryFlow.modelData.color
                            }
                        }
                    }
                }
            }
        }
        Label {
            objectName: "blockPaletteHint"
            Layout.fillWidth: true
            visible: root.paletteHint.length > 0
            text: root.paletteHint
            color: ThemeControls.AppTheme.warning
            wrapMode: Text.WordWrap
            font.pixelSize: 11
        }
    }

    // ---- Canvas ----

    Blocks.BlockCanvas {
        id: canvas

        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 340
        Layout.preferredHeight: root.expanded ? -1 : 460
        controller: root.controller
        viewer: root.viewer
        viewport: root.viewport
        armedSlot: root.armedSlot
        onArmRequested: (blockId, key) => root.requestArm(blockId, key)
    }

    function activatePaletteBlock(block) {
        if (block.optionKind === "" && block.shape === "reporter") {
            if (root.armedSlot) {
                root.controller.attachReporter(
                            root.armedSlot.blockId, root.armedSlot.key,
                            block.id)
                root.armedSlot = null
                return
            }
            const position = canvas.nextDropPosition()
            const created = root.controller.addLooseBlock(
                        block.id, position.x, position.y)
            if (created !== 0)
                root.showPaletteHint(qsTr(
                    "Placed on the canvas — drag it into a number slot "
                    + "to use it."))
            return
        }
        root.controller.addBlock(block.id)
    }
}
