pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root

    required property var store
    property string scriptKind: "conditions"
    property string sourceText: ""
    property string mode: "save"
    property var entries: []
    property int selectedIndex: -1
    property string statusText: ""
    property bool overwriteReady: false
    readonly property real frameRadius: 6
    readonly property string selectedName:
        selectedIndex >= 0 && selectedIndex < entries.length
        ? entries[selectedIndex].name : ""
    signal scriptLoaded(string text)

    objectName: "scriptLibraryDialog"
    title: mode === "save" ? qsTr("Save script") : qsTr("Load script")
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0
    width: Math.min(404, parent ? parent.width - 32 : 404)
    height: mode === "save" ? 216
                            : Math.min(356, parent ? parent.height - 32 : 356)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0

    function refresh() {
        entries = store ? store.files(scriptKind) : []
        selectedIndex = entries.length > 0 ? 0 : -1
    }

    function openForSave(text) {
        mode = "save"
        sourceText = text
        statusText = ""
        overwriteReady = false
        fileNameField.text = ""
        open()
        Qt.callLater(function() { fileNameField.forceActiveFocus() })
    }

    function openForLoad() {
        mode = "load"
        statusText = ""
        overwriteReady = false
        refresh()
        open()
        Qt.callLater(function() { scriptList.forceActiveFocus() })
    }

    function saveCurrent() {
        if (!store)
            return
        const result = store.save(scriptKind, fileNameField.text,
                                  sourceText, overwriteReady)
        statusText = result.message || ""
        if (result.ok) {
            refresh()
            close()
        } else {
            overwriteReady = result.exists === true
            fileNameField.forceActiveFocus()
        }
    }

    function loadCurrent() {
        if (!store || selectedName.length === 0)
            return
        const result = store.load(scriptKind, selectedName)
        statusText = result.message || ""
        if (result.ok) {
            scriptLoaded(result.content || "")
            close()
        }
    }

    Overlay.modal: Rectangle {
        color: AppTheme.scrim
    }

    background: Rectangle {
        objectName: "scriptLibraryDialogFrame"
        color: AppTheme.surface
        border.width: 1
        border.color: AppTheme.borderStrong
        radius: root.frameRadius
    }

    header: Item {
        implicitHeight: 44

        Rectangle {
            id: headerSurface

            objectName: "scriptLibraryDialogHeaderSurface"
            anchors.fill: parent
            anchors.leftMargin: 1
            anchors.rightMargin: 1
            anchors.topMargin: 1
            color: AppTheme.surfaceAlternate
            radius: root.frameRadius - 1

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: parent.radius
                color: parent.color
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: AppTheme.border
            }
        }

        Label {
            anchors.left: parent.left
            anchors.right: closeButton.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: root.title
            color: AppTheme.text
            font.pixelSize: 14
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }

        ThemedButton {
            id: closeButton
            objectName: "closeScriptLibraryButton"
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 28
            height: 28
            implicitWidth: 28
            implicitHeight: 28
            leftPadding: 0
            rightPadding: 0
            topPadding: 0
            bottomPadding: 0
            flat: true
            text: "\u00d7"
            Accessible.name: qsTr("Close")
            onClicked: root.close()
        }
    }

    contentItem: ColumnLayout {
        spacing: 8

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 12
            visible: root.mode === "save"
            spacing: 6

            Label {
                Layout.fillWidth: true
                text: qsTr("File name")
                color: AppTheme.textMuted
                font.pixelSize: 11
            }

            TextField {
                id: fileNameField
                objectName: "scriptLibraryFileNameField"
                Layout.fillWidth: true
                selectByMouse: true
                placeholderText: qsTr("my-script")
                Accessible.name: qsTr("Script file name")
                onTextEdited: {
                    root.overwriteReady = false
                    root.statusText = ""
                }
                Keys.onReturnPressed: root.saveCurrent()
                Keys.onEnterPressed: root.saveCurrent()
            }

            Label {
                Layout.fillWidth: true
                text: fileNameField.text.trim().length > 0
                      ? (fileNameField.text.toLowerCase().endsWith(".txt")
                         ? fileNameField.text.trim()
                         : fileNameField.text.trim() + ".txt")
                      : qsTr("Saved as a .txt file")
                color: AppTheme.textFaint
                font.pixelSize: 10
                elide: Text.ElideMiddle
            }

            Item {
                Layout.fillHeight: true
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.topMargin: 8
            visible: root.mode === "load"
            spacing: 4

            Label {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.entries.length === 0
                text: qsTr("No saved .txt scripts yet.")
                color: AppTheme.textMuted
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            ListView {
                id: scriptList
                objectName: "scriptLibraryFileList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.entries.length > 0
                clip: true
                model: root.entries
                currentIndex: root.selectedIndex
                focus: root.mode === "load"
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }
                Keys.onReturnPressed: root.loadCurrent()
                Keys.onEnterPressed: root.loadCurrent()

                delegate: ThemedItemDelegate {
                    id: fileDelegate
                    required property int index
                    required property var modelData

                    objectName: "scriptLibraryFileItem_" + index
                    width: scriptList.width
                    height: 36
                    leftPadding: 8
                    rightPadding: 8
                    topPadding: 0
                    bottomPadding: 0
                    highlighted: index === root.selectedIndex
                    Accessible.name: modelData.name
                    onHoveredChanged: {
                        if (hovered)
                            root.selectedIndex = index
                    }
                    onClicked: root.selectedIndex = index
                    onDoubleClicked: {
                        root.selectedIndex = index
                        root.loadCurrent()
                    }

                    contentItem: RowLayout {
                        spacing: 8

                        Rectangle {
                            objectName: "scriptLibraryFileIcon_"
                                        + fileDelegate.index
                            Layout.preferredWidth: 18
                            Layout.preferredHeight: 20
                            Layout.alignment: Qt.AlignVCenter
                            color: AppTheme.infoSoft
                            border.width: 1
                            border.color: AppTheme.info
                            radius: 2

                            Label {
                                anchors.centerIn: parent
                                text: "T"
                                color: AppTheme.info
                                font.pixelSize: 9
                                font.weight: Font.Bold
                            }
                        }

                        Label {
                            objectName: "scriptLibraryFileLabel_"
                                        + fileDelegate.index
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            text: fileDelegate.modelData.name || ""
                            color: AppTheme.text
                            font.family: "monospace"
                            font.pixelSize: 11
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideMiddle
                        }
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            visible: text.length > 0
            text: root.statusText
            color: root.overwriteReady ? AppTheme.warning : AppTheme.error
            font.pixelSize: 10
            wrapMode: Text.WordWrap
        }
    }

    footer: Item {
        implicitHeight: 48

        Rectangle {
            id: footerSurface

            objectName: "scriptLibraryDialogFooterSurface"
            anchors.fill: parent
            anchors.leftMargin: 1
            anchors.rightMargin: 1
            anchors.bottomMargin: 1
            color: AppTheme.surfaceAlternate
            radius: root.frameRadius - 1

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: parent.radius
                color: parent.color
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: AppTheme.border
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 8

            Item {
                Layout.fillWidth: true
            }

            ThemedButton {
                objectName: "cancelScriptLibraryButton"
                text: qsTr("Cancel")
                onClicked: root.close()
            }

            ThemedButton {
                objectName: "confirmScriptLibraryButton"
                text: root.mode === "save"
                      ? (root.overwriteReady ? qsTr("Replace") : qsTr("Save"))
                      : qsTr("Load")
                highlighted: true
                enabled: root.mode === "save"
                         ? fileNameField.text.trim().length > 0
                         : root.selectedName.length > 0
                onClicked: root.mode === "save"
                           ? root.saveCurrent() : root.loadCurrent()
            }
        }
    }
}
