import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

// The ordered substack below a hat (mutation windows) or a container
// (mutation atoms). Children are full BlockViews loaded at runtime —
// static instantiation would create a compile-time type cycle with
// BlockView, which renders this sequence for containers and hats.
ColumnLayout {
    id: root

    property var controller
    property var viewer
    property var viewport
    property var armedSlot: null
    property var blockIds: []
    property string emptyHint: ""

    readonly property int count: blockIds.length

    signal armRequested(int blockId, string key)

    spacing: 4

    Repeater {
        model: root.blockIds

        delegate: Loader {
            id: blockLoader

            required property var modelData
            required property int index

            Layout.fillWidth: true
            source: "BlockView.qml"

            onLoaded: {
                item.objectName = "blockView" + blockLoader.modelData
                item.controller = root.controller
                item.viewer = root.viewer
                item.viewport = root.viewport
                item.blockId = blockLoader.modelData
                item.armedSlot = Qt.binding(() => root.armedSlot)
                item.moveIndex = blockLoader.index
                item.moveCount = Qt.binding(() => root.count)
                item.width = Qt.binding(() => blockLoader.width)
                item.armRequested.connect((blockId, key) =>
                    root.armRequested(blockId, key))
                item.refresh()
            }
        }
    }

    Label {
        Layout.fillWidth: true
        visible: root.count === 0 && root.emptyHint.length > 0
        wrapMode: Text.WordWrap
        text: root.emptyHint
        color: ThemeControls.AppTheme.textMuted
        font.italic: true
        font.pixelSize: 11
    }
}
