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
            // Do not spin up Chromium on application startup or QML smoke
            // tests. The editor process exists only while Blocks is visible.
            active: root.visible && root.bridge !== null

            sourceComponent: Component {
                WebEngineView {
                    id: webView

                    objectName: "blocklyWebEngineView"
                    anchors.fill: parent
                    webChannel: channel
                    url: "qrc:///blockly/assets/blockly/index.html"
                    backgroundColor: ThemeControls.AppTheme.panel
                    focus: true
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
