pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root

    required property var controller
    required property var assistance
    property string script: controller.conditionScript
    property string gateMode: controller.conditionGateMode
    property bool running: controller.running
    property string evaluationTargetId: controller.evaluationTargetId
    property bool automaticCompletionBlocked: false
    property bool documentSyncScheduled: false
    property var pendingSymbolDocumentation: ({})
    property string pendingSymbolHoverKey: ""
    property string activeSymbolHoverKey: ""
    readonly property bool editorActive: conditionScriptArea.activeFocus
    readonly property bool overlayOpen:
        completionPopup.visible || conditionScriptLibraryDialog.visible
    readonly property color syntaxSymbolColor: AppTheme.accent
    readonly property color syntaxPreviousColor: AppTheme.textMuted
    readonly property color syntaxFunctionColor: AppTheme.info
    readonly property color syntaxNumberColor: AppTheme.warning
    readonly property color syntaxOperatorColor: AppTheme.textFaint
    readonly property color syntaxErrorColor: AppTheme.error
    readonly property Flickable editorFlickable:
        conditionScriptScroll.contentItem as Flickable
    readonly property real editorContentY:
        editorFlickable ? editorFlickable.contentY : 0
    signal scriptEdited(string text)
    signal gateModeEdited(string mode)

    objectName: "conditionBuilder"
    Layout.fillWidth: true
    spacing: 4

    function refreshHighlightPalette() {
        if (!assistance)
            return
        assistance.setHighlightPalette(syntaxSymbolColor,
                                       syntaxPreviousColor,
                                       syntaxFunctionColor,
                                       syntaxNumberColor,
                                       syntaxOperatorColor,
                                       syntaxErrorColor)
    }

    function positionCompletion() {
        if (!completionPopup.visible || !Overlay.overlay)
            return
        const rectangle = conditionScriptArea.cursorRectangle
        const below = conditionScriptArea.mapToItem(
                        Overlay.overlay,
                        rectangle.x,
                        rectangle.y + rectangle.height + 12)
        const above = conditionScriptArea.mapToItem(
                        Overlay.overlay,
                        rectangle.x,
                        rectangle.y - completionPopup.height - 12)
        completionPopup.x = Math.max(
                    12,
                    Math.min(below.x,
                             Overlay.overlay.width
                             - completionPopup.width - 12))
        completionPopup.y = below.y + completionPopup.height
                            <= Overlay.overlay.height - 12
                            ? below.y : Math.max(12, above.y)
    }

    function openCompletion(explicitRequest) {
        clearSymbolHover()
        if (running || conditionScriptArea.inputMethodComposing
                || assistance.completions.length === 0) {
            completionPopup.close()
            return
        }
        if (!explicitRequest
                && root.assistance.completionContext.automaticTrigger
                !== true)
            return
        completionPopup.resetSelection()
        completionPopup.open()
        Qt.callLater(positionCompletion)
    }

    function applyCompletion(index) {
        assistance.updateDocumentState(conditionScriptArea.text,
                                       conditionScriptArea.cursorPosition)
        const selection = assistance.completionEdit(index)
        if (selection.completionId === undefined)
            return
        const edit = assistance.acceptCompletion(
                    selection.completionId, selection.revision)
        if (edit.accepted !== true) {
            completionPopup.close()
            root.scheduleDocumentSync()
            return
        }
        automaticCompletionBlocked = true
        conditionScriptArea.text = edit.source
        conditionScriptArea.cursorPosition = edit.cursorPosition
        completionPopup.close()
        conditionScriptArea.forceActiveFocus()
        Qt.callLater(function() {
            automaticCompletionBlocked = false
            if (edit.reopen === true)
                root.openCompletion(true)
        })
    }

    function scheduleDocumentSync() {
        if (documentSyncScheduled)
            return
        documentSyncScheduled = true
        Qt.callLater(function() {
            documentSyncScheduled = false
            assistance.updateDocumentState(
                        conditionScriptArea.text,
                        conditionScriptArea.cursorPosition)
            if (automaticCompletionBlocked
                    || !conditionScriptArea.activeFocus
                    || conditionScriptArea.inputMethodComposing)
                return
            if (completionPopup.visible)
                root.openCompletion(true)
            else
                root.openCompletion(false)
        })
    }

    function toggleLine(lineNumber) {
        const edit = assistance.toggleLine(
                    lineNumber, conditionScriptArea.cursorPosition)
        if (edit.source === undefined)
            return
        conditionScriptArea.text = edit.source
        conditionScriptArea.cursorPosition = edit.cursorPosition
        conditionScriptArea.forceActiveFocus()
    }

    function textLineRectangle(position) {
        const boundedPosition = Math.max(
                    0, Math.min(position, conditionScriptArea.length))
        return conditionScriptArea.positionToRectangle(boundedPosition)
    }

    function symbolDocumentationAt(position) {
        let documentation = assistance.documentationAt(position)
        if ((documentation.symbol === undefined || !documentation.symbol)
                && position > 0)
            documentation = assistance.documentationAt(position - 1)
        return documentation
    }

    function clearSymbolHover() {
        symbolHoverTimer.stop()
        pendingSymbolDocumentation = ({})
        pendingSymbolHoverKey = ""
        activeSymbolHoverKey = ""
        symbolHoverPopup.close()
    }

    function scheduleSymbolHoverAt(position) {
        if (root.running || completionPopup.visible
                || conditionScriptLibraryDialog.visible) {
            clearSymbolHover()
            return
        }
        const documentation = symbolDocumentationAt(position)
        if (documentation.symbol === undefined || !documentation.symbol) {
            clearSymbolHover()
            return
        }
        const key = documentation.symbol + ":" + documentation.start
                + ":" + documentation.length
        if (key === pendingSymbolHoverKey
                || (symbolHoverPopup.visible
                    && key === activeSymbolHoverKey))
            return
        symbolHoverTimer.stop()
        symbolHoverPopup.close()
        activeSymbolHoverKey = ""
        pendingSymbolDocumentation = documentation
        pendingSymbolHoverKey = key
        symbolHoverTimer.restart()
    }

    function updateSymbolHover(localPosition) {
        const position = conditionScriptArea.positionAt(
                    localPosition.x, localPosition.y)
        scheduleSymbolHoverAt(position)
    }

    function positionSymbolHover() {
        if (!symbolHoverPopup.visible || !Overlay.overlay)
            return
        const rectangle = conditionScriptArea.positionToRectangle(
                    symbolHoverPopup.documentation.start || 0)
        const below = conditionScriptArea.mapToItem(
                    Overlay.overlay, rectangle.x,
                    rectangle.y + rectangle.height + 8)
        const above = conditionScriptArea.mapToItem(
                    Overlay.overlay, rectangle.x,
                    rectangle.y - symbolHoverPopup.height - 8)
        symbolHoverPopup.x = Math.max(
                    12, Math.min(below.x,
                                 Overlay.overlay.width
                                 - symbolHoverPopup.width - 12))
        symbolHoverPopup.y = below.y + symbolHoverPopup.height
                <= Overlay.overlay.height - 12
                ? below.y : Math.max(12, above.y)
    }

    function openPendingSymbolHover() {
        if (pendingSymbolDocumentation.symbol === undefined
                || completionPopup.visible
                || conditionScriptLibraryDialog.visible)
            return
        symbolHoverPopup.documentation = pendingSymbolDocumentation
        activeSymbolHoverKey = pendingSymbolHoverKey
        symbolHoverPopup.open()
        Qt.callLater(positionSymbolHover)
    }

    function applyFunctionCallPair() {
        assistance.updateDocumentState(conditionScriptArea.text,
                                       conditionScriptArea.cursorPosition)
        const edit = assistance.functionCallEdit(
                    conditionScriptArea.cursorPosition)
        if (edit.source === undefined)
            return false
        clearSymbolHover()
        automaticCompletionBlocked = true
        conditionScriptArea.text = edit.source
        conditionScriptArea.cursorPosition = edit.cursorPosition
        assistance.updateDocumentState(conditionScriptArea.text,
                                       conditionScriptArea.cursorPosition)
        Qt.callLater(function() {
            automaticCompletionBlocked = false
            root.openCompletion(false)
        })
        return true
    }

    onSyntaxSymbolColorChanged: refreshHighlightPalette()
    onSyntaxPreviousColorChanged: refreshHighlightPalette()
    onSyntaxFunctionColorChanged: refreshHighlightPalette()
    onSyntaxNumberColorChanged: refreshHighlightPalette()
    onSyntaxOperatorColorChanged: refreshHighlightPalette()
    onSyntaxErrorColorChanged: refreshHighlightPalette()
    onEvaluationTargetIdChanged: {
        if (assistance.evaluationTargetId !== evaluationTargetId)
            assistance.evaluationTargetId = evaluationTargetId
    }
    onGateModeChanged: {
        if (assistance.gateMode !== gateMode)
            assistance.gateMode = gateMode
    }
    onRunningChanged: {
        if (running)
            clearSymbolHover()
    }

    Component.onCompleted: {
        assistance.updateDocumentState(conditionScriptArea.text,
                                       conditionScriptArea.cursorPosition)
        assistance.evaluationTargetId = root.evaluationTargetId
        assistance.gateMode = root.gateMode
        assistance.attachDocument(conditionScriptArea.textDocument)
        refreshHighlightPalette()
    }

    RowLayout {
        id: gateModeRow

        objectName: "conditionGateModeRow"
        Layout.fillWidth: true
        spacing: 8

        Label {
            id: gateModeLabel

            objectName: "conditionGateModeLabel"
            Layout.fillWidth: true
            text: qsTr("Combine enabled gates")
            color: AppTheme.text
            font.pixelSize: 12
            font.weight: Font.Medium
            verticalAlignment: Text.AlignVCenter
        }

        Rectangle {
            id: gateModeGroup

            objectName: "conditionGateModeGroup"
            Layout.preferredWidth: 116
            Layout.preferredHeight: 32
            color: AppTheme.control
            border.width: 1
            border.color: AppTheme.borderStrong
            radius: 5

            RowLayout {
                anchors.fill: parent
                anchors.margins: 2
                spacing: 2

                ThemedButton {
                    id: gateAndButton

                    objectName: "conditionGateAndButton"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    enabled: !root.running
                    flat: true
                    highlighted: root.gateMode !== "or"
                    text: qsTr("AND")
                    font.pixelSize: 10
                    leftPadding: 6
                    rightPadding: 6
                    topPadding: 2
                    bottomPadding: 2
                    Accessible.name: qsTr("Combine gates with AND")
                    Accessible.description:
                        qsTr("Require every enabled condition line.")
                    onClicked: {
                        if (root.gateMode !== "and")
                            root.gateModeEdited("and")
                    }
                }

                ThemedButton {
                    id: gateOrButton

                    objectName: "conditionGateOrButton"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    enabled: !root.running
                    flat: true
                    highlighted: root.gateMode === "or"
                    text: qsTr("OR")
                    font.pixelSize: 10
                    leftPadding: 6
                    rightPadding: 6
                    topPadding: 2
                    bottomPadding: 2
                    Accessible.name: qsTr("Combine gates with OR")
                    Accessible.description:
                        qsTr("Accept any enabled condition line.")
                    onClicked: {
                        if (root.gateMode !== "or")
                            root.gateModeEdited("or")
                    }
                }
            }
        }
    }

    Rectangle {
        id: editorFrame
        readonly property int contentInset: 2

        objectName: "conditionEditorFrame"
        Layout.fillWidth: true
        Layout.topMargin: 4
        Layout.preferredHeight: 164
        color: root.running ? AppTheme.disabledSurface : AppTheme.codeSurface
        border.width: conditionScriptArea.activeFocus ? 2 : 1
        border.color: conditionScriptArea.activeFocus
                      ? AppTheme.focus : AppTheme.border
        radius: 6
        clip: true

        Rectangle {
            id: gateRail

            objectName: "conditionGateRail"
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.leftMargin: editorFrame.contentInset
            anchors.topMargin: editorFrame.contentInset
            anchors.bottomMargin: editorFrame.contentInset
            width: 24
            radius: Math.max(0, editorFrame.radius - editorFrame.contentInset)
            color: AppTheme.codeAlternate

            Rectangle {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: gateRail.radius
                color: gateRail.color
            }

            Rectangle {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: 1
                color: AppTheme.border
            }

            Item {
                anchors.fill: parent
                clip: true

                Repeater {
                    model: root.assistance.lineStates

                    delegate: Item {
                        id: gateDelegate
                        required property var modelData
                        readonly property rect textLineRectangle:
                            root.textLineRectangle(modelData.start)
                        readonly property bool interactive:
                            modelData.state !== "blank" && !root.running
                        readonly property color stateColor:
                            modelData.state === "valid"
                            ? AppTheme.success
                            : modelData.state === "disabled"
                              || modelData.state === "error"
                              ? AppTheme.error
                              : AppTheme.codeLineNumber

                        objectName: "conditionGateLine_" + modelData.line
                        y: textLineRectangle.y - root.editorContentY
                        width: parent.width
                        height: textLineRectangle.height
                        Accessible.role: Accessible.Button
                        Accessible.name: modelData.state === "disabled"
                                         ? qsTr("Enable condition line %1")
                                               .arg(modelData.line)
                                         : qsTr("Disable condition line %1")
                                               .arg(modelData.line)
                        Accessible.description: modelData.message || ""

                        Rectangle {
                            anchors.fill: parent
                            color: gateHover.hovered
                                   && gateDelegate.interactive
                                   ? AppTheme.controlHover : "transparent"
                        }

                        Label {
                            objectName: "conditionGateLineNumber_"
                                        + gateDelegate.modelData.line
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 2
                            anchors.rightMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            text: gateDelegate.modelData.line
                            color: gateDelegate.stateColor
                            font.family: "monospace"
                            font.pixelSize: 9
                            font.weight: gateDelegate.modelData.state
                                         === "blank"
                                         ? Font.Normal : Font.DemiBold
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                        }

                        HoverHandler {
                            id: gateHover
                            enabled: gateDelegate.interactive
                        }

                        TapHandler {
                            enabled: gateDelegate.interactive
                            acceptedButtons: Qt.LeftButton
                            onTapped: root.toggleLine(
                                          gateDelegate.modelData.line)
                        }
                    }
                }
            }
        }

        Item {
            id: lineHighlightViewport

            anchors.left: gateRail.right
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.rightMargin: editorFrame.contentInset
            anchors.topMargin: editorFrame.contentInset
            anchors.bottomMargin: editorFrame.contentInset
            clip: true

            Repeater {
                model: root.assistance.lineStates

                delegate: Rectangle {
                    required property var modelData
                    readonly property rect textLineRectangle:
                        root.textLineRectangle(modelData.start)
                    objectName: "conditionLineHighlight_" + modelData.line
                    y: textLineRectangle.y - root.editorContentY
                    width: lineHighlightViewport.width
                    height: textLineRectangle.height
                    color: modelData.state === "error"
                           ? AppTheme.errorSoft : "transparent"
                }
            }
        }

        ScrollView {
            id: conditionScriptScroll

            objectName: "conditionScriptScrollView"
            anchors.left: gateRail.right
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.rightMargin: editorFrame.contentInset
            anchors.topMargin: editorFrame.contentInset
            anchors.bottomMargin: editorFrame.contentInset
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AsNeeded
            ScrollBar.vertical.policy: ScrollBar.AsNeeded

            TextArea {
                id: conditionScriptArea

                objectName: "conditionScriptTextArea"
                width: Math.max(conditionScriptScroll.availableWidth,
                                contentWidth + leftPadding + rightPadding)
                text: root.script
                enabled: !root.running
                selectByMouse: true
                wrapMode: TextEdit.NoWrap
                textFormat: TextEdit.PlainText
                font.family: "monospace"
                font.pixelSize: 12
                color: enabled ? AppTheme.text : AppTheme.disabledText
                selectionColor: AppTheme.selection
                selectedTextColor: AppTheme.text
                leftPadding: 8
                rightPadding: 8
                topPadding: 8
                bottomPadding: 8
                placeholderText: qsTr("kmh(car.speed) >= 200")
                Accessible.name: qsTr("Condition lines")
                Accessible.description: qsTr(
                    "Press Control Space for suggestions. Click a line number to enable or disable it.")
                background: Item {}

                onTextChanged: {
                    root.clearSymbolHover()
                    if (root.script !== text)
                        root.scriptEdited(text)
                    root.scheduleDocumentSync()
                }

                onCursorPositionChanged: {
                    root.scheduleDocumentSync()
                }

                Keys.onPressed: function(event) {
                    const controlSpace = event.key === Qt.Key_Space
                            && (event.modifiers & Qt.ControlModifier)
                    if (controlSpace) {
                        root.openCompletion(true)
                        event.accepted = true
                        return
                    }
                    const openingParenthesis = event.text === "("
                            || event.key === Qt.Key_ParenLeft
                    if (openingParenthesis
                            && selectionStart === selectionEnd
                            && root.applyFunctionCallPair()) {
                        event.accepted = true
                        return
                    }
                    if (!completionPopup.visible)
                        return
                    if (event.key === Qt.Key_Down) {
                        completionPopup.moveSelection(1)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Up) {
                        completionPopup.moveSelection(-1)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Return
                               || event.key === Qt.Key_Enter
                               || event.key === Qt.Key_Tab) {
                        root.applyCompletion(completionPopup.currentIndex)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Escape) {
                        completionPopup.close()
                        event.accepted = true
                    }
                }

                HoverHandler {
                    id: conditionSymbolHoverHandler

                    enabled: !root.running
                    acceptedDevices: PointerDevice.Mouse
                    onPointChanged: root.updateSymbolHover(point.position)
                    onHoveredChanged: {
                        if (!hovered)
                            root.clearSymbolHover()
                    }
                }
            }
        }
    }

    RowLayout {
        objectName: "conditionLibraryActions"
        Layout.fillWidth: true
        Layout.topMargin: 4
        spacing: 8

        ThemedButton {
            objectName: "conditionLoadButton"
            Layout.fillWidth: true
            enabled: !root.running
            highlighted: true
            text: qsTr("Load file")
            Accessible.description: qsTr(
                "Load a saved condition script.")
            onClicked: conditionScriptLibraryDialog.openForLoad()
        }

        ThemedButton {
            objectName: "conditionSaveButton"
            Layout.fillWidth: true
            enabled: !root.running
            text: qsTr("Save file")
            Accessible.description: qsTr(
                "Save this condition script as a named text file.")
            onClicked: conditionScriptLibraryDialog.openForSave(
                           conditionScriptArea.text)
        }
    }

    Label {
        Layout.fillWidth: true
        visible: root.assistance.parameterHint.signature !== undefined
                 && root.assistance.parameterHint.signature.length > 0
                 && conditionScriptArea.activeFocus
                 && !completionPopup.visible
        text: (root.assistance.parameterHint.signature || "")
              + (root.assistance.parameterHint.activeParameter !== undefined
                 ? qsTr(" \u00b7 argument %1").arg(
                       root.assistance.parameterHint.activeParameter + 1)
                 : "")
        color: AppTheme.info
        font.family: "monospace"
        font.pixelSize: 10
        elide: Text.ElideRight
    }

    ConditionCompletionPopup {
        id: completionPopup

        objectName: "conditionCompletionPopup"
        parent: Overlay.overlay
        suggestions: root.assistance.completions
        contextObject: root.assistance.completionContext
        parameterHint: root.assistance.parameterHint
        onCompletionRequested: function(index) {
            root.applyCompletion(index)
        }
    }

    Timer {
        id: symbolHoverTimer

        objectName: "conditionSymbolHoverTimer"
        interval: 350
        repeat: false
        onTriggered: root.openPendingSymbolHover()
    }

    ConditionSymbolHover {
        id: symbolHoverPopup

        parent: Overlay.overlay
    }

    ScriptLibraryDialog {
        id: conditionScriptLibraryDialog

        objectName: "conditionScriptLibraryDialog"
        parent: Overlay.overlay
        store: root.controller.scriptFileStore
        scriptKind: "conditions"
        onScriptLoaded: function(text) {
            conditionScriptArea.text = text
            conditionScriptArea.cursorPosition = text.length
            conditionScriptArea.forceActiveFocus()
        }
    }
}
