import QtQuick
import QtQuick.Controls
import QtWebEngine
import QtWebChannel
import ".." as ThemeControls

// Production block workbench host. Blockly owns editor interaction/rendering;
// the WebChannel bridge owns the native trust boundary and compilation.
Item {
    id: root

    required property var bridge
    property bool activated: false
    readonly property bool editorHasFocus: visible && webLoader.item !== null && webLoader.item.activeFocus
    onVisibleChanged: if (visible) activated = true
    Component.onCompleted: if (visible) activated = true

    objectName: "blockWorkspace"

    WebChannel {
        id: channel

        Component.onCompleted: {
            channel.registerObjects({"foreverBridge": root.bridge})
        }
    }

    Rectangle {
        anchors.fill: parent
        color: ThemeControls.AppTheme.panel

        Loader {
            id: webLoader

            anchors.fill: parent
            // Load lazily, then retain the editor: tab changes must preserve
            // pending edits, undo history, viewport, and configuration cards.
            active: root.activated && root.bridge !== null

            sourceComponent: Component {
                WebEngineView {
                    id: webView

                    objectName: "blocklyWebEngineView"
                    anchors.fill: parent
                    webChannel: channel
                    url: "qrc:///blockly/assets/blockly/index.html"
                    backgroundColor: ThemeControls.AppTheme.panel
                    focus: root.visible
                    activeFocusOnTab: true
                    Accessible.name: qsTr("Visual search block editor")
                    Accessible.description: qsTr("Blockly editor for composing search logic")

                    onNavigationRequested: request => {
                        const target = request.url.toString()
                        if (target.startsWith("qrc:"))
                            request.accept()
                        else
                            request.reject()
                    }
                }
            }
        }
    }
}
