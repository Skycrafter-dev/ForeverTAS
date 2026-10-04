pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var viewer
    property bool expanded: false
    readonly property var debuggerModel: viewer.simulationDebugger
    property var editingLine: null
    property bool hasDraftEdit: false
    property int inspectionTab: 0
    readonly property bool waitingForPause: root.debuggerModel.running
    signal expansionRequested(bool expanded)

    implicitHeight: 760

    Dialog {
        id: toolsDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(560, root.width - 24)
        title: qsTr("Debugger tools")
        modal: true
        footer: DialogButtonBox {
            ThemedButton { text: qsTr("Apply"); DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
            ThemedButton { text: qsTr("Cancel"); DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
        }
        onOpened: {
            lldbPath.text = root.debuggerModel.configuredLldbPath
            terminalPath.text = root.debuggerModel.configuredTerminalPath
        }
        onAccepted: root.debuggerModel.configureTools(lldbPath.text, terminalPath.text)
        contentItem: ColumnLayout {
            Label { text: qsTr("LLDB executable"); color: AppTheme.text }
            TextField {
                id: lldbPath
                Layout.fillWidth: true
                placeholderText: qsTr("Automatic")
                selectByMouse: true
            }
            Label { text: qsTr("Terminal bridge executable"); color: AppTheme.text }
            TextField {
                id: terminalPath
                Layout.fillWidth: true
                placeholderText: Qt.platform.os === "windows" ? "winpty.exe" : "script"
                selectByMouse: true
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        z: -1
        color: AppTheme.panel
    }

    Binding {
        target: root.debuggerModel
        property: "darkMode"
        value: AppTheme.dark
    }

    function commitActiveEdit() {
        if (root.editingLine)
            root.editingLine.commitEdit()
    }

    function cancelActiveEdit() {
        if (root.editingLine)
            root.editingLine.cancelEdit()
    }

    function escapeStyledText(value) {
        return String(value || "").replace(/&/g, "&amp;")
                                     .replace(/</g, "&lt;")
                                     .replace(/>/g, "&gt;")
    }

    function sourceName(entry) {
        const name = String(entry.name || "")
        if (!entry.modified || !entry.breakpoint)
            return name
        let result = ""
        for (let index = 0; index < name.length; ++index) {
            const color = index % 2 === 0
                        ? AppTheme.codeBreakpoint : AppTheme.success
            result += "<font color=\"" + color + "\">"
                    + root.escapeStyledText(name.charAt(index)) + "</font>"
        }
        return result
    }

    function restoreEditorPositions(sourcePosition, codePosition) {
        Qt.callLater(function() {
            sourceTree.forceLayout()
            codeList.forceLayout()
            sourceTree.contentY = Math.max(
                0, Math.min(sourcePosition,
                            sourceTree.contentHeight - sourceTree.height))
            codeList.contentY = Math.max(
                0, Math.min(codePosition,
                            codeList.contentHeight - codeList.height))
        })
    }

    function toggleBreakpoint(lineNumber) {
        const sourcePosition = sourceTree.contentY
        const codePosition = codeList.contentY
        if (!root.debuggerModel.toggleBreakpoint(
                    root.debuggerModel.selectedFilePath, lineNumber))
            return
        root.restoreEditorPositions(sourcePosition, codePosition)
    }

    function updateLine(lineNumber, text) {
        const sourcePosition = sourceTree.contentY
        const codePosition = codeList.contentY
        if (!root.debuggerModel.updateLine(lineNumber, text))
            return false
        root.restoreEditorPositions(sourcePosition, codePosition)
        return true
    }

    function insertLineAfter(lineNumber) {
        if (root.debuggerModel.running || root.debuggerModel.stepping
                || root.debuggerModel.compiling)
            return -1
        root.commitActiveEdit()
        const sourcePosition = sourceTree.contentY
        const codePosition = codeList.contentY
        const insertedLine = root.debuggerModel.insertLineAfter(lineNumber)
        if (insertedLine < 1)
            return -1
        root.restoreEditorPositions(sourcePosition, codePosition)
        Qt.callLater(function() {
            codeList.positionViewAtIndex(insertedLine - 1, ListView.Contain)
            Qt.callLater(function() {
                const item = codeList.itemAtIndex(insertedLine - 1)
                if (item)
                    item.beginEdit()
            })
        })
        return insertedLine
    }

    function deleteLine(lineNumber) {
        if (root.debuggerModel.running || root.debuggerModel.stepping
                || root.debuggerModel.compiling)
            return false
        root.cancelActiveEdit()
        const sourcePosition = sourceTree.contentY
        const codePosition = codeList.contentY
        if (!root.debuggerModel.deleteLine(lineNumber))
            return false
        root.restoreEditorPositions(sourcePosition, codePosition)
        return true
    }

    function revealExecutingLine() {
        if (root.waitingForPause)
            return
        Qt.callLater(function() {
            if (!root.waitingForPause
                    && root.debuggerModel.selectedFilePath
                    === root.debuggerModel.activeFilePath
                    && root.debuggerModel.activeLine > 0) {
                codeList.forceLayout()
                const centeredY =
                    (root.debuggerModel.activeLine - 0.5) * 28
                    - codeList.height / 2
                codeList.contentY = Math.max(
                    0,
                    Math.min(centeredY,
                             Math.max(0,
                                      codeList.contentHeight
                                      - codeList.height)))
            }
        })
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Reference source")
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: AppTheme.text
                }

                Label {
                    Layout.fillWidth: true
                    text: root.debuggerModel.preparing
                          ? qsTr("preparing native source")
                          : (root.debuggerModel.running
                             ? qsTr("native execution running")
                             : (root.debuggerModel.stepping
                                ? qsTr("native execution stepping")
                                : (root.debuggerModel.compiling
                                   ? qsTr("compiling edited C++")
                                   : qsTr("native execution paused"))))
                    font.family: "monospace"
                    font.pixelSize: 10
                    color: AppTheme.textMuted
                    elide: Text.ElideRight
                }
            }

            ThemedButton {
                objectName: "restartLiveSimulationButton"
                text: qsTr("Restart")
                enabled: root.viewer.loaded && !root.viewer.loading
                         && root.debuggerModel.available
                onClicked: {
                    root.commitActiveEdit()
                    root.viewer.startSimulationDebugger()
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Restart the reference engine from tick zero")
            }

            ThemedButton {
                objectName: "resetLiveEditsButton"
                text: qsTr("Reset")
                enabled: root.debuggerModel.hasEdits || root.hasDraftEdit
                onClicked: {
                    root.cancelActiveEdit()
                    root.debuggerModel.resetEdits()
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Restore all in-memory source edits")
            }

            ThemedToolButton {
                id: editorExpansionButton

                objectName: "toggleCodeEditorExpansionButton"
                text: root.expanded ? "\u2715" : "\u26f6"
                font.pixelSize: root.expanded ? 15 : 18
                onClicked: root.expansionRequested(!root.expanded)
                Accessible.name: root.expanded
                                 ? qsTr("Restore code panel")
                                 : qsTr("Expand code panel")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            ThemedButton {
                objectName: "debuggerToolsButton"
                text: qsTr("Tools")
                enabled: !root.debuggerModel.active && !root.debuggerModel.preparing
                onClicked: toolsDialog.open()
            }
            ThemedButton {
                objectName: "refreshDebuggerToolsButton"
                text: qsTr("Recheck")
                enabled: !root.debuggerModel.active && !root.debuggerModel.preparing
                onClicked: root.debuggerModel.refreshDebuggerTools()
            }
        }

        Rectangle {
            id: referenceLoadingWarning

            objectName: "referenceLoadingWarning"
            Layout.fillWidth: true
            Layout.preferredHeight: referenceLoadingWarningText.implicitHeight
                                    + 24
            visible: root.debuggerModel.loadingReplay
            color: AppTheme.errorSoft
            border.width: 2
            border.color: AppTheme.error
            radius: 4

            Label {
                id: referenceLoadingWarningText

                objectName: "referenceLoadingWarningText"
                anchors.fill: parent
                anchors.margins: 12
                text: root.debuggerModel.statusText
                color: AppTheme.error
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.WordWrap
                font.pixelSize: 19
                font.weight: Font.Bold
            }
        }

        Label {
            objectName: "simulationDebuggerStatusText"
            Layout.fillWidth: true
            visible: !root.debuggerModel.loadingReplay
            text: root.debuggerModel.statusText
            color: root.debuggerModel.editError.length > 0
                   ? AppTheme.error : AppTheme.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: 11
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            ThemedButton {
                objectName: "debuggerSubstepForwardButton"
                Layout.fillWidth: true
                text: qsTr("Substep Forward")
                enabled: root.debuggerModel.canStepSource
                onClicked: {
                    root.commitActiveEdit()
                    root.debuggerModel.stepSubstep()
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Advance one native execution substep")
            }

            ThemedButton {
                objectName: "debuggerSourceLineStepButton"
                Layout.fillWidth: true
                text: qsTr("Source Line Step")
                enabled: root.debuggerModel.canStepSource
                onClicked: {
                    root.commitActiveEdit()
                    root.debuggerModel.stepSourceLine()
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Execute through the current source line")
            }

            ThemedButton {
                objectName: "debuggerTickStepButton"
                Layout.fillWidth: true
                text: qsTr("Tick Step")
                enabled: root.debuggerModel.canStepTick
                onClicked: {
                    root.commitActiveEdit()
                    root.debuggerModel.stepTick()
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Advance exactly one physics tick")
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.expanded ? 104 : 118
            color: AppTheme.surface
            border.width: 1
            border.color: AppTheme.border
            radius: 6
            clip: true

            ListView {
                id: sourceTree

                objectName: "simulationSourceTree"
                anchors.fill: parent
                anchors.margins: 3
                model: root.debuggerModel.fileEntries
                clip: true
                flickableDirection: Flickable.VerticalFlick
                interactive: contentHeight > height
                boundsBehavior: Flickable.StopAtBounds
                rightMargin: sourceScrollBar.visible
                             ? sourceScrollBar.width + 3 : 0
                ScrollBar.vertical: ScrollBar {
                    id: sourceScrollBar

                    objectName: "simulationSourceTreeScrollBar"
                    policy: ScrollBar.AsNeeded
                    interactive: true
                }

                delegate: ThemedItemDelegate {
                    id: sourceRow

                    required property var modelData

                    width: sourceTree.width
                           - (sourceScrollBar.visible
                              ? sourceScrollBar.width + 3 : 0)
                    height: 25
                    leftPadding: 7 + sourceRow.modelData.depth * 13
                    rightPadding: 6
                    highlighted: false
                    onClicked: {
                        root.commitActiveEdit()
                        if (sourceRow.modelData.directory)
                            root.debuggerModel.toggleFolder(
                                sourceRow.modelData.path)
                        else
                            root.debuggerModel.selectFile(
                                sourceRow.modelData.path)
                    }

                    contentItem: RowLayout {
                        spacing: 5

                        Label {
                            text: sourceRow.modelData.directory
                                  ? (sourceRow.modelData.expanded ? "v" : ">")
                                  : "{}"
                            color: sourceRow.modelData.breakpoint
                                   ? AppTheme.codeBreakpoint
                                   : (sourceRow.modelData.modified
                                      ? AppTheme.success
                                      : AppTheme.textMuted)
                            font.family: "monospace"
                            font.pixelSize: 10
                        }

                        Label {
                            objectName: "simulationSourceName"
                            Layout.fillWidth: true
                            text: root.sourceName(sourceRow.modelData)
                            textFormat: sourceRow.modelData.modified
                                        && sourceRow.modelData.breakpoint
                                        ? Text.StyledText : Text.PlainText
                            color: sourceRow.modelData.breakpoint
                                   ? AppTheme.codeBreakpoint
                                   : (sourceRow.modelData.modified
                                      ? AppTheme.success : AppTheme.text)
                            font.weight: sourceRow.modelData.modified
                                         || sourceRow.modelData.breakpoint
                                         ? Font.DemiBold : Font.Normal
                            font.family: sourceRow.modelData.directory
                                         ? "sans-serif" : "monospace"
                            font.pixelSize: 11
                            elide: Text.ElideMiddle
                        }
                    }

                    background: Rectangle {
                        color: sourceRow.modelData.selected
                               ? AppTheme.selection : "transparent"
                        radius: 3
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: root.debuggerModel.selectedFilePath
                color: AppTheme.textMuted
                font.family: "monospace"
                font.pixelSize: 10
                elide: Text.ElideMiddle
            }

            Label {
                visible: root.debuggerModel.active
                text: qsTr("tick %1").arg(
                          root.debuggerModel.executionTick)
                color: AppTheme.info
                font.family: "monospace"
                font.pixelSize: 10
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: root.expanded ? 150 : 250
            color: AppTheme.codeSurface
            border.width: 1
            border.color: AppTheme.border
            radius: 6
            clip: true

            ListView {
                id: codeList

                objectName: "simulationCodeViewer"
                anchors.fill: parent
                anchors.margins: 1
                model: root.debuggerModel.lines
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                ScrollBar.horizontal: ScrollBar {}

                delegate: Rectangle {
                    id: codeLine

                    required property var modelData
                    required property int index
                    property bool editing: false
                    function commitEdit() {
                        if (!codeLine.editing)
                            return
                        const committedText = liveEdit.text
                        codeLine.editing = false
                        if (root.editingLine === codeLine)
                            root.editingLine = null
                        root.updateLine(
                            codeLine.modelData.number, committedText)
                        root.hasDraftEdit = false
                    }
                    function cancelEdit() {
                        if (!codeLine.editing)
                            return
                        liveEdit.text = codeLine.modelData.text
                        codeLine.editing = false
                        if (root.editingLine === codeLine)
                            root.editingLine = null
                        root.hasDraftEdit = false
                    }
                    function beginEdit() {
                        if (!codeLine.modelData.editable)
                            return
                        if (root.debuggerModel.running
                                || root.debuggerModel.stepping) {
                            root.viewer.pause()
                            return
                        }
                        root.commitActiveEdit()
                        codeLine.editing = true
                        root.editingLine = codeLine
                        root.hasDraftEdit =
                            liveEdit.text !== codeLine.modelData.original
                        liveEdit.forceActiveFocus()
                        liveEdit.selectAll()
                    }
                    function insertAfter() {
                        root.insertLineAfter(codeLine.modelData.number)
                    }
                    function deleteCurrent() {
                        root.deleteLine(codeLine.modelData.number)
                    }
                    readonly property bool draftModified:
                        codeLine.editing
                        ? liveEdit.text !== codeLine.modelData.original
                        : codeLine.modelData.modified

                    width: Math.max(codeList.width, codeRow.implicitWidth + 12)
                    height: 28
                    readonly property bool displayedActive:
                        codeLine.modelData.active
                        && !root.waitingForPause
                        && root.debuggerModel.selectedFilePath
                           === root.debuggerModel.activeFilePath
                        && codeLine.modelData.number
                           === root.debuggerModel.activeLine
                    color: codeLine.displayedActive
                           ? AppTheme.codeActive
                           : (index % 2 === 0
                              ? AppTheme.codeSurface
                              : AppTheme.codeAlternate)

                    Timer {
                        id: lineBreakpointTimer
                        interval: 280
                        repeat: false
                        onTriggered:
                            root.toggleBreakpoint(codeLine.modelData.number)
                    }

                    HoverHandler {
                        id: codeLineHover
                    }

                    TapHandler {
                        enabled: !codeLine.editing
                        acceptedButtons: Qt.LeftButton
                        onTapped: function(eventPoint, button) {
                            if (eventPoint.position.x >= 23)
                                lineBreakpointTimer.restart()
                        }
                        onDoubleTapped: function(eventPoint, button) {
                            lineBreakpointTimer.stop()
                            if (eventPoint.position.x >= 23)
                                codeLine.beginEdit()
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        width: codeLine.draftModified ? 3 : 0
                        height: parent.height
                        color: AppTheme.success
                    }

                    Row {
                        anchors.right: parent.right
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        z: 2
                        visible: codeLineHover.hovered && !codeLine.editing

                        ThemedToolButton {
                            objectName: "insertLiveCodeLineButton"
                            width: 22
                            height: 22
                            text: "+"
                            enabled: !root.debuggerModel.running
                                     && !root.debuggerModel.stepping
                                     && !root.debuggerModel.compiling
                                     && codeLine.modelData.editable
                                     && codeLine.modelData.number
                                        < codeList.count
                            onClicked: codeLine.insertAfter()
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Insert a source line after line %1")
                                          .arg(codeLine.modelData.number)
                        }

                        ThemedToolButton {
                            objectName: "deleteLiveCodeLineButton"
                            width: 22
                            height: 22
                            text: "\u00d7"
                            enabled: !root.debuggerModel.running
                                     && !root.debuggerModel.stepping
                                     && !root.debuggerModel.compiling
                                     && codeList.count > 1
                            onClicked: codeLine.deleteCurrent()
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Delete source line %1")
                                          .arg(codeLine.modelData.number)
                        }
                    }

                    RowLayout {
                        id: codeRow

                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4

                        Item {
                            Layout.preferredWidth: 19
                            Layout.preferredHeight: 24

                            Rectangle {
                                anchors.centerIn: parent
                                width: 9
                                height: 9
                                radius: 5
                                visible: codeLine.modelData.breakpoint
                                color: AppTheme.codeBreakpoint
                            }

                            TapHandler {
                                enabled: codeLine.modelData.editable
                                onTapped:
                                    root.toggleBreakpoint(
                                        codeLine.modelData.number)
                            }
                        }

                        Label {
                            Layout.preferredWidth: 23
                            text: codeLine.modelData.number
                            horizontalAlignment: Text.AlignRight
                            color: codeLine.displayedActive
                                   ? AppTheme.info : AppTheme.codeLineNumber
                            font.family: "monospace"
                            font.pixelSize: 10
                        }

                        TextField {
                            id: liveEdit

                            objectName: "liveCodeEditableLine"
                            visible: codeLine.editing
                            Layout.preferredWidth: Math.max(
                                                       255,
                                                       contentWidth + 18)
                            Layout.preferredHeight: 25
                            text: codeLine.modelData.text
                            selectByMouse: true
                            font.family: "monospace"
                            font.pixelSize: 11
                            color: codeLine.draftModified
                                   ? AppTheme.success : AppTheme.text
                            leftPadding: 5
                            rightPadding: 5
                            topPadding: 2
                            bottomPadding: 2
                            background: Rectangle {
                                color: parent.activeFocus
                                       ? AppTheme.surface : "transparent"
                                border.width: parent.activeFocus ? 1 : 0
                                border.color: AppTheme.focus
                                radius: 3
                            }
                            onTextChanged: {
                                if (codeLine.editing) {
                                    root.hasDraftEdit =
                                        liveEdit.text
                                        !== codeLine.modelData.original
                                }
                            }
                            onEditingFinished: codeLine.commitEdit()
                            Keys.onEscapePressed: codeLine.cancelEdit()
                            Keys.onReturnPressed: function(event) {
                                const lineNumber =
                                    codeLine.modelData.number
                                codeLine.commitEdit()
                                root.insertLineAfter(lineNumber)
                                event.accepted = true
                            }
                        }

                        Text {
                            visible: !codeLine.editing
                            Layout.preferredWidth: Math.max(
                                                       255,
                                                       implicitWidth)
                            text: codeLine.modelData.highlighted
                            textFormat: Text.RichText
                            color: AppTheme.text
                            font.family: "monospace"
                            font.pixelSize: 11
                            elide: Text.ElideNone
                        }

                        Label {
                            visible: codeLine.modelData.inlineValue.length > 0
                            text: codeLine.modelData.inlineValue
                            color: AppTheme.codeLineNumber
                            font.family: "monospace"
                            font.pixelSize: 9
                            leftPadding: 8
                        }
                    }
                }
            }

            Rectangle {
                id: waitingForPauseOverlay

                objectName: "debuggerWaitingForPauseOverlay"
                anchors.top: parent.top
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.topMargin: 8
                width: Math.min(Math.max(0, parent.width - 16),
                                waitingForPauseLabel.implicitWidth + 18)
                height: 26
                visible: root.waitingForPause
                z: 2
                color: AppTheme.overlay
                border.width: 1
                border.color: AppTheme.overlayBorder
                radius: 4
                opacity: 0.92

                Label {
                    id: waitingForPauseLabel

                    objectName: "debuggerWaitingForPauseText"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    text: qsTr("Waiting for pause")
                    color: AppTheme.textMuted
                    font.pixelSize: 10
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                }
            }
        }

        Label {
            visible: root.debuggerModel.editError.length > 0
            Layout.fillWidth: true
            text: root.debuggerModel.editError
            color: AppTheme.error
            font.pixelSize: 10
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            TabBar {
                Layout.fillWidth: true
                currentIndex: root.inspectionTab
                onCurrentIndexChanged: root.inspectionTab = currentIndex
                ThemedTabButton { text: qsTr("Debug output") }
                ThemedTabButton { text: qsTr("Watches") }
            }

            ThemedButton {
                objectName: "clearSimulationDebugOutputButton"
                visible: root.inspectionTab === 0
                text: qsTr("Clear")
                enabled: root.debuggerModel.debugOutput.length > 0
                onClicked: root.debuggerModel.clearDebugOutput()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Clear printed debug output")
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.expanded ? 100 : 132
            visible: root.inspectionTab === 0
            color: AppTheme.surface
            border.width: 1
            border.color: AppTheme.border
            radius: 6
            clip: true

            ListView {
                id: debugOutputList

                objectName: "simulationDebugOutput"
                anchors.fill: parent
                anchors.margins: 3
                model: root.debuggerModel.debugOutput
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                delegate: ThemedItemDelegate {
                    id: outputEntry

                    required property var modelData
                    required property int index

                    width: debugOutputList.width
                    height: Math.max(42, outputContent.implicitHeight + 8)
                    leftPadding: 6
                    rightPadding: 6
                    topPadding: 4
                    bottomPadding: 4
                    onClicked: {
                        root.commitActiveEdit()
                        root.debuggerModel.openDebugOutput(outputEntry.index)
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Open printed source location")

                    contentItem: ColumnLayout {
                        id: outputContent

                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            Label {
                                Layout.fillWidth: true
                                text: outputEntry.modelData.location
                                color: AppTheme.info
                                font.family: "monospace"
                                font.pixelSize: 9
                                font.underline: true
                                elide: Text.ElideMiddle
                            }

                            Label {
                                text: qsTr("%1 · tick %2")
                                      .arg(outputEntry.modelData.context)
                                      .arg(outputEntry.modelData.tick)
                                color: AppTheme.textFaint
                                font.family: "monospace"
                                font.pixelSize: 9
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: outputEntry.modelData.message
                            color: AppTheme.text
                            font.family: "monospace"
                            font.pixelSize: 10
                            wrapMode: Text.WrapAnywhere
                        }
                    }
                }

                Label {
                    objectName: "simulationDebugOutputEmptyState"
                    anchors.centerIn: parent
                    visible: root.debuggerModel.debugOutput.length === 0
                    text: qsTr("No printed output for this run.")
                    color: AppTheme.textFaint
                    font.pixelSize: 10
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.expanded ? 100 : 132
            visible: root.inspectionTab === 1
            color: AppTheme.surface
            border.width: 1
            border.color: AppTheme.border
            radius: 6
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 3
                RowLayout {
                    Layout.fillWidth: true
                    TextField {
                        id: watchPath
                        objectName: "debuggerWatchPath"
                        Layout.fillWidth: true
                        placeholderText: qsTr("Variable path")
                        selectByMouse: true
                        function pin() {
                            if (root.debuggerModel.pinWatch(text).length > 0) text = ""
                        }
                        onAccepted: pin()
                    }
                    ThemedIconButton {
                        objectName: "addDebuggerWatch"
                        Layout.preferredWidth: 30
                        Layout.preferredHeight: 30
                        icon.source: "qrc:/icons/plus.svg"
                        Accessible.name: qsTr("Pin watch")
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Pin watch")
                        onClicked: watchPath.pin()
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: root.debuggerModel.watchError
                    color: AppTheme.error
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }
                ListView {
                    id: watchList
                    objectName: "debuggerWatchList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.debuggerModel.watches
                    ScrollBar.vertical: ScrollBar {}
                    delegate: RowLayout {
                        id: watchRow
                        required property var modelData
                        width: watchList.width
                        height: 38
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                Layout.fillWidth: true
                                text: watchRow.modelData.expression
                                color: AppTheme.text
                                font.family: "monospace"
                                font.pixelSize: 10
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                text: watchRow.modelData.status === "Ready"
                                    ? watchRow.modelData.value : watchRow.modelData.status
                                color: watchRow.modelData.status === "Ready" ? AppTheme.info : AppTheme.textMuted
                                font.family: "monospace"
                                font.pixelSize: 10
                                elide: Text.ElideRight
                                ToolTip.visible: watchHover.hovered
                                ToolTip.text: watchRow.modelData.type + " " + text
                                HoverHandler { id: watchHover }
                            }
                        }
                        ThemedIconButton {
                            Layout.preferredWidth: 28
                            Layout.preferredHeight: 28
                            icon.source: "qrc:/icons/trash-2.svg"
                            Accessible.name: qsTr("Remove watch")
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Remove watch")
                            onClicked: root.debuggerModel.unpinWatch(watchRow.modelData.id)
                        }
                    }
                }
            }
        }
    }

    Connections {
        target: root.debuggerModel

        function onExecutionChanged() {
            root.revealExecutingLine()
        }

        function onLinesChanged() {
            root.revealExecutingLine()
        }

        function onSourceLocationRequested(line) {
            Qt.callLater(function() {
                codeList.forceLayout()
                const centeredY = (line - 0.5) * 28 - codeList.height / 2
                codeList.contentY = Math.max(
                    0,
                    Math.min(centeredY,
                             Math.max(0,
                                      codeList.contentHeight
                                      - codeList.height)))
            })
        }
    }

    onWaitingForPauseChanged: {
        if (!root.waitingForPause)
            root.revealExecutingLine()
    }

    onVisibleChanged: {
        if (!visible)
            root.commitActiveEdit()
    }
}
