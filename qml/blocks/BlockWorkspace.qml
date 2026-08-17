import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls
import "." as Blocks

// Scratch-like composition editor: a palette of categorized blocks above
// the single search script. Clicking a search block replaces the script
// hat, an evaluation block replaces the evaluator slot, a mutation window
// appends to the script, a mutation block snaps into the last window, and
// value blocks plug into the armed number slot. Rich collection editors
// load as detail panels under the evaluator block.
ColumnLayout {
    id: root

    property var controller
    property var viewer
    property var viewport

    readonly property var palette: controller ? controller.blockPalette : []
    readonly property var script: controller ? controller.blockScript : null
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
                model: root.palette

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
        }

        StackLayout {
            id: paletteStack

            objectName: "paletteStack"
            Layout.fillWidth: true
            currentIndex: 0

            Repeater {
                model: root.palette

                delegate: Flow {
                    id: paletteCategoryFlow

                    required property var modelData

                    spacing: 4

                    Repeater {
                        model: modelData.blocks

                        delegate: ThemeControls.ThemedButton {
                            required property var modelData

                            objectName: "paletteBlock_"
                                        + root.sanitizedId(modelData.id)
                            text: modelData.label
                            flat: true
                            enabled: !root.controller.running
                            onClicked: root.activatePaletteBlock(
                                           modelData)

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

    // ---- Script ----

    Rectangle {
        objectName: "blockScript"
        Layout.fillWidth: true
        implicitHeight: scriptColumn.implicitHeight + 16
        radius: 10
        color: ThemeControls.AppTheme.surfaceAlternate
        border.width: 1
        border.color: ThemeControls.AppTheme.border

        ColumnLayout {
            id: scriptColumn

            anchors.fill: parent
            anchors.margins: 8
            spacing: 8

            Blocks.BlockView {
                id: hatView

                objectName: "blockViewHat"
                Layout.fillWidth: true
                controller: root.controller
                viewer: root.viewer
                viewport: root.viewport
                blockId: root.script ? root.script.hat : 0
                armedSlot: root.armedSlot
                onArmRequested: (blockId, key) => root.requestArm(blockId,
                                                                  key)
            }

            Label {
                Layout.leftMargin: 10
                visible: evaluatorView.blockId !== 0
                text: qsTr("scored by")
                color: ThemeControls.AppTheme.textMuted
                font.italic: true
                font.pixelSize: 11
            }

            Blocks.BlockView {
                id: evaluatorView

                objectName: "blockViewEvaluator"
                Layout.fillWidth: true
                Layout.leftMargin: 12
                controller: root.controller
                viewer: root.viewer
                viewport: root.viewport
                blockId: root.script ? root.script.evaluator : 0
                armedSlot: root.armedSlot
                onArmRequested: (blockId, key) => root.requestArm(blockId,
                                                                  key)
            }

            // Rich detail panel for collection-managed evaluators.
            Loader {
                id: evaluatorDetail

                objectName: "blockViewEvaluatorDetail"
                Layout.fillWidth: true
                Layout.leftMargin: 12
                readonly property string detailComponent:
                    evaluatorView.blockInformation
                    && evaluatorView.blockInformation.detail
                    ? evaluatorView.blockInformation.detail : ""
                active: detailComponent.length > 0
                source: detailComponent
                function injectDetail() {
                    if (!item)
                        return
                    item.controller = root.controller
                    item.viewer = root.viewer
                    item.viewport = root.viewport
                    item.blockId = evaluatorView.blockId
                    item.blockInformation = evaluatorView.blockInformation
                }
                onLoaded: injectDetail()
                Connections {
                    target: evaluatorView
                    ignoreUnknownSignals: true
                    function onBlockIdChanged() {
                        evaluatorDetail.injectDetail()
                    }
                }
            }

            Label {
                Layout.leftMargin: 10
                visible: mutatorRepeater.count > 0
                text: qsTr("apply input mutations")
                color: ThemeControls.AppTheme.textMuted
                font.italic: true
                font.pixelSize: 11
            }

            Repeater {
                id: mutatorRepeater

                objectName: "blockMutators"
                model: root.script ? root.script.groups : []

                delegate: ColumnLayout {
                    id: mutationWindow

                    required property var modelData
                    required property int index

                    objectName: "blockWindow" + modelData.blockId
                    Layout.fillWidth: true
                    spacing: 4

                    Blocks.BlockView {
                        id: windowView

                        objectName: "blockView" + mutationWindow.modelData.blockId
                        Layout.fillWidth: true
                        controller: root.controller
                        viewer: root.viewer
                        viewport: root.viewport
                        blockId: mutationWindow.modelData.blockId
                        armedSlot: root.armedSlot
                        moveIndex: mutationWindow.index
                        moveCount: mutatorRepeater.count
                        onArmRequested: (blockId, key) =>
                            root.requestArm(blockId, key)
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 28
                        spacing: 4

                        Repeater {
                            id: atomRepeater

                            model: mutationWindow.modelData.atoms

                            delegate: Blocks.BlockView {
                                id: atomView

                                required property var modelData
                                required property int index

                                objectName: "blockView" + modelData
                                Layout.fillWidth: true
                                controller: root.controller
                                viewer: root.viewer
                                viewport: root.viewport
                                blockId: modelData
                                armedSlot: root.armedSlot
                                moveIndex: index
                                moveCount: atomRepeater.count
                                onArmRequested: (blockId, key) =>
                                    root.requestArm(blockId, key)
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: mutationWindow.modelData.atoms.length === 0
                            wrapMode: Text.WordWrap
                            text: qsTr("Add mutation blocks from the palette "
                                       + "to run inside this window.")
                            color: ThemeControls.AppTheme.textMuted
                            font.pixelSize: 11
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: root.script && root.script.groups.length === 0
                wrapMode: Text.WordWrap
                text: qsTr("Add a mutation window from the palette, then put "
                           + "mutation blocks inside it to define how inputs "
                           + "change each iteration.")
                color: ThemeControls.AppTheme.textMuted
                font.pixelSize: 11
            }
        }
    }

    function activatePaletteBlock(block) {
        if (block.optionKind === "" && block.shape === "reporter") {
            if (!root.armedSlot) {
                root.showPaletteHint(qsTr(
                    "Value blocks plug into a number slot: click the "
                    + "\u2295 button next to the slot first, then pick "
                    + "the value block."))
                return
            }
            root.controller.attachReporter(
                        root.armedSlot.blockId, root.armedSlot.key, block.id)
            root.armedSlot = null
            return
        }
        root.controller.addBlock(block.id)
    }
}
