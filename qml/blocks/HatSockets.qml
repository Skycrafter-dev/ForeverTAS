import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".." as ThemeControls

// The script hat's evaluator socket: the evaluation block that scores
// each run plus its rich detail panel. The evaluator renders through a
// runtime Loader because it is a full BlockView; static instantiation
// would create a compile-time type cycle. The canvas highlights this
// whole area while an evaluation reporter is dragged over it.
Item {
    id: root

    property var controller
    property var viewer
    property var viewport
    property var canvas: null
    property var armedSlot: null
    property var hatInformation: null

    objectName: "hatSockets"
    implicitWidth: socketColumn.implicitWidth
    implicitHeight: socketColumn.implicitHeight

    readonly property int evaluatorId: hatInformation
                                     ? hatInformation.evaluator : 0

    signal armRequested(int blockId, string key)

    // Drop-zone highlight while an evaluation reporter hovers.
    Rectangle {
        anchors.fill: parent
        anchors.margins: -4
        z: -1
        radius: 8
        visible: root.canvas !== null && root.canvas.evaluatorHighlight
        color: "transparent"
        border.width: 2
        border.color: ThemeControls.AppTheme.focus
    }

    ColumnLayout {
        id: socketColumn

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 6

        Label {
            text: qsTr("scored by")
            color: ThemeControls.AppTheme.textMuted
            font.italic: true
            font.pixelSize: 11
        }

        Loader {
            id: evaluatorLoader

            objectName: "blockViewEvaluator"
            Layout.fillWidth: true
            visible: item !== null
            active: root.evaluatorId !== 0
            source: "BlockView.qml"

            onLoaded: {
                item.controller = root.controller
                item.viewer = root.viewer
                item.viewport = root.viewport
                item.canvas = root.canvas
                item.blockId = root.evaluatorId
                item.armedSlot = Qt.binding(() => root.armedSlot)
                item.width = Qt.binding(() => evaluatorLoader.width)
                item.armRequested.connect((blockId, key) =>
                    root.armRequested(blockId, key))
                item.refresh()
            }
        }

        // Rich detail panel for collection-managed evaluators.
        Loader {
            id: evaluatorDetail

            objectName: "blockViewEvaluatorDetail"
            Layout.fillWidth: true
            visible: evaluatorLoader.item !== null
            readonly property string detailComponent: {
                const view = evaluatorLoader.item
                return view && view.blockInformation
                        && view.blockInformation.detail
                        ? view.blockInformation.detail : ""
            }
            active: detailComponent.length > 0
                    && evaluatorLoader.item !== null
            source: detailComponent
            function injectDetail() {
                const view = evaluatorLoader.item
                if (!item || !view)
                    return
                item.controller = root.controller
                item.viewer = root.viewer
                item.viewport = root.viewport
                item.blockId = view.blockId
                item.blockInformation = view.blockInformation
            }
            onLoaded: injectDetail()
            Connections {
                target: evaluatorLoader.item
                ignoreUnknownSignals: true
                function onBlockIdChanged() {
                    evaluatorDetail.injectDetail()
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.evaluatorId === 0
            wrapMode: Text.WordWrap
            text: qsTr("No evaluation selected — pick an evaluate block "
                       + "from the palette, or drag one here.")
            color: ThemeControls.AppTheme.warning
            font.pixelSize: 11
        }
    }
}
