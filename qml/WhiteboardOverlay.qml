import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import ForeverTAS.Viewer 1.0

Item {
    id: root

    required property var model
    property bool available: false
    readonly property real boardTop: 52
    readonly property real toolbarRight: toolbar.x + toolbar.width
    readonly property real toolbarBottom: toolbar.y + toolbar.height
    property int editingIndex: -2
    property string editingFingerprint: ""
    property real pendingTextX: 0
    property real pendingTextY: 0
    property bool drawingListOpen: false
    property real toolbarMaximumWidth: width - 28
    property bool imageExportInProgress: false
    property int pendingImageBoardIndex: -1
    property string pendingImageMode: ""
    readonly property color toolbarControlText:
        AppTheme.dark ? AppTheme.viewerOverlayText : AppTheme.text
    property var captureViewpoint: function() { return ({}) }
    property var restoreViewpoint: function(board) {}
    property var exportBackgroundImage: function(index, fileUrl) {
        return false
    }

    function chooseImageExport(index, mode) {
        if (imageExportInProgress
                || index < 0 || index >= model.boardCount)
            return
        pendingImageBoardIndex = index
        pendingImageMode = mode
        imageExportDialog.open()
    }

    function textEntryFingerprint(index) {
        if (index < 0 || index >= model.items.length)
            return ""
        const item = model.items[index]
        return item.type + ":" + item.text + ":" + item.x + ":" + item.y
    }

    function beginTextEntry(index, value, normalizedX, normalizedY) {
        // Commit any in-progress entry first: starting a second edit
        // must never discard what was already typed.
        if (textEditor.visible)
            commitTextEntry()
        editingIndex = index
        editingFingerprint = textEntryFingerprint(index)
        pendingTextX = normalizedX
        pendingTextY = normalizedY
        textEditor.text = value
        textEditor.visible = true
        textEditor.x = Math.max(8, Math.min(
                                    boardArea.width - textEditor.width - 8,
                                    normalizedX * boardArea.width))
        textEditor.y = Math.max(8, Math.min(
                                    boardArea.height - textEditor.height - 8,
                                    normalizedY * boardArea.height))
        Qt.callLater(function() {
            textEditor.forceActiveFocus()
            textEditor.selectAll()
        })
    }

    function commitTextEntry() {
        if (!textEditor.visible)
            return
        const value = textEditor.text.trim()
        if (value.length > 0) {
            if (editingIndex >= 0) {
                // The item list may have changed while editing; only
                // write when the item under the recorded index is still
                // the one the edit started on.
                if (editingIndex < model.items.length
                        && textEntryFingerprint(editingIndex)
                        === editingFingerprint)
                    model.setText(editingIndex, value)
            } else {
                model.addText(pendingTextX, pendingTextY, value)
            }
        }
        cancelTextEntry()
    }

    function cancelTextEntry() {
        textEditor.visible = false
        editingIndex = -2
        boardArea.forceActiveFocus()
    }

    function focusBoard(index) {
        if (!model.selectBoard(index))
            return
        const board = model.boards[index]
        if (board)
            restoreViewpoint(board)
    }

    function clampTextEditor() {
        if (!textEditor.visible)
            return
        textEditor.x = Math.max(
                    8, Math.min(boardArea.width - textEditor.width - 8,
                                textEditor.x))
        textEditor.y = Math.max(
                    8, Math.min(boardArea.height - textEditor.height - 8,
                                textEditor.y))
    }

    onWidthChanged: clampTextEditor()
    onHeightChanged: clampTextEditor()

    component WhiteboardToolButton: ThemedButton {
        id: toolButton

        required property string toolId
        required property string label
        required property real buttonWidth

        objectName: "whiteboardToolButton_" + toolId
        visible: root.model.active
        height: 32
        width: Math.max(buttonWidth, implicitWidth + 4)
        checkable: true
        elideText: false
        text: label

        // Toggling severs a `checked:` binding (re-clicking the active
        // tool would look unchecked while staying active), so sync from
        // the model imperatively.
        function synchronize() {
            checked = root.model.tool === toolButton.toolId
        }

        Component.onCompleted: synchronize()
        Connections {
            target: root.model
            function onToolChanged() {
                toolButton.synchronize()
            }
        }
        onClicked: root.model.tool = toolId
    }

    onAvailableChanged: {
        if (!available && model.active)
            model.active = false
    }

    // Keyboard shortcut for the drawings list, which is a floating panel
    // rather than a Popup.
    Keys.onEscapePressed: drawingListOpen = false

    Connections {
        target: root.model

        function onActiveChanged() {
            if (!root.model.active) {
                // Keep what was typed instead of discarding it.
                root.commitTextEntry()
                colorPopup.close()
            }
            modeToggle.synchronize()
        }
    }

    Item {
        id: boardArea
        objectName: "whiteboardBoardArea"
        anchors.top: parent.top
        anchors.topMargin: root.boardTop
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true

        MouseArea {
            id: drawingInput
            objectName: "whiteboardDrawingInput"
            anchors.fill: parent
            z: 5
            enabled: root.model.active
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            cursorShape: root.model.tool === "eraser"
                         ? Qt.CrossCursor
                         : root.model.tool === "select"
                           ? Qt.SizeAllCursor : Qt.ArrowCursor
            property real lastBoardX: 0
            property real lastBoardY: 0

            onPressed: mouse => {
                lastBoardX = mouse.x
                lastBoardY = mouse.y
                if (root.model.tool === "text") {
                    root.beginTextEntry(
                                -1,
                                "",
                                mouse.x / Math.max(1, width),
                                mouse.y / Math.max(1, height))
                } else if (root.model.tool === "select"
                           || root.model.tool === "eraser") {
                    const index = root.model.itemAt(
                                    mouse.x / Math.max(1, width),
                                    mouse.y / Math.max(1, height))
                    if (index >= 0) {
                        root.model.selectItem(index)
                        if (root.model.tool === "eraser") {
                            root.model.eraseSelected(
                                        mouse.x / Math.max(1, width),
                                        mouse.y / Math.max(1, height),
                                        root.model.size
                                        / Math.max(1, Math.min(width, height)))
                        }
                    } else {
                        root.model.clearSelection()
                    }
                } else {
                    root.model.beginItem(
                                mouse.x / Math.max(1, width),
                                mouse.y / Math.max(1, height))
                }
            }
            onPositionChanged: mouse => {
                if (!(mouse.buttons & Qt.LeftButton))
                    return
                if (root.model.drawing) {
                    root.model.updateItem(
                                mouse.x / Math.max(1, width),
                                mouse.y / Math.max(1, height))
                } else if (root.model.selectedIndex >= 0
                           && root.model.tool === "select") {
                    root.model.moveSelected(
                                (mouse.x - lastBoardX) / Math.max(1, width),
                                (mouse.y - lastBoardY) / Math.max(1, height))
                } else if (root.model.selectedIndex >= 0
                           && root.model.tool === "eraser") {
                    root.model.eraseSelected(
                                mouse.x / Math.max(1, width),
                                mouse.y / Math.max(1, height),
                                root.model.size
                                / Math.max(1, Math.min(width, height)))
                }
                lastBoardX = mouse.x
                lastBoardY = mouse.y
            }
            onReleased: {
                if (root.model.drawing)
                    root.model.finishItem()
            }
            onCanceled: root.model.cancelItem()
            onDoubleClicked: mouse => {
                if (root.model.tool !== "select")
                    return
                const index = root.model.itemAt(
                                mouse.x / Math.max(1, width),
                                mouse.y / Math.max(1, height))
                const item = index >= 0 ? root.model.items[index] : null
                if (item && item.type === "text") {
                    root.beginTextEntry(index, item.text, item.x, item.y)
                }
            }
        }

        Repeater {
            id: drawingRepeater
            objectName: "whiteboardDrawingRepeater"
            model: root.model.items

            delegate: Item {
                id: drawingDelegate
                objectName: "whiteboardDrawingItem"
                required property var modelData
                required property int index

                x: modelData.x * boardArea.width
                y: modelData.y * boardArea.height
                width: Math.max(2, modelData.width * boardArea.width)
                height: Math.max(2, modelData.height * boardArea.height)
                z: modelData.selected ? 2 : 1

                WhiteboardCanvasItem {
                    objectName: "whiteboardCanvasItem"
                    anchors.fill: parent
                    drawing: drawingDelegate.modelData
                }

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -3
                    color: "transparent"
                    border.width: 1
                    border.color: AppTheme.focus
                    visible: root.model.active
                             && drawingDelegate.modelData.selected
                             && root.model.tool !== "eraser"
                }

            }
        }

        Rectangle {
            id: resizeHandle
            objectName: "whiteboardResizeHandle"
            width: 13
            height: 13
            x: root.model.selectedIndex >= 0
               ? (root.model.selectedItem.x + root.model.selectedItem.width)
                 * boardArea.width - width / 2
               : 0
            y: root.model.selectedIndex >= 0
               ? (root.model.selectedItem.y + root.model.selectedItem.height)
                 * boardArea.height - height / 2
               : 0
            radius: 2
            color: AppTheme.focus
            border.width: 1
            border.color: AppTheme.text
            visible: root.model.active
                     && root.model.selectedIndex >= 0
                     && root.model.tool === "select"
            z: 7

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                cursorShape: Qt.SizeFDiagCursor
                property real lastBoardX: 0
                property real lastBoardY: 0
                onPressed: mouse => {
                    const point = resizeHandle.mapToItem(
                                    boardArea, mouse.x, mouse.y)
                    lastBoardX = point.x
                    lastBoardY = point.y
                }
                onPositionChanged: mouse => {
                    if (!(mouse.buttons & Qt.LeftButton))
                        return
                    const point = resizeHandle.mapToItem(
                                    boardArea, mouse.x, mouse.y)
                    root.model.resizeSelected(
                                (point.x - lastBoardX)
                                / Math.max(1, boardArea.width),
                                (point.y - lastBoardY)
                                / Math.max(1, boardArea.height))
                    lastBoardX = point.x
                    lastBoardY = point.y
                }
            }
        }

        TextField {
            id: textEditor
            objectName: "whiteboardTextEditor"
            z: 12
            width: Math.max(60, Math.min(300, boardArea.width - 16))
            height: 42
            visible: false
            placeholderText: qsTr("Annotation text")
            color: AppTheme.viewerOverlayText
            selectByMouse: true
            maximumLength: 500
            onAccepted: root.commitTextEntry()
            onEditingFinished: {
                if (visible)
                    root.commitTextEntry()
            }
            Keys.onEscapePressed: root.cancelTextEntry()

            background: Rectangle {
                radius: 4
                color: AppTheme.viewerOverlayStrong
                border.width: 1
                border.color: textEditor.activeFocus
                              ? AppTheme.focus : AppTheme.viewerOverlayBorder
            }
        }
    }

    Rectangle {
        id: toolbar
        objectName: "whiteboardToolbar"
        readonly property real primaryPreferredContentWidth:
            root.model.active
                ? primaryLeadingControls.width
                    + 4 + primaryTrailingControls.width
                : modeToggle.width + 4 + inactiveListButton.width
        readonly property real secondaryPreferredContentWidth:
            root.model.active
                ? secondaryLeadingControls.width
                    + 4 + secondaryTrailingControls.width
                : 0
        readonly property real preferredContentWidth:
            Math.max(primaryPreferredContentWidth,
                     secondaryPreferredContentWidth)
        x: 14
        y: root.boardTop + 10
        z: 20
        width: Math.min(
                   parent.width - 28,
                   root.toolbarMaximumWidth,
                   preferredContentWidth + 12)
        height: root.model.active
                ? toolbarContent.height + 12 : 46
        radius: 6
        color: AppTheme.viewerOverlayStrong
        border.width: 1
        border.color: root.model.active
                      ? (AppTheme.dark ? "#8b978d" : "#758176")
                      : AppTheme.viewerOverlayBorder
        clip: true

        Item {
            id: toolbarContent
            objectName: "whiteboardToolbarContent"
            width: toolbar.width - 12
            height: primaryToolbarRow.height
                    + (secondaryToolbarRow.visible
                       ? 4 + secondaryToolbarRow.height : 0)
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter

            Item {
                id: primaryToolbarRow
                objectName: "whiteboardPrimaryToolbarRow"
                width: parent.width
                height: !root.model.active
                        || width >= toolbar.primaryPreferredContentWidth
                        ? 34 : 72

                Row {
                    id: primaryLeadingControls
                    spacing: 4

                    ThemedButton {
                        id: modeToggle
                        objectName: "whiteboardModeToggle"
                        contentObjectName: "whiteboardModeToggleLabel"
                        width: Math.max(98, implicitWidth + 4)
                        height: 34
                        checkable: true
                        enabled: root.available
                        elideText: false
                        text: qsTr("Whiteboard")

                        // Toggling severs a `checked:` binding; sync from
                        // the model so forced deactivation (map unload)
                        // still clears the pressed look.
                        function synchronize() {
                            checked = root.model.active
                        }

                        Component.onCompleted: synchronize()
                        onToggled: root.model.active = checked

                        ToolTip.visible: hovered
                        ToolTip.delay: 350
                        ToolTip.text: root.available
                                      ? qsTr("Draw over the 3D viewer")
                                      : qsTr("Load a map to use the whiteboard")
                    }

                    Item {
                        visible: root.model.active
                        width: 1
                        height: 34

                        Rectangle {
                            anchors.fill: parent
                            anchors.topMargin: 5
                            anchors.bottomMargin: 5
                            color: AppTheme.viewerOverlayBorder
                        }
                    }

                    WhiteboardToolButton {
                        toolId: "select"
                        label: qsTr("Select")
                        buttonWidth: 56
                    }
                    WhiteboardToolButton {
                        toolId: "pen"
                        label: qsTr("Pen")
                        buttonWidth: 48
                    }
                    WhiteboardToolButton {
                        toolId: "line"
                        label: qsTr("Line")
                        buttonWidth: 48
                    }

                }

                ThemedButton {
                    id: inactiveListButton
                    objectName: "whiteboardInactiveListButton"
                    visible: !root.model.active
                    x: modeToggle.width + 4
                    width: Math.max(76, implicitWidth + 4)
                    height: 32
                    elideText: false
                    text: qsTr("Drawings")
                    onClicked:
                        root.drawingListOpen = !root.drawingListOpen
                }

                Row {
                    id: primaryTrailingControls
                    x: root.model.active
                       && primaryToolbarRow.width >=
                          toolbar.primaryPreferredContentWidth
                       ? primaryLeadingControls.width + 4 : 0
                    y: root.model.active
                       && primaryToolbarRow.width <
                          toolbar.primaryPreferredContentWidth
                       ? 38 : 0
                    spacing: 4

                    WhiteboardToolButton {
                        toolId: "rectangle"
                        label: qsTr("Rect")
                        buttonWidth: 48
                    }
                    WhiteboardToolButton {
                        toolId: "ellipse"
                        label: qsTr("Ellipse")
                        buttonWidth: 58
                    }
                    WhiteboardToolButton {
                        toolId: "text"
                        label: qsTr("Text")
                        buttonWidth: 48
                    }
                    WhiteboardToolButton {
                        toolId: "eraser"
                        label: qsTr("Erase")
                        buttonWidth: 52
                    }
                }
            }

            Item {
                id: secondaryToolbarRow
                objectName: "whiteboardSecondaryToolbarRow"
                visible: root.model.active
                width: parent.width
                y: primaryToolbarRow.height + 4
                height: width >= toolbar.secondaryPreferredContentWidth
                        ? 34 : 72

                Row {
                    id: secondaryLeadingControls
                    spacing: 4

                    ThemedToolButton {
                        id: colorButton
                        objectName: "whiteboardColorButton"
                        visible: root.model.active
                        width: 34
                        height: 34
                        Accessible.name: qsTr("Drawing color")
                        onClicked: colorPopup.open()

                        contentItem: Rectangle {
                            width: 18
                            height: 18
                            radius: 3
                            anchors.centerIn: parent
                            color: root.model.color
                            border.width: 1
                            border.color: AppTheme.viewerOverlayMuted
                        }

                        ToolTip.visible: hovered
                        ToolTip.delay: 350
                        ToolTip.text: qsTr("Drawing color")
                    }

                    Label {
                        visible: root.model.active
                        height: 34
                        text: qsTr("Size")
                        color: AppTheme.viewerOverlayMuted
                        font.pixelSize: 11
                        verticalAlignment: Text.AlignVCenter
                    }

                    ThemedSlider {
                        id: sizeSlider
                        objectName: "whiteboardSizeSlider"
                        visible: root.model.active
                        width: 70
                        height: 34
                        Accessible.name: qsTr("Drawing size")
                        from: 1
                        to: 24
                        stepSize: 1
                        value: root.model.size
                        onMoved: root.model.size = value
                        ToolTip.visible: hovered || pressed
                        ToolTip.text: qsTr("%1 px").arg(Math.round(value))
                    }

                    SliderValueField {
                        objectName: "whiteboardSizeSliderValueField"
                        visible: root.model.active
                        width: 62
                        height: 34
                        value: root.model.size.toString()
                        from: sizeSlider.from
                        to: sizeSlider.to
                        maximumEnabled: false
                        integer: true
                        suffix: qsTr("px")
                        accessibleName: qsTr("Drawing size exact value")
                        fieldColor: AppTheme.viewerOverlayControl
                        fieldDisabledColor: AppTheme.viewerOverlayControl
                        fieldBorderColor: AppTheme.viewerOverlayBorder
                        fieldTextColor: AppTheme.viewerOverlayText
                        fieldDisabledTextColor: AppTheme.viewerOverlayMuted
                        onEdited: value => root.model.size = Number(value)
                    }
                }

                Row {
                    id: secondaryTrailingControls
                    x: secondaryToolbarRow.width >=
                       toolbar.secondaryPreferredContentWidth
                       ? secondaryLeadingControls.width + 4 : 0
                    y: secondaryToolbarRow.width <
                       toolbar.secondaryPreferredContentWidth ? 38 : 0
                    spacing: 4

                    ThemedToolButton {
                        objectName: "whiteboardDeleteButton"
                        visible: root.model.active
                        enabled: root.model.selectedIndex >= 0
                        width: 34
                        height: 34
                        Accessible.name: qsTr("Delete selected item")
                        text: "\u00d7"
                        font.pixelSize: 22
                        onClicked: root.model.removeSelected()
                        ToolTip.visible: hovered
                        ToolTip.delay: 350
                        ToolTip.text: qsTr("Delete selected item")
                    }

                    ThemedButton {
                        objectName: "whiteboardPickUpButton"
                        visible: root.model.active
                        enabled: root.model.count === 0
                                 && root.model.selectedBoardIndex >= 0
                        width: Math.max(66, implicitWidth + 4)
                        height: 32
                        elideText: false
                        text: qsTr("Pick up")
                        onClicked: root.model.pickUpBoard(
                                       root.model.selectedBoardIndex)
                        ToolTip.visible: hovered
                        ToolTip.delay: 350
                        ToolTip.text: qsTr(
                                          "Pick up the selected drawing so it can be placed again")
                    }

                    ThemedButton {
                        objectName: "whiteboardPlaceButton"
                        visible: root.model.active
                        enabled: root.model.count > 0
                                 && root.model.mapKey.length > 0
                                 && root.model.mapName.length > 0
                        width: Math.max(58, implicitWidth + 4)
                        height: 32
                        elideText: false
                        text: qsTr("Place")
                        onClicked: {
                            const index = root.model.captureCurrentBoard(
                                            qsTr("Drawing %1").arg(
                                                root.model.boardCount + 1),
                                            root.captureViewpoint())
                            if (index >= 0)
                                root.drawingListOpen = true
                        }
                        ToolTip.visible: hovered
                        ToolTip.delay: 350
                        ToolTip.text: qsTr(
                                          "Place this drawing at the current camera view")
                    }

                    ThemedButton {
                        objectName: "whiteboardActiveListButton"
                        visible: root.model.active
                        width: Math.max(72, implicitWidth + 4)
                        height: 32
                        elideText: false
                        text: qsTr("Drawings")
                        onClicked:
                            root.drawingListOpen = !root.drawingListOpen
                    }
                }
            }
        }
    }

    Rectangle {
        id: drawingList
        objectName: "whiteboardDrawingList"
        z: 25
        visible: root.drawingListOpen
        width: Math.min(310, root.toolbarMaximumWidth, root.width - 28)
        height: Math.max(0, Math.min(450, root.height - y - 12))
        x: 14
        y: root.toolbarBottom + 8
        radius: 6
        color: AppTheme.viewerOverlayStrong
        border.width: 1
        border.color: AppTheme.viewerOverlayBorder
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            RowLayout {
                Layout.fillWidth: true

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Drawings")
                    color: AppTheme.viewerOverlayText
                    font.pixelSize: 16
                    font.bold: true
                }

                ThemedToolButton {
                    objectName: "whiteboardCloseListButton"
                    text: "\u00d7"
                    font.pixelSize: 20
                    Accessible.name: qsTr("Close drawing list")
                    onClicked: root.drawingListOpen = false
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Label {
                    anchors.centerIn: parent
                    width: parent.width - 24
                    visible: root.model.boardCount === 0
                    text: qsTr("No placed drawings")
                    color: AppTheme.viewerOverlayMuted
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }

                ListView {
                    id: boardListView
                    objectName: "whiteboardBoardListView"
                    anchors.fill: parent
                    visible: root.model.boardCount > 0
                    clip: true
                    spacing: 2
                    model: root.model.boards

                    delegate: Item {
                        id: boardRow
                        required property var modelData
                        required property int index

                        width: ListView.view.width
                        height: 64

                        Rectangle {
                            anchors.fill: parent
                            color: boardRow.modelData.selected
                                   ? AppTheme.selection : "transparent"
                        }

                        ThemedButton {
                            anchors.left: parent.left
                            anchors.right: imageExportButton.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            anchors.rightMargin: 6
                            flat: true
                            enabled: boardRow.modelData.isCurrentMap
                            onClicked: root.focusBoard(boardRow.index)

                            contentItem: Column {
                                spacing: 2

                                Label {
                                    width: parent.width
                                    text: boardRow.modelData.name
                                    color: boardRow.modelData.isCurrentMap
                                           ? AppTheme.viewerOverlayText
                                           : AppTheme.viewerOverlayMuted
                                    elide: Text.ElideRight
                                }

                                Label {
                                    objectName: "whiteboardBoardMapName"
                                    width: parent.width
                                    text: boardRow.modelData.mapName
                                          + (boardRow.modelData.isCurrentMap
                                             ? qsTr(" \u00b7 Current")
                                             : "")
                                    color: AppTheme.viewerOverlayMuted
                                    font.pixelSize: 10
                                    elide: Text.ElideRight
                                    ToolTip.visible: mapNameHover.hovered
                                                     && truncated
                                    ToolTip.text: boardRow.modelData.mapName

                                    HoverHandler {
                                        id: mapNameHover
                                    }
                                }
                            }
                        }

                        ThemedToolButton {
                            id: imageExportButton
                            objectName: "whiteboardBoardImageExportButton"
                            anchors.right: visibilityToggle.left
                            anchors.rightMargin: 2
                            anchors.verticalCenter: parent.verticalCenter
                            width: 34
                            height: 34
                            enabled: !root.imageExportInProgress
                            text: "\u2193"
                            font.pixelSize: 18
                            Accessible.name: qsTr("Export %1 as an image")
                                             .arg(boardRow.modelData.name)
                            onClicked: {
                                imageExportMenu.boardIndex = boardRow.index
                                imageExportMenu.currentMap =
                                        boardRow.modelData.isCurrentMap
                                imageExportMenu.popup(
                                            imageExportButton,
                                            0,
                                            imageExportButton.height)
                            }
                            ToolTip.visible: hovered
                            ToolTip.delay: 350
                            ToolTip.text: qsTr("Export image")
                        }

                        ThemedCheckBox {
                            id: visibilityToggle
                            objectName: "whiteboardBoardVisibilityToggle"
                            anchors.right: removeBoardButton.left
                            anchors.rightMargin: 2
                            anchors.verticalCenter: parent.verticalCenter
                            width: 36
                            enabled: boardRow.modelData.isCurrentMap
                            Accessible.name: qsTr(
                                                 "Show %1 in the current map")
                                             .arg(boardRow.modelData.name)

                            // Clicking severs a `checked:` binding; sync
                            // from the boards list so imports and other
                            // updates stay reflected.
                            function synchronize() {
                                checked = boardRow.modelData.visible
                            }

                            Component.onCompleted: synchronize()
                            Connections {
                                target: root.model
                                function onBoardsChanged() {
                                    visibilityToggle.synchronize()
                                }
                            }
                            onClicked: root.model.setBoardVisible(
                                           boardRow.index, checked)
                        }

                        ThemedToolButton {
                            id: removeBoardButton
                            objectName: "whiteboardRemoveBoardButton"
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: 34
                            height: 34
                            text: "\u00d7"
                            font.pixelSize: 20
                            Accessible.name: qsTr("Delete %1")
                                             .arg(boardRow.modelData.name)
                            onClicked:
                                root.model.removeBoard(boardRow.index)
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: AppTheme.viewerOverlayBorder
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: root.model.operationMessage.length > 0
                text: root.model.operationMessage
                color: AppTheme.viewerOverlayMuted
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true

                ThemedButton {
                    objectName: "whiteboardImportButton"
                    Layout.fillWidth: true
                    elideText: false
                    text: qsTr("Import")
                    onClicked: importDialog.open()
                }

                ThemedButton {
                    objectName: "whiteboardExportButton"
                    Layout.fillWidth: true
                    enabled: root.model.boardCount > 0
                    elideText: false
                    text: qsTr("Export set")
                    onClicked: exportDialog.open()
                }
            }
        }
    }

    Menu {
        id: imageExportMenu
        objectName: "whiteboardImageExportMenu"
        property int boardIndex: -1
        property bool currentMap: false

        ThemedMenuItem {
            objectName: "whiteboardExportBackgroundMenuItem"
            text: qsTr("Image with full background")
            enabled: imageExportMenu.currentMap
                     && !root.imageExportInProgress
            onTriggered: root.chooseImageExport(
                             imageExportMenu.boardIndex, "background")
        }

        ThemedMenuItem {
            objectName: "whiteboardExportTransparentMenuItem"
            text: qsTr("Transparent drawing only")
            enabled: !root.imageExportInProgress
            onTriggered: root.chooseImageExport(
                             imageExportMenu.boardIndex, "transparent")
        }
    }

    Popup {
        id: colorPopup
        objectName: "whiteboardColorPopup"
        parent: root
        x: Math.min(root.width - width - 12,
                    toolbar.x + 8)
        y: toolbar.y + toolbar.height + 6
        z: 30
        width: 212
        height: colorPopupColumn.implicitHeight + 20
        padding: 10
        closePolicy: Popup.CloseOnEscape
                     | Popup.CloseOnPressOutside

        onAboutToShow: {
            customColor.text = root.model.color.toString()
            colorErrorLabel.text = ""
        }

        background: Rectangle {
            radius: 6
            color: AppTheme.viewerOverlayStrong
            border.width: 1
            border.color: AppTheme.viewerOverlayBorder
        }

        ColumnLayout {
            id: colorPopupColumn

            anchors.fill: parent
            spacing: 8

            RowLayout {
                spacing: 6
                Repeater {
                    model: [
                        "#f8faf9", "#dce75c", "#42d3c6",
                        "#ff785a", "#ff4f86", "#7ca9ff"
                    ]
                    delegate: ThemedToolButton {
                        objectName: "whiteboardColorSwatch"
                        Layout.preferredWidth: 26
                        Layout.preferredHeight: 26
                        onClicked: {
                            root.model.color = modelData
                            customColor.text = modelData
                            colorErrorLabel.text = ""
                            colorPopup.close()
                        }
                        contentItem: Rectangle {
                            anchors.centerIn: parent
                            width: 18
                            height: 18
                            radius: 3
                            color: modelData
                            border.width: root.model.color.toString()
                                          === modelData ? 2 : 1
                            border.color: root.model.color.toString()
                                          === modelData
                                          ? AppTheme.accent
                                          : AppTheme.borderStrong
                        }
                    }
                }
            }

            TextField {
                id: customColor
                objectName: "whiteboardCustomColor"
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                placeholderText: qsTr("#RRGGBB")
                selectByMouse: true
                color: colorErrorLabel.text.length > 0
                       ? AppTheme.error : AppTheme.viewerOverlayText
                onAccepted: {
                    if (/^#[0-9a-fA-F]{6}$/.test(text.trim())) {
                        root.model.color = text.trim()
                        colorErrorLabel.text = ""
                        colorPopup.close()
                    } else {
                        colorErrorLabel.text = qsTr(
                                    "Enter a color as #RRGGBB.")
                    }
                }
            }

            Label {
                id: colorErrorLabel

                Layout.fillWidth: true
                visible: text.length > 0
                text: ""
                color: AppTheme.error
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }
        }
    }

    FileDialog {
        id: importDialog
        objectName: "whiteboardImportDialog"
        title: qsTr("Import whiteboard set")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("ForeverTAS whiteboards (*.json)")]
        onAccepted: root.model.importBoardSet(selectedFile)
    }

    FileDialog {
        id: exportDialog
        objectName: "whiteboardExportDialog"
        title: qsTr("Export named whiteboard set")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTr("ForeverTAS whiteboards (*.json)")]
        onAccepted: root.model.exportBoardSet(selectedFile)
    }

    FileDialog {
        id: imageExportDialog
        objectName: "whiteboardImageExportDialog"
        title: root.pendingImageMode === "background"
               ? qsTr("Export drawing with full background")
               : qsTr("Export transparent drawing")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: [qsTr("PNG images (*.png)")]
        onAccepted: {
            const index = root.pendingImageBoardIndex
            const mode = root.pendingImageMode
            root.pendingImageBoardIndex = -1
            root.pendingImageMode = ""
            if (mode === "background") {
                if (!root.exportBackgroundImage(
                            index, selectedFile)) {
                    root.model.finishBoardImageExport(false, true)
                }
            } else {
                root.model.exportBoardContentImage(
                            index, selectedFile)
            }
        }
        onRejected: {
            root.pendingImageBoardIndex = -1
            root.pendingImageMode = ""
        }
    }
}
