pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    property var suggestions: []
    property var contextObject: ({})
    property var parameterHint: ({})
    property int currentIndex: suggestions.length > 0 ? 0 : -1
    property bool pointerSelectionArmed: false
    property bool pointerPositionSampled: false
    property point lastPointerPosition: Qt.point(0, 0)
    readonly property var currentSuggestion:
        currentIndex >= 0 && currentIndex < suggestions.length
        ? suggestions[currentIndex] : ({})
    signal completionRequested(int index)

    modal: false
    focus: false
    closePolicy: Popup.CloseOnPressOutside
    padding: 2
    width: Math.min(341, Overlay.overlay.width - 24)
    height: Math.min(178, Overlay.overlay.height - 24)

    function iconLetter(kind) {
        if (kind === "object")
            return "{}"
        if (kind === "function")
            return "\u0192"
        if (kind === "vector")
            return "\u2192"
        if (kind === "number")
            return "#"
        return "\u2022"
    }

    function iconColor(kind) {
        if (kind === "object")
            return AppTheme.success
        if (kind === "function")
            return AppTheme.info
        if (kind === "vector")
            return AppTheme.accent
        return AppTheme.warning
    }

    function iconSurface(kind) {
        if (kind === "object")
            return AppTheme.successSoft
        if (kind === "function")
            return AppTheme.infoSoft
        if (kind === "vector")
            return AppTheme.accentSoft
        return AppTheme.warningSoft
    }

    function moveSelection(delta) {
        if (suggestions.length === 0) {
            currentIndex = -1
            return
        }
        currentIndex = (Math.max(0, currentIndex) + delta
                        + suggestions.length) % suggestions.length
        suggestionList.positionViewAtIndex(currentIndex, ListView.Contain)
    }

    function resetSelection() {
        currentIndex = suggestions.length > 0 ? 0 : -1
        if (currentIndex >= 0)
            suggestionList.positionViewAtBeginning()
    }

    function updatePointerSelection(index, scenePosition) {
        if (!pointerPositionSampled) {
            lastPointerPosition = scenePosition
            pointerPositionSampled = true
            return
        }
        if (Math.abs(scenePosition.x - lastPointerPosition.x) < 0.5
                && Math.abs(scenePosition.y - lastPointerPosition.y) < 0.5)
            return
        lastPointerPosition = scenePosition
        pointerSelectionArmed = true
        currentIndex = index
    }

    onOpened: {
        pointerSelectionArmed = false
        pointerPositionSampled = false
    }
    onClosed: {
        pointerSelectionArmed = false
        pointerPositionSampled = false
    }

    background: Rectangle {
        color: AppTheme.surface
        border.width: 1
        border.color: AppTheme.borderStrong
        radius: 6
    }

    contentItem: ColumnLayout {
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            visible: root.parameterHint.signature !== undefined
                     && root.parameterHint.signature.length > 0
            color: AppTheme.infoSoft

            Label {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                text: (root.parameterHint.signature || "")
                      + (root.parameterHint.activeParameter !== undefined
                         ? qsTr(" \u00b7 argument %1").arg(
                               root.parameterHint.activeParameter + 1)
                         : "")
                color: AppTheme.info
                font.family: "monospace"
                font.pixelSize: 10
                font.weight: Font.DemiBold
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            ListView {
                id: suggestionList

                objectName: "conditionCompletionList"
                Layout.preferredWidth: 200
                Layout.maximumWidth: 200
                Layout.fillHeight: true
                clip: true
                model: root.suggestions
                currentIndex: root.currentIndex
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {
                    id: suggestionScrollBar

                    objectName: "conditionCompletionListScrollBar"
                    policy: ScrollBar.AsNeeded
                }

                delegate: ThemedItemDelegate {
                    id: suggestionDelegate
                    required property int index
                    required property var modelData

                    objectName: "conditionCompletionItem_" + index
                    width: suggestionList.width
                    height: 34
                    leftPadding: 6
                    rightPadding: suggestionScrollBar.visible
                                  ? suggestionScrollBar.width + 10 : 6
                    topPadding: 0
                    bottomPadding: 0
                    focusPolicy: Qt.NoFocus
                    highlighted: index === root.currentIndex
                    onClicked: {
                        root.currentIndex = index
                        root.completionRequested(index)
                    }
                    Accessible.name: modelData.label
                    Accessible.description:
                        (modelData.kind || "") + ". "
                        + (modelData.description || "")

                    HoverHandler {
                        acceptedDevices: PointerDevice.Mouse
                        onPointChanged: root.updatePointerSelection(
                                            suggestionDelegate.index,
                                            point.scenePosition)
                    }

                    contentItem: RowLayout {
                        spacing: 6

                        Rectangle {
                            objectName: "conditionCompletionTypeIcon_"
                                        + suggestionDelegate.index
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 16
                            color: root.iconSurface(
                                       suggestionDelegate.modelData.kind)
                            border.width: 1
                            border.color: root.iconColor(
                                              suggestionDelegate.modelData.kind)
                            radius: 3

                            Label {
                                objectName: "conditionCompletionTypeGlyph_"
                                            + suggestionDelegate.index
                                anchors.fill: parent
                                text: root.iconLetter(
                                          suggestionDelegate.modelData.kind)
                                color: root.iconColor(
                                           suggestionDelegate.modelData.kind)
                                font.family: "monospace"
                                font.pixelSize: 8
                                font.weight: Font.Bold
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: suggestionDelegate.modelData.label || ""
                            color: AppTheme.text
                            font.family: "monospace"
                            font.pixelSize: 11
                            font.weight: Font.Medium
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.maximumWidth: 48
                            text: suggestionDelegate.modelData.kind || ""
                            color: AppTheme.textFaint
                            font.pixelSize: 9
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: AppTheme.border
            }

            Flickable {
                id: documentationFlickable

                objectName: "conditionCompletionDocumentation"
                Layout.preferredWidth: 136
                Layout.maximumWidth: 136
                Layout.fillHeight: true
                contentWidth: width
                contentHeight: documentationColumn.implicitHeight + 16
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }

                ColumnLayout {
                    id: documentationColumn

                    x: 8
                    y: 8
                    width: documentationFlickable.width - 16
                    spacing: 6
                    objectName: "conditionCompletionDocumentationContent"

                    Label {
                        Layout.fillWidth: true
                        visible: root.contextObject.symbol !== undefined
                                 && root.contextObject.symbol.length > 0
                        text: (root.contextObject.symbol || "") + "."
                        color: AppTheme.textMuted
                        font.family: "monospace"
                        font.pixelSize: 9
                        elide: Text.ElideRight
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Rectangle {
                            objectName: "conditionCompletionDocumentationTypeIcon"
                            Layout.preferredWidth: 18
                            Layout.preferredHeight: 18
                            color: root.iconSurface(
                                       root.currentSuggestion.kind)
                            border.width: 1
                            border.color: root.iconColor(
                                              root.currentSuggestion.kind)
                            radius: 3

                            Label {
                                objectName: "conditionCompletionDocumentationTypeGlyph"
                                anchors.fill: parent
                                text: root.iconLetter(
                                          root.currentSuggestion.kind)
                                color: root.iconColor(
                                           root.currentSuggestion.kind)
                                font.family: "monospace"
                                font.pixelSize: 8
                                font.weight: Font.Bold
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: root.currentSuggestion.symbol || ""
                            color: AppTheme.accent
                            font.family: "monospace"
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            wrapMode: Text.WrapAnywhere
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.currentSuggestion.kind
                              ? root.currentSuggestion.kind
                                + (root.currentSuggestion.unit
                                   ? " \u00b7 "
                                     + root.currentSuggestion.unit : "")
                              : ""
                        color: AppTheme.textMuted
                        font.pixelSize: 9
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.currentSuggestion.description || ""
                        color: AppTheme.text
                        font.pixelSize: 10
                        wrapMode: Text.WordWrap
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: exampleLabel.implicitHeight + 12
                        visible: exampleLabel.text.length > 0
                        color: AppTheme.codeAlternate
                        border.width: 1
                        border.color: AppTheme.border
                        radius: 3

                        Label {
                            id: exampleLabel
                            anchors.fill: parent
                            anchors.margins: 6
                            text: root.currentSuggestion.example || ""
                            color: AppTheme.text
                            font.family: "monospace"
                            font.pixelSize: 10
                            wrapMode: Text.WrapAnywhere
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            color: AppTheme.surfaceAlternate

            Label {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                text: qsTr("\u2191\u2193 choose   Enter insert   Esc close")
                color: AppTheme.textFaint
                font.pixelSize: 9
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
        }
    }
}
