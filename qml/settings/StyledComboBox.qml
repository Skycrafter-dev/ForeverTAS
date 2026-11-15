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

        width: control.popup.availableWidth
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
        radius: 7 * ThemeControls.AppTheme.cornerScale
        color: control.effectiveBackgroundColor
        border.width: control.popup.visible || control.activeFocus ? 2 : 1
        border.color: control.effectiveBorderColor
    }

    popup: Popup {
        objectName: control.objectName.length > 0
                    ? control.objectName + "Popup"
                    : "styledComboPopup"
        // Below the field, or above it when there is more room there (a
        // field near the window's bottom).
        property real roomBelow: 0
        property real roomAbove: 0
        // at most about twelve options; the rest scroll
        readonly property real wantedHeight:
            Math.min(contentItem.implicitHeight, 12.5 * 36)
            + topPadding + bottomPadding
        readonly property bool opensAbove:
            wantedHeight > roomBelow && roomAbove > roomBelow
        onAboutToShow: {
            const top = control.mapToItem(null, 0, 0).y
            roomAbove = Math.max(0, top + 1 - topMargin)
            roomBelow = Math.max(0, control.Window.height
                                    - (top + control.height - 1)
                                    - bottomMargin)
        }
        y: opensAbove ? 1 - height : control.height - 1
        width: Math.min(control.width,
                        control.Window.width - leftMargin - rightMargin)
        implicitHeight: Math.min(wantedHeight,
                                 opensAbove ? roomAbove : roomBelow)
        topMargin: 8
        bottomMargin: 8
        leftMargin: 8
        rightMargin: 8
        padding: 4

        contentItem: ListView {
            id: optionList
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
            // the wheel as in the panels: a notch is about three options
            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: event => {
                    const delta = event.pixelDelta.y !== 0
                        ? event.pixelDelta.y * 2.5 : event.angleDelta.y
                    optionList.contentY = Math.max(
                        0, Math.min(optionList.contentHeight - optionList.height,
                                    optionList.contentY - delta))
                    event.accepted = true
                }
            }
        }

        background: ThemeControls.GlassPopupBackground {
            radius: 10 * ThemeControls.AppTheme.cornerScale
            shadowRadius: 18
            shadowOffset: 8
        }
    }
}
