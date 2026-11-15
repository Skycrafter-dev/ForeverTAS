import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "settings"

// Edit > Preferences: the race's keys and gamepad buttons (click one to
// change it, then press the new key or button; Esc keeps the old one), the
// gamepad, and MCP (AI assistants driving ForeverTAS).
GlassDialog {
    id: dialog

    required property var raceControls
    required property var gamepad
    // The MCP bridge (null in tests that do not make one).
    property var mcp: null
    // 0: Controls, 1: Gamepad, 2: MCP.
    property alias section: sections.currentIndex
    // [action, index] while waiting for a key.
    property var capturing: null

    title: qsTr("Preferences")
    leftPadding: 16
    rightPadding: 16
    bottomPadding: 14
    modal: true
    dim: false
    closePolicy: capturing ? Popup.NoAutoClose : Popup.CloseOnEscape
    onClosed: capturing = null

    contentItem: ColumnLayout {
        spacing: 10

        TabBar {
            id: sections
            objectName: "preferencesSections"
            Layout.fillWidth: true
            background: Item {}
            ThemedTabButton { text: qsTr("Controls") }
            ThemedTabButton { text: qsTr("Gamepad") }
            ThemedTabButton { text: qsTr("MCP") }
        }

        StackLayout {
            Layout.fillWidth: true
            currentIndex: sections.currentIndex

        ColumnLayout {
            objectName: "controlsSection"
            spacing: 10

        Label {
            Layout.fillWidth: true
            text: qsTr("The race view takes these keys while it has the focus, and the gamepad's buttons while the window has it. Click one, then press the new key or button; Esc keeps the old one.")
            wrapMode: Text.WordWrap
            color: AppTheme.textMuted
            font.pixelSize: 11
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: qsTr("Savestates")
                color: AppTheme.text
            }
            ThemedIconButton {
                objectName: "fewerSavestatesButton"
                icon.source: "qrc:/icons/minus.svg"
                enabled: dialog.raceControls.savestateCount > 1
                onClicked: dialog.raceControls.setSavestates(dialog.raceControls.savestateCount - 1)
            }
            Label {
                objectName: "savestateCountLabel"
                text: dialog.raceControls.savestateCount
                color: AppTheme.text
                horizontalAlignment: Text.AlignHCenter
                Layout.preferredWidth: 24
            }
            ThemedIconButton {
                objectName: "moreSavestatesButton"
                icon.source: "qrc:/icons/plus.svg"
                enabled: dialog.raceControls.savestateCount < 12
                onClicked: dialog.raceControls.setSavestates(dialog.raceControls.savestateCount + 1)
            }
            Item { Layout.fillWidth: true }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(440, dialog.parent ? dialog.parent.height - 220 : 440)
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: parent.width
                spacing: 2

                Repeater {
                    model: dialog.raceControls.actions

                    delegate: RowLayout {
                        id: row
                        required property var modelData
                        readonly property var keys: {
                            dialog.raceControls.revision
                            return dialog.raceControls.bindings[modelData.id] || []
                        }
                        Layout.fillWidth: true
                        spacing: 6

                        Label {
                            Layout.fillWidth: true
                            text: row.modelData.label
                            color: AppTheme.text
                            elide: Text.ElideRight
                        }

                        Repeater {
                            model: 3
                            delegate: ThemedButton {
                                required property int index
                                readonly property bool waiting: dialog.capturing
                                    && dialog.capturing[0] === row.modelData.id
                                    && dialog.capturing[1] === index
                                objectName: "key_" + row.modelData.id + "_" + index
                                Layout.preferredWidth: 104
                                text: waiting ? qsTr("Press a key or button…")
                                      : index < row.keys.length ? dialog.raceControls.bindingText(row.keys[index])
                                      : "—"
                                enabled: index <= row.keys.length
                                highlighted: waiting
                                onClicked: {
                                    dialog.capturing = [row.modelData.id, Math.min(index, row.keys.length)]
                                    capture.forceActiveFocus()
                                }
                            }
                        }

                        ThemedIconButton {
                            icon.source: "qrc:/icons/x.svg"
                            enabled: row.keys.length > 0
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Nothing")
                            onClicked: {
                                for (let i = row.keys.length - 1; i >= 0; --i)
                                    dialog.raceControls.setBinding(row.modelData.id, i, null)
                            }
                        }
                    }
                }
            }
        }

        ThemedButton {
            objectName: "resetControlsButton"
            text: qsTr("Reset all keys and buttons")
            onClicked: dialog.raceControls.resetAll()
        }
        }

        ColumnLayout {
            objectName: "gamepadSection"
            spacing: 10

            Label {
                text: qsTr("Device")
                color: AppTheme.text
            }
            StyledComboBox {
                objectName: "gamepadDeviceSelector"
                Layout.fillWidth: true
                model: {
                    const pads = dialog.gamepad.devices
                    const choice = dialog.gamepad.choice
                    let items = [{ choice: "auto", name: qsTr("Automatic (the first connected)") },
                                 { choice: "none", name: qsTr("None") }]
                    for (const pad of pads)
                        items.push({ choice: pad.identity, name: pad.name })
                    if (choice !== "auto" && choice !== "none" && dialog.gamepad.selectedId === 0)
                        items.push({ choice: choice, name: qsTr("The chosen gamepad (not connected)") })
                    return items
                }
                textRole: "name"
                valueRole: "choice"
                // Once the list is there.
                currentIndex: {
                    count
                    const choice = dialog.gamepad.choice
                    let index = indexOfValue(choice)
                    // the chosen one, found on another port
                    for (let i = 0; index < 0 && i < dialog.gamepad.devices.length; ++i) {
                        if (dialog.gamepad.devices[i].id === dialog.gamepad.selectedId)
                            index = i + 2
                    }
                    return Math.max(0, index)
                }
                onActivated: dialog.gamepad.choice = currentValue
            }
            Label {
                Layout.fillWidth: true
                text: dialog.gamepad.status
                wrapMode: Text.Wrap
                color: AppTheme.textMuted
            }
            Label {
                objectName: "gamepadLastButton"
                Layout.fillWidth: true
                visible: dialog.gamepad.selectedId !== 0
                text: dialog.gamepad.lastButton.length > 0
                      ? qsTr("Last button pressed: %1").arg(dialog.gamepad.lastButton)
                      : qsTr("Press a button to check it reaches ForeverTAS.")
                color: AppTheme.text
            }
            Label {
                objectName: "gamepadUnmapped"
                Layout.fillWidth: true
                visible: dialog.gamepad.unmapped.length > 0
                text: qsTr("Seen, but without a gamepad layout (cannot drive): %1").arg(dialog.gamepad.unmapped.join(", "))
                wrapMode: Text.Wrap
                color: AppTheme.textMuted
            }
            RowLayout {
                Label {
                    text: qsTr("Steering deadzone")
                    color: AppTheme.text
                }
                ThemedSlider {
                    objectName: "gamepadDeadzone"
                    Layout.fillWidth: true
                    from: 0
                    to: 0.5
                    stepSize: 0.01
                    value: dialog.gamepad.deadzone
                    onMoved: dialog.gamepad.deadzone = value
                }
                Label {
                    text: Math.round(dialog.gamepad.deadzone * 100) + "%"
                    color: AppTheme.text
                }
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Left stick steers; right trigger or the south button accelerates, left trigger or the east button brakes. Like the keys, it takes over the race that plays. The other buttons do what Controls binds them to.")
                wrapMode: Text.Wrap
                color: AppTheme.textMuted
                font.pixelSize: 11
            }
            Item { Layout.fillHeight: true }
        }

        ColumnLayout {
            objectName: "mcpSection"
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: qsTr("MCP lets an AI assistant (Claude or any MCP client) drive ForeverTAS: load maps, test scripts, set up and run searches. What it changes shows here as you watch.")
                wrapMode: Text.WordWrap
                color: AppTheme.textMuted
                font.pixelSize: 11
            }
            SettingSwitch {
                objectName: "mcpEnabledSwitch"
                label: qsTr("Let MCP clients drive ForeverTAS")
                checked: dialog.mcp !== null && dialog.mcp.enabled
                running: dialog.mcp === null
                onToggled: checked => { if (dialog.mcp) dialog.mcp.enabled = checked }
            }
            Label {
                objectName: "mcpStatus"
                Layout.fillWidth: true
                text: dialog.mcp === null ? "" : dialog.mcp.status
                      + (dialog.mcp.clientCount > 0
                         ? " " + qsTr("%n client(s) connected.", "", dialog.mcp.clientCount) : "")
                wrapMode: Text.WordWrap
                color: AppTheme.text
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Off, nothing listens and nothing runs. To connect a client, add this to its MCP configuration; with ForeverTAS closed, the client opens it with MCP on.")
                wrapMode: Text.WordWrap
                color: AppTheme.textMuted
                font.pixelSize: 11
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 110
                clip: true
                TextArea {
                    id: mcpConfig
                    objectName: "mcpClientConfig"
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.NoWrap
                    font.family: "monospace"
                    font.pixelSize: 11
                    color: AppTheme.text
                    text: dialog.mcp === null ? "" : dialog.mcp.clientConfig
                    background: Rectangle {
                        color: AppTheme.surface
                        border.width: 1
                        border.color: AppTheme.border
                        radius: 6
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ThemedButton {
                    objectName: "copyMcpConfigButton"
                    text: qsTr("Copy")
                    enabled: mcpConfig.text.length > 0
                    onClicked: {
                        mcpConfig.selectAll()
                        mcpConfig.copy()
                        mcpConfig.deselect()
                    }
                }
            }
            Item { Layout.fillHeight: true }
        }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            ThemedButton {
                objectName: "closePreferencesButton"
                text: qsTr("Close")
                onClicked: dialog.close()
            }
        }

        // Takes the next gamepad button while a binding is being changed.
        Connections {
            target: dialog.gamepad
            function onButtonChanged(button, pressed) {
                if (!pressed || !dialog.capturing)
                    return
                dialog.raceControls.setBinding(dialog.capturing[0], dialog.capturing[1], { pad: button })
                dialog.capturing = null
            }
        }

        // Takes the next key while one is being changed.
        Item {
            id: capture
            focus: dialog.capturing !== null
            Keys.priority: Keys.BeforeItem
            Keys.onPressed: event => {
                event.accepted = true
                if (!dialog.capturing)
                    return
                const modifierKeys = [Qt.Key_Shift, Qt.Key_Control, Qt.Key_Alt, Qt.Key_Meta, Qt.Key_AltGr]
                if (modifierKeys.indexOf(event.key) >= 0)
                    return
                if (event.key !== Qt.Key_Escape)
                    dialog.raceControls.setBinding(dialog.capturing[0], dialog.capturing[1],
                                                   dialog.raceControls.bindingFromEvent(event))
                dialog.capturing = null
            }
        }
    }
}
