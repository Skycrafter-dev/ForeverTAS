import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ForeverTAS.Viewer
import "settings"

// File > Export to MP4: renders the selected run as the game shows a
// replay (its car, camera, and HUD), in the background.
GlassDialog {
    id: root

    property var viewer
    readonly property alias exporter: exporter

    title: qsTr("Export to MP4")
    modal: true
    // Closing leaves a running export going; the dialog shows it again.
    closePolicy: Popup.CloseOnEscape

    VideoExport {
        id: exporter
        objectName: "videoExport"
        viewer: root.viewer
    }

    readonly property var sizes: [
        { text: "1280 × 720", width: 1280, height: 720 },
        { text: "1920 × 1080", width: 1920, height: 1080 },
        { text: "2560 × 1440", width: 2560, height: 1440 },
        { text: "3840 × 2160", width: 3840, height: 2160 }
    ]

    contentItem: ColumnLayout {
        spacing: 8

        Label {
            text: qsTr("File")
            color: AppTheme.textMuted
            font.pixelSize: 11
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Label {
                objectName: "videoExportPath"
                Layout.fillWidth: true
                text: exporter.outputPath.length > 0
                      ? exporter.outputPath : qsTr("Not chosen yet")
                elide: Text.ElideMiddle
                color: exporter.outputPath.length > 0
                       ? AppTheme.text : AppTheme.textFaint
            }
            ThemedButton {
                objectName: "videoExportBrowse"
                text: qsTr("Choose...")
                enabled: !exporter.running
                onClicked: exporter.chooseOutput()
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 10
            rowSpacing: 6

            Label { text: qsTr("Camera"); color: AppTheme.textMuted }
            StyledComboBox {
                objectName: "videoExportCamera"
                Layout.fillWidth: true
                enabled: !exporter.running
                model: [{ text: qsTr("Camera 1"), value: 1 },
                        { text: qsTr("Camera 2"), value: 2 },
                        { text: qsTr("Camera 3"), value: 3 }]
                textRole: "text"
                valueRole: "value"
                currentIndex: indexOfValue(exporter.camera)
                onActivated: exporter.camera = currentValue
            }

            Label { text: qsTr("Size"); color: AppTheme.textMuted }
            StyledComboBox {
                objectName: "videoExportSize"
                Layout.fillWidth: true
                enabled: !exporter.running
                model: root.sizes
                textRole: "text"
                currentIndex: {
                    for (let i = 0; i < root.sizes.length; ++i) {
                        if (root.sizes[i].width === exporter.videoWidth
                                && root.sizes[i].height === exporter.videoHeight)
                            return i
                    }
                    return 1
                }
                onActivated: index => {
                    exporter.videoWidth = root.sizes[index].width
                    exporter.videoHeight = root.sizes[index].height
                }
            }

            Label { text: qsTr("Frame rate"); color: AppTheme.textMuted }
            StyledComboBox {
                objectName: "videoExportFps"
                Layout.fillWidth: true
                enabled: !exporter.running
                model: [{ text: "30 fps", value: 30 },
                        { text: "60 fps", value: 60 },
                        { text: "120 fps", value: 120 }]
                textRole: "text"
                valueRole: "value"
                currentIndex: indexOfValue(exporter.framesPerSecond)
                onActivated: exporter.framesPerSecond = currentValue
            }
        }

        Rectangle {
            objectName: "videoExportProgress"
            Layout.fillWidth: true
            Layout.topMargin: 4
            implicitHeight: 6
            radius: 3 * AppTheme.cornerScale
            color: AppTheme.control
            visible: exporter.running || exporter.progress > 0

            Rectangle {
                width: parent.width * Math.max(0, Math.min(1, exporter.progress))
                height: parent.height
                radius: parent.radius
                color: AppTheme.accent
            }
        }

        Label {
            objectName: "videoExportStatus"
            Layout.fillWidth: true
            visible: text.length > 0
            text: exporter.status
            wrapMode: Text.Wrap
            color: AppTheme.textMuted
            font.pixelSize: 11
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            spacing: 6

            ThemedButton {
                objectName: "videoExportShow"
                visible: !exporter.running && exporter.finishedPath.length > 0
                text: qsTr("Show file")
                onClicked: exporter.showFinished()
            }
            Item { Layout.fillWidth: true }
            ThemedButton {
                objectName: "videoExportClose"
                text: qsTr("Close")
                onClicked: root.close()
            }
            ThemedButton {
                objectName: "videoExportStart"
                text: exporter.running ? qsTr("Stop") : qsTr("Export")
                enabled: exporter.running
                         || (root.viewer && root.viewer.loaded
                             && root.viewer.runCount > 0)
                onClicked: {
                    if (exporter.running)
                        exporter.abort()
                    else if (exporter.outputPath.length > 0
                             || exporter.chooseOutput())
                        exporter.start()
                }
            }
        }
    }
}
