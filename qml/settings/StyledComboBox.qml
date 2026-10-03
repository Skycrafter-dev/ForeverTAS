import QtQuick
import QtQuick.Controls
import ".." as ThemeControls

ComboBox {
    id: control

    // Keep model-owned selections independent of currentIndex, which user picks write.
    property var selectedValue: undefined
    function synchronizeSelection() {
        if (selectedValue !== undefined)
            currentIndex = indexOfValue(selectedValue)
    }
    onSelectedValueChanged: synchronizeSelection()
    onModelChanged: Qt.callLater(synchronizeSelection)
    onCountChanged: Qt.callLater(synchronizeSelection)
    Component.onCompleted: synchronizeSelection()

    readonly property bool slotStyled: true
    readonly property bool themedControl: true
    readonly property color effectiveBackgroundColor:
        !enabled ? ThemeControls.AppTheme.disabledSurface
        : pressed ? ThemeControls.AppTheme.controlPressed
        : hovered ? ThemeControls.AppTheme.controlHover
                  : ThemeControls.AppTheme.surface
    readonly property color effectiveBorderColor:
        !enabled ? ThemeControls.AppTheme.border
        : popup.visible ? ThemeControls.AppTheme.accent
        : activeFocus ? ThemeControls.AppTheme.focus
                      : ThemeControls.AppTheme.border
    readonly property color effectiveTextColor:
        enabled ? ThemeControls.AppTheme.text
                : ThemeControls.AppTheme.disabledText

    hoverEnabled: true
    implicitHeight: 36
    leftPadding: 12
    rightPadding: 38

    delegate: ThemeControls.ThemedItemDelegate {
        id: optionDelegate

        required property int index

        width: control.popup.width - 2
        height: 36
        leftPadding: 11
        rightPadding: 11
        text: control.textAt(index)
        highlighted: control.highlightedIndex === index
        hoverEnabled: true
        onClicked: {
            control.currentIndex = index
            control.activated(index)
            control.popup.close()
        }

        contentItem: Text {
            text: optionDelegate.text
            color: optionDelegate.effectiveTextColor
            font: optionDelegate.font
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

    }

    contentItem: Text {
        objectName: control.objectName.length > 0
                    ? control.objectName + "Content"
                    : "styledComboContent"
        text: control.displayText
        color: control.effectiveTextColor
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Item {
        width: 18
        height: 18
        x: control.width - width - 11
        y: Math.round((control.height - height) / 2)

        Rectangle {
            width: 7
            height: 1.5
            x: 2.5
            y: 8
            radius: 1
            rotation: 42
            color: control.enabled
                   ? ThemeControls.AppTheme.textMuted
                   : ThemeControls.AppTheme.disabledText
        }

        Rectangle {
            width: 7
            height: 1.5
            x: 8.5
            y: 8
            radius: 1
            rotation: -42
            color: control.enabled
                   ? ThemeControls.AppTheme.textMuted
                   : ThemeControls.AppTheme.disabledText
        }
    }

    background: Rectangle {
        radius: 7
        color: control.effectiveBackgroundColor
        border.width: control.popup.visible || control.activeFocus ? 2 : 1
        border.color: control.effectiveBorderColor
    }

    popup: Popup {
        objectName: control.objectName.length > 0
                    ? control.objectName + "Popup"
                    : "styledComboPopup"
        y: control.height - 1
        width: Math.min(control.width,
                        control.Window.width - leftMargin - rightMargin)
        implicitHeight: Math.min(
                            contentItem.implicitHeight + topPadding
                            + bottomPadding,
                            Math.max(
                                0,
                                control.Window.height
                                - control.mapToItem(
                                    null, 0, control.height - 1).y
                                - bottomMargin))
        topMargin: 8
        bottomMargin: 8
        leftMargin: 8
        rightMargin: 8
        padding: 1

        contentItem: ListView {
            objectName: control.objectName.length > 0
                        ? control.objectName + "PopupList"
                        : "styledComboPopupList"
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }
        }

        background: Rectangle {
            color: ThemeControls.AppTheme.surface
            border.width: 1
            border.color: ThemeControls.AppTheme.borderStrong
            radius: 6
        }
    }
}
