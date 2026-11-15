import QtQuick
import QtQuick.Controls
import ".." as ThemeControls

// Links a time window to others in the same color, as in TICK: windows on
// one chain change together. Click to link or unlink (keeping the color),
// right-click to pick the color; a window never chained links in white.
Item {
    id: root

    property var controller
    property string chainKey
    // Names the window in tooltips: "Input window".
    property string label: qsTr("This window")
    property bool running: false

    // Minecraft's 16 dye colors in its order, with the ink that reads on
    // each (TICK's chain-colors.ts).
    readonly property var colors: [
        { id: "white", label: qsTr("White"), hex: "#F9FFFE", ink: "#1D1D21" },
        { id: "orange", label: qsTr("Orange"), hex: "#F9801D", ink: "#1D1D21" },
        { id: "magenta", label: qsTr("Magenta"), hex: "#C74EBD", ink: "#1D1D21" },
        { id: "lightBlue", label: qsTr("Light blue"), hex: "#3AB3DA", ink: "#1D1D21" },
        { id: "yellow", label: qsTr("Yellow"), hex: "#FED83D", ink: "#1D1D21" },
        { id: "lime", label: qsTr("Lime"), hex: "#80C71F", ink: "#1D1D21" },
        { id: "pink", label: qsTr("Pink"), hex: "#F38BAA", ink: "#1D1D21" },
        { id: "gray", label: qsTr("Gray"), hex: "#474F52", ink: "#F9FFFE" },
        { id: "lightGray", label: qsTr("Light gray"), hex: "#9D9D97", ink: "#1D1D21" },
        { id: "cyan", label: qsTr("Cyan"), hex: "#169C9C", ink: "#1D1D21" },
        { id: "purple", label: qsTr("Purple"), hex: "#8932B8", ink: "#F9FFFE" },
        { id: "blue", label: qsTr("Blue"), hex: "#3C44AA", ink: "#F9FFFE" },
        { id: "brown", label: qsTr("Brown"), hex: "#835432", ink: "#F9FFFE" },
        { id: "green", label: qsTr("Green"), hex: "#5E7C16", ink: "#F9FFFE" },
        { id: "red", label: qsTr("Red"), hex: "#B02E26", ink: "#F9FFFE" },
        { id: "black", label: qsTr("Black"), hex: "#1D1D21", ink: "#F9FFFE" }
    ]

    readonly property var chain: {
        if (!controller || chainKey.length === 0)
            return { color: "white", linked: false }
        controller.windowChainRevision
        controller.modifierPasses
        controller.evaluationTargetId
        return controller.windowChain(chainKey)
    }
    readonly property bool linked: chain.linked === true
    readonly property var color:
        colors.find(entry => entry.id === chain.color) ?? colors[0]

    implicitWidth: 26
    implicitHeight: 24
    visible: chainKey.length > 0 && !!controller

    ThemeControls.ThemedIconButton {
        id: button
        objectName: "windowChainButton"
        anchors.fill: parent
        icon.source: "qrc:/icons/" + (root.linked ? "link" : "unlink") + ".svg"
        icon.width: 14
        icon.height: 14
        padding: 5
        enabled: !root.running
        ToolTip.visible: hovered
        ToolTip.delay: 500
        ToolTip.text: root.linked
            ? qsTr("%1 chain: every window on it changes together. Click to unlink, right-click for another color.").arg(root.color.label)
            : qsTr("Not chained: this window changes alone. Click to chain it, right-click to pick its chain color.")
        Accessible.name: root.label + ": " + (root.linked ? root.color.label + " chain" : qsTr("not chained"))
        onClicked: root.controller.setWindowChain(root.chainKey, root.chain.color, !root.linked)
        background: Rectangle {
            radius: 6 * ThemeControls.AppTheme.cornerScale
            color: root.linked ? root.color.hex : Qt.rgba(1, 1, 1, button.hovered ? 0.12 : 0.06)
            border.width: 1
            border.color: root.linked ? Qt.darker(root.color.hex, 1.3) : Qt.rgba(1, 1, 1, 0.16)
        }
        icon.color: root.linked ? root.color.ink : ThemeControls.AppTheme.textMuted
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            enabled: !root.running
            onClicked: colorPicker.open()
        }
    }

    // Not "palette": every item already has a palette property.
    Popup {
        id: colorPicker
        objectName: "windowChainPalette"
        y: root.height + 4
        x: Math.min(0, -width + root.width)
        padding: 8
        background: ThemeControls.GlassPopupBackground { radius: 10 * ThemeControls.AppTheme.cornerScale }
        Grid {
            columns: 8
            spacing: 5
            Repeater {
                model: root.colors
                Rectangle {
                    required property var modelData
                    width: 22
                    height: 22
                    radius: 5 * ThemeControls.AppTheme.cornerScale
                    color: modelData.hex
                    border.width: root.linked && root.chain.color === modelData.id ? 2 : 1
                    border.color: root.linked && root.chain.color === modelData.id
                                  ? ThemeControls.AppTheme.text : Qt.rgba(0, 0, 0, 0.4)
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        ToolTip.visible: containsMouse
                        ToolTip.delay: 400
                        ToolTip.text: modelData.label
                        onClicked: {
                            colorPicker.close()
                            root.controller.setWindowChain(root.chainKey, modelData.id, true)
                        }
                    }
                }
            }
        }
    }
}
