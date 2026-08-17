import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls
import "../settings" as SettingsControls

// One typed value slot of a block. Renders the inline editor for the
// field's kind, or the reporter block plugged into the slot. Number
// slots can receive value reporters through the attach button; the
// workspace highlights the armed slot and attaches the next value block
// chosen from the palette.
Item {
    id: root

    property var controller
    property int blockId: 0
    property var fieldData: null
    property bool compact: false
    property var armedSlot: null

    signal armRequested(int blockId, string key)

    readonly property bool mirrored:
        fieldData ? fieldData.kind === "mirrored" : false
    readonly property bool hasReporter:
        fieldData && fieldData.reporter !== undefined
        && fieldData.reporter !== null
    readonly property bool slotArmed: armedSlot !== null
                                      && fieldData !== null
                                      && armedSlot.blockId === root.blockId
                                      && armedSlot.key === fieldData.key

    visible: !mirrored && fieldData !== null
    implicitHeight: visible ? slotRow.implicitHeight : 0
    implicitWidth: slotRow.implicitWidth

    RowLayout {
        id: slotRow

        anchors.fill: parent
        spacing: 4

        Label {
            visible: root.fieldData && root.fieldData.label.length > 0
            text: root.fieldData ? root.fieldData.label : ""
            color: ThemeControls.AppTheme.textMuted
            font.pixelSize: root.compact ? 11 : 12
        }

        Loader {
            id: inlineEditor

            Layout.minimumWidth: root.compact ? 64 : 96
            Layout.preferredWidth: root.compact
                                   ? 72
                                   : root.fieldData
                                     && root.fieldData.kind === "line"
                                     ? 150
                                     : 110
            Layout.fillWidth: root.fieldData
                              && root.fieldData.kind === "line"
            Layout.preferredHeight: item ? item.implicitHeight : 36
            active: root.visible && !root.hasReporter && !root.mirrored

            sourceComponent: root.fieldData
                             ? (root.fieldData.kind === "enum"
                                ? enumEditor
                                : root.fieldData.kind === "boolean"
                                  ? booleanEditor
                                  : root.fieldData.kind === "line"
                                    ? lineEditor
                                    : numberEditor)
                             : null

            Component {
                id: numberEditor

                SettingsControls.ScrubNumberField {
                    objectName: "block" + root.blockId + "Field"
                                + root.fieldData.key
                    value: root.fieldData.value
                    enabled: !root.controller.running
                    integer: root.fieldData.decimals === 0
                    decimals: root.fieldData.decimals
                    dragStep: root.fieldData.step
                    minimum: root.fieldData.hasRange
                             ? root.fieldData.minimum : -100000000
                    maximum: root.fieldData.hasRange
                             ? root.fieldData.maximum : 100000000
                    leftPadding: 8
                    onEdited: value =>
                        root.controller.setBlockField(
                            root.blockId, root.fieldData.key, value)
                }
            }

            Component {
                id: lineEditor

                TextField {
                    id: lineSlotEditor

                    objectName: "block" + root.blockId + "Field"
                                + root.fieldData.key
                    enabled: !root.controller.running
                    selectByMouse: true
                    color: enabled ? ThemeControls.AppTheme.text
                                   : ThemeControls.AppTheme.disabledText
                    background: Rectangle {
                        radius: 4
                        color: enabled
                               ? ThemeControls.AppTheme.surface
                               : ThemeControls.AppTheme.disabledSurface
                        border.width: 1
                        border.color: parent.activeFocus
                                      ? ThemeControls.AppTheme.focus
                                      : ThemeControls.AppTheme.border
                    }

                    // Text edits sever a `text:` binding, so keep the
                    // field in sync imperatively from fieldData changes.
                    function synchronize() {
                        const next = root.fieldData ? root.fieldData.value : ""
                        if (!activeFocus && text !== next)
                            text = next
                    }

                    Component.onCompleted: synchronize()
                    Connections {
                        target: root
                        function onFieldDataChanged() {
                            lineSlotEditor.synchronize()
                        }
                    }
                    onEditingFinished:
                        root.controller.setBlockField(
                            root.blockId, root.fieldData.key, text)
                }
            }

            Component {
                id: booleanEditor

                ThemeControls.ThemedCheckBox {
                    id: booleanSlotEditor

                    objectName: "block" + root.blockId + "Switch"
                                + root.fieldData.key
                    enabled: !root.controller.running

                    // Toggling severs a `checked:` binding; sync from
                    // fieldData imperatively so external updates (text
                    // interchange, program reload) still land.
                    function synchronize() {
                        checked = root.fieldData
                                  && root.fieldData.value === "true"
                    }

                    Component.onCompleted: synchronize()
                    Connections {
                        target: root
                        function onFieldDataChanged() {
                            booleanSlotEditor.synchronize()
                        }
                    }
                    onToggled: root.controller.setBlockField(
                                   root.blockId, root.fieldData.key,
                                   checked ? "true" : "false")
                }
            }

            Component {
                id: enumEditor

                SettingsControls.StyledComboBox {
                    objectName: "block" + root.blockId + "Combo"
                                + root.fieldData.key
                    model: root.fieldData.enumValues
                    textRole: "label"
                    valueRole: "value"
                    boundIndex: {
                        const values = root.fieldData.enumValues
                        for (let index = 0; index < values.length; ++index) {
                            if (values[index].value === root.fieldData.value)
                                return index
                        }
                        return -1
                    }
                    enabled: !root.controller.running
                    onActivated: selectedIndex =>
                        root.controller.setBlockField(
                            root.blockId, root.fieldData.key,
                            valueAt(selectedIndex))
                }
            }
        }

        // Reporter chip: the value block plugged into this slot.
        Rectangle {
            id: reporterChip

            visible: root.hasReporter
            Layout.preferredWidth: chipRow.implicitWidth + 12
            Layout.preferredHeight: chipRow.implicitHeight + 8
            radius: 12
            color: ThemeControls.AppTheme.surfaceAlternate
            border.width: 1
            border.color: ThemeControls.AppTheme.border

            RowLayout {
                id: chipRow

                anchors.centerIn: parent
                spacing: 4

                Label {
                    text: root.hasReporter ? root.fieldData.reporter.label : ""
                    color: ThemeControls.AppTheme.textMuted
                    font.pixelSize: 11
                }

                Repeater {
                    model: root.hasReporter
                           ? root.fieldData.reporter.fields : []

                    delegate: Loader {
                        id: chipSlotLoader

                        required property var modelData

                        source: "BlockSlot.qml"

                        onLoaded: {
                            item.controller = root.controller
                            item.blockId = root.fieldData.reporter.blockId
                            item.compact = true
                            // Bindings (not one-time assignments) so a
                            // re-armed slot propagates into nested slots.
                            item.fieldData = Qt.binding(
                                        function() {
                                            return chipSlotLoader.modelData
                                        })
                            item.armedSlot = Qt.binding(
                                        function() { return root.armedSlot })
                        }

                        Connections {
                            target: chipSlotLoader.item

                            function onArmRequested(blockId, key) {
                                root.armRequested(blockId, key)
                            }
                        }
                    }
                }

                ThemeControls.ThemedToolButton {
                    objectName: "block" + root.blockId + "Detach"
                                + (root.fieldData ? root.fieldData.key : "")
                    text: "×"
                    enabled: !root.controller.running
                    onClicked: root.controller.detachReporter(
                                   root.blockId, root.fieldData.key)
                }
            }
        }

        ThemeControls.ThemedToolButton {
            id: attachButton

            objectName: "block" + root.blockId + "Attach"
                        + (root.fieldData ? root.fieldData.key : "")
            visible: root.fieldData && root.fieldData.kind === "number"
                     && !root.hasReporter && !root.compact
            text: root.slotArmed ? "◉" : "⊕"
            highlighted: root.slotArmed
            enabled: !root.controller.running
            onClicked: root.armRequested(root.blockId, root.fieldData.key)
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Plug a value block into this slot")
        }
    }
}
