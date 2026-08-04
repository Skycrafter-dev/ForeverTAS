import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml.Models
import QtQuick3D
import "settings"
import ForeverTAS.Viewer 1.0

ApplicationWindow {
    id: window

    required property var controller
    required property var viewer

    property string renderMode: "textured"
    property bool codeEditorExpanded: false
    readonly property bool rayTracingEnabled:
        renderMode === "textured-rt"
    property real measuredFps: 0
    property int framesSinceSample: 0
    readonly property var settingsWheelRedirectorObject:
        settingsWheelRedirector

    Binding {
        target: AppTheme
        property: "dark"
        value: window.controller.darkMode
    }

    FrameAnimation {
        id: frameRateMonitor
        objectName: "frameRateMonitor"
        running: window.visible
        onTriggered: ++window.framesSinceSample
    }

    Timer {
        interval: 1000
        repeat: true
        running: window.visible
        onTriggered: {
            window.measuredFps = window.framesSinceSample
            window.framesSinceSample = 0
        }
    }

    function runColor(index) {
        const colors = ["#ff8a3d", "#3d8dff", "#63c77b", "#c57aeb",
                        "#e7c24f", "#54c7c1"]
        const normalized = Math.max(0, index) % colors.length
        return colors[normalized]
    }

    function stepViewerTick(delta) {
        if (!window.viewer.loaded) {
            return
        }
        window.viewer.pause()
        window.viewer.currentTick = window.viewer.currentTick + delta
    }

    function manualControlForKey(key) {
        if (key === Qt.Key_Left || key === Qt.Key_A || key === Qt.Key_Q)
            return "left"
        if (key === Qt.Key_Right || key === Qt.Key_D)
            return "right"
        if (key === Qt.Key_Up || key === Qt.Key_W || key === Qt.Key_Z)
            return "accelerate"
        if (key === Qt.Key_Down || key === Qt.Key_S)
            return "brake"
        return ""
    }

    function manualActionForKey(key) {
        if (key === Qt.Key_Delete)
            return "give-up"
        if (key === Qt.Key_Return || key === Qt.Key_Enter
                || key === Qt.Key_Backspace)
            return "respawn"
        return ""
    }

    function handleManualActionKey(key, active, autoRepeat) {
        if (!window.viewer.manualDriving)
            return false
        const action = manualActionForKey(key)
        if (action.length === 0)
            return false
        if (active && !autoRepeat) {
            if (action === "give-up")
                window.viewer.giveUpManualDrive()
            else
                window.viewer.respawnManualDrive()
        }
        return true
    }

    function handleManualKey(event, active) {
        if (!window.viewer.manualDriving
            && !(window.viewer.takeOverOnInput
                 && window.viewer.playing))
            return
        if (handleManualActionKey(
                    event.key, active, event.isAutoRepeat)) {
            event.accepted = true
            return
        }
        const control = manualControlForKey(event.key)
        if (control.length === 0)
            return
        event.accepted = true
        if (!event.isAutoRepeat)
            window.viewer.setManualInput(control, active)
    }

    function commitBaseInputScript() {
        if (window.controller.baseInputScript !== baseInputScriptArea.text)
            window.controller.baseInputScript = baseInputScriptArea.text
    }

    onActiveChanged: {
        if (!active) {
            window.viewer.releaseManualInputs()
            viewport.releaseFreeMovement()
        }
    }

    Component.onCompleted: Qt.callLater(function() {
        if (window.viewer.loaded)
            viewport.resetCameraFocus()
    })

    Connections {
        target: window.viewer

        function onSceneChanged() {
            viewport.resetCameraFocus()
        }
    }

    width: 1420
    height: 820
    minimumWidth: 1240
    minimumHeight: 580
    visible: true
    title: qsTr("ForeverTAS")
    color: AppTheme.window

    Dialog {
        id: replaceBaseInputScriptDialog

        objectName: "replaceBaseInputScriptDialog"
        anchors.centerIn: parent
        modal: true
        title: qsTr("Replace base input script?")
        onAccepted: window.controller.extractReplayInputs()

        footer: DialogButtonBox {
            spacing: 8

            ThemedButton {
                objectName: "confirmReplaceBaseInputButton"
                text: qsTr("Yes")
                highlighted: true
                DialogButtonBox.buttonRole: DialogButtonBox.YesRole
            }

            ThemedButton {
                objectName: "cancelReplaceBaseInputButton"
                text: qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }

            onAccepted: replaceBaseInputScriptDialog.accept()
            onRejected: replaceBaseInputScriptDialog.reject()

            background: Rectangle {
                color: AppTheme.panel
            }
        }

        Label {
            width: 360
            text: qsTr("Extracting replay inputs will replace the current script.")
            wrapMode: Text.WordWrap
        }
    }

    Dialog {
        id: telemetryEditorDialog
        objectName: "telemetryEditorDialog"
        anchors.centerIn: parent
        modal: true
        width: Math.min(620, window.width - 48)
        title: qsTr("Scripted telemetry")
        onOpened: {
            telemetryScriptEditor.text = window.viewer.telemetryScript
            telemetryScriptEditor.forceActiveFocus()
        }
        onAccepted:
            window.viewer.telemetryScript = telemetryScriptEditor.text

        footer: DialogButtonBox {
            spacing: 8

            ThemedButton {
                objectName: "resetTelemetryScriptButton"
                text: qsTr("Reset")
                DialogButtonBox.buttonRole: DialogButtonBox.ResetRole
                onClicked:
                    telemetryScriptEditor.text =
                        window.viewer.defaultTelemetryScript
            }

            ThemedButton {
                objectName: "cancelTelemetryScriptButton"
                text: qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }

            ThemedButton {
                objectName: "applyTelemetryScriptButton"
                text: qsTr("Apply")
                highlighted: true
                enabled: telemetryScriptErrorLabel.text.length === 0
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }

            onAccepted: telemetryEditorDialog.accept()
            onRejected: telemetryEditorDialog.reject()

            background: Rectangle {
                color: AppTheme.panel
            }
        }

        contentItem: ColumnLayout {
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                StyledComboBox {
                    id: telemetryFieldCombo
                    objectName: "telemetryFieldCombo"
                    Layout.fillWidth: true
                    model: [
                        "{camera.x:2}", "{camera.y:2}", "{camera.z:2}",
                        "{car.x:2}", "{car.y:2}", "{car.z:2}",
                        "{car.velocity.x:2}", "{car.velocity.y:2}",
                        "{car.velocity.z:2}", "{car.speed:2}",
                        "{car.speedKph:2}", "{input.accelerate:2}",
                        "{input.brake:2}", "{input.steering:2}",
                        "{race.checkpoints:0}",
                        "{race.totalCheckpoints:0}", "{race.laps:0}",
                        "{race.totalLaps:0}", "{race.finished}",
                        "{time.ms:0}", "{time.s:2}", "{tick:0}",
                        "{run.name}"
                    ]
                    Accessible.name: qsTr("Telemetry field")
                }

                ThemedButton {
                    objectName: "insertTelemetryFieldButton"
                    text: qsTr("Insert")
                    onClicked: {
                        const token = telemetryFieldCombo.currentText
                        telemetryScriptEditor.insert(
                                    telemetryScriptEditor.cursorPosition,
                                    token)
                        telemetryScriptEditor.forceActiveFocus()
                    }
                }
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 150
                clip: true

                TextArea {
                    id: telemetryScriptEditor
                    objectName: "telemetryScriptEditor"
                    color: AppTheme.text
                    selectionColor: AppTheme.accent
                    selectedTextColor: AppTheme.textOnAccent
                    font.family: "monospace"
                    font.pixelSize: 12
                    wrapMode: TextEdit.Wrap
                    background: Rectangle {
                        color: AppTheme.surface
                        border.width: telemetryScriptEditor.activeFocus ? 2 : 1
                        border.color: telemetryScriptEditor.activeFocus
                                      ? AppTheme.focus : AppTheme.borderStrong
                        radius: 5
                    }
                }
            }

            Label {
                id: telemetryScriptErrorLabel
                objectName: "telemetryScriptErrorLabel"
                Layout.fillWidth: true
                visible: text.length > 0
                text: window.viewer.telemetryScriptError(
                          telemetryScriptEditor.text)
                color: AppTheme.error
                wrapMode: Text.WordWrap
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.minimumHeight: 42
                Layout.preferredHeight: Math.min(
                                            120,
                                            telemetryScriptPreview
                                                .implicitHeight + 18)
                color: AppTheme.viewerOverlay
                border.width: 1
                border.color: AppTheme.viewerOverlayBorder
                radius: 6
                clip: true

                Label {
                    id: telemetryScriptPreview
                    objectName: "telemetryScriptPreview"
                    anchors.fill: parent
                    anchors.margins: 9
                    text: viewport.renderTelemetry(
                              telemetryScriptEditor.text)
                    color: AppTheme.viewerOverlayText
                    font.family: "monospace"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    palette {
        window: AppTheme.window
        windowText: AppTheme.text
        base: AppTheme.surface
        alternateBase: AppTheme.surfaceAlternate
        text: AppTheme.text
        brightText: AppTheme.text
        button: AppTheme.control
        buttonText: AppTheme.text
        light: AppTheme.surfaceRaised
        midlight: AppTheme.border
        mid: AppTheme.borderStrong
        dark: AppTheme.textFaint
        shadow: AppTheme.scrim
        highlight: AppTheme.accent
        highlightedText: AppTheme.textOnAccent
        placeholderText: AppTheme.textFaint
        toolTipBase: AppTheme.tooltipBackground
        toolTipText: AppTheme.tooltipText
    }

    Overlay.modal: Rectangle {
        color: AppTheme.scrim
    }

    Overlay.modeless: Rectangle {
        color: "transparent"
    }

    Shortcut {
        objectName: "stepBackwardShortcut"
        sequence: "Left"
        context: Qt.ApplicationShortcut
        autoRepeat: true
        enabled: window.viewer.runCount > 0
                 && !window.viewer.manualDriving
                 && !viewport.freeCamera
                 && !(window.viewer.takeOverOnInput
                      && window.viewer.playing)
        onActivated: window.stepViewerTick(-1)
    }

    Shortcut {
        objectName: "stepForwardShortcut"
        sequence: "Right"
        context: Qt.ApplicationShortcut
        autoRepeat: true
        enabled: window.viewer.runCount > 0
                 && !window.viewer.manualDriving
                 && !viewport.freeCamera
                 && !(window.viewer.takeOverOnInput
                      && window.viewer.playing)
        onActivated: window.stepViewerTick(1)
    }

    Shortcut {
        objectName: "saveBaseInputScriptShortcut"
        sequences: [StandardKey.Save]
        context: Qt.ApplicationShortcut
        enabled: baseInputScriptArea.activeFocus
        onActivated: window.commitBaseInputScript()
    }

    Shortcut {
        objectName: "undoBaseInputScriptShortcut"
        sequences: [StandardKey.Undo]
        context: Qt.ApplicationShortcut
        enabled: baseInputScriptArea.activeFocus
                 && (baseInputScriptArea.text
                     !== window.controller.baseInputScript
                     || window.controller.canUndoBaseInputScript)
        onActivated: {
            if (baseInputScriptArea.text
                    !== window.controller.baseInputScript) {
                baseInputScriptArea.undo()
            } else {
                window.controller.undoBaseInputScript()
            }
        }
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        Rectangle {
            id: workspaceContent

            objectName: "workspaceContent"
            SplitView.fillWidth: true
            SplitView.minimumWidth: 680
            visible: !window.codeEditorExpanded
            color: AppTheme.window

            RowLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    id: timelinePanel
                    objectName: "timelinePanel"
                    Layout.preferredWidth: 252
                    Layout.minimumWidth: 220
                    Layout.maximumWidth: 300
                    Layout.fillHeight: true
                    color: AppTheme.panel

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 52
                            color: AppTheme.panelAlternate

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 14
                                anchors.rightMargin: 14
                                anchors.topMargin: 8
                                anchors.bottomMargin: 8
                                spacing: 2

                                Label {
                                    objectName: "timelineTimeLabel"
                                    text: window.viewer.timeText
                                    color: AppTheme.text
                                    font.family: "monospace"
                                    font.pixelSize: 15
                                    font.weight: Font.Medium
                                }

                                Label {
                                    text: window.viewer.runCount > 0
                                          ? qsTr("Tick %1 / %2 · 100 Hz")
                                                .arg(window.viewer.currentTick)
                                                .arg(Math.max(0,
                                                              window.viewer.tickCount - 1))
                                          : window.viewer.loaded
                                            ? qsTr("Map loaded · no search run")
                                            : qsTr("100 physics ticks / second")
                                    color: AppTheme.textMuted
                                    font.pixelSize: 10
                                }
                            }
                        }

                        RaceTimeline {
                            id: raceTimeline
                            objectName: "raceTimeline"
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            viewer: window.viewer
                            darkMode: AppTheme.dark
                            enabled: window.viewer.runCount > 0
                                     && !window.viewer.manualDriving
                            pixelsPerTick: 3
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 50
                            color: AppTheme.panelAlternate

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                spacing: 10

                                Rectangle {
                                    Layout.preferredWidth: 10
                                    Layout.preferredHeight: 10
                                    radius: 2
                                    color: AppTheme.info
                                }
                                Label {
                                    text: qsTr("Steer")
                                    color: AppTheme.viewerOverlayMuted
                                    font.pixelSize: 10
                                }
                                Rectangle {
                                    Layout.preferredWidth: 10
                                    Layout.preferredHeight: 10
                                    radius: 2
                                    color: AppTheme.accent
                                }
                                Label {
                                    text: qsTr("Gas")
                                    color: AppTheme.viewerOverlayMuted
                                    font.pixelSize: 10
                                }
                                Rectangle {
                                    Layout.preferredWidth: 10
                                    Layout.preferredHeight: 10
                                    radius: 2
                                    color: AppTheme.error
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Brake")
                                    color: AppTheme.viewerOverlayMuted
                                    font.pixelSize: 10
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.fillHeight: true
                    color: AppTheme.border
                }

                Item {
                    id: viewport
                    objectName: "raceViewport"
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    property real orbitYaw: 35
                    property real orbitPitch: -20
                    property real orbitDistance: 38
                    property real cameraFieldOfView: 55
                    property bool freeCamera: false
                    property bool orbitalCamera: false
                    readonly property vector3d sceneCameraPosition:
                        viewCamera.scenePosition
                    readonly property var sceneCameraRotation:
                        viewCamera.sceneRotation
                    property vector3d freeCameraPosition:
                        Qt.vector3d(0, 0, 0)
                    function renderTelemetry(script) {
                        // These explicit reads keep the binding live while the
                        // C++ formatter samples the selected run internally.
                        const timelineTime = window.viewer.timeMs
                        const selectedCarPosition = window.viewer.carPosition
                        return window.viewer.renderTelemetry(
                                    script, viewCamera.scenePosition)
                    }
                    readonly property bool carCameraActive:
                        !freeCamera && !orbitalCamera && !cuboidFocused
                        && window.viewer.carCameraAvailable
                        && window.viewer.runCount > 0
                    readonly property vector3d cameraForward: {
                        if (carCameraActive) {
                            const dx = window.viewer.carCameraTarget.x
                                     - window.viewer.carCameraPosition.x
                            const dy = window.viewer.carCameraTarget.y
                                     - window.viewer.carCameraPosition.y
                            const dz = window.viewer.carCameraTarget.z
                                     - window.viewer.carCameraPosition.z
                            const length = Math.sqrt(dx * dx + dy * dy
                                                   + dz * dz)
                            if (length > 0.000001)
                                return Qt.vector3d(dx / length, dy / length,
                                                   dz / length)
                        }
                        const yaw = orbitYaw * Math.PI / 180
                        const pitch = orbitPitch * Math.PI / 180
                        const pitchCos = Math.cos(pitch)
                        return Qt.vector3d(
                            -Math.sin(yaw) * pitchCos,
                            Math.sin(pitch),
                            -Math.cos(yaw) * pitchCos)
                    }
                    readonly property vector3d freeCameraTarget:
                        Qt.vector3d(
                            freeCameraPosition.x
                                + cameraForward.x * orbitDistance,
                            freeCameraPosition.y
                                + cameraForward.y * orbitDistance,
                            freeCameraPosition.z
                                + cameraForward.z * orbitDistance)
                    property bool hasObjectFocus: false
                    property bool cuboidFocused: false
                    property int exactWhiteboardBoardIndex: -1
                    property string exactWhiteboardBoardId: ""
                    property int lastFocusedWhiteboardIndex: -1
                    property string lastFocusedWhiteboardId: ""
                    property vector3d cuboidFocusCenter:
                        Qt.vector3d(0, 0, 0)
                    readonly property vector3d cameraTarget:
                        freeCamera ? freeCameraTarget
                        : cuboidFocused ? cuboidFocusCenter
                        : carCameraActive
                          ? window.viewer.carCameraTarget
                          : window.viewer.carPosition
                    readonly property string cameraFocusMode:
                        freeCamera ? "free"
                        : cuboidFocused ? "object"
                        : orbitalCamera ? "orbital"
                        : carCameraActive ? "preset" : "car"
                    property bool freeMoveForward: false
                    property bool freeMoveBackward: false
                    property bool freeMoveLeft: false
                    property bool freeMoveRight: false
                    property bool freeMoveUp: false
                    property bool freeMoveDown: false
                    property real freeMoveSpeed: 0
                    property real freeMoveStartedAt: 0
                    property real freeMoveLastStepAt: 0
                    readonly property real freeMoveInitialSpeed: 9
                    readonly property real freeMoveAcceleration: 30
                    readonly property real freeMoveMaximumSpeed: 360
                    property real viewRotationTargetYaw: orbitYaw
                    property real viewRotationTargetPitch: orbitPitch
                    property bool viewRotationSmoothing: false
                    property bool cuboidDragActive: false
                    property bool cuboidPointerCaptured: false
                    property string cuboidDragKind: ""
                    property string cuboidDragAxis: ""
                    property real cuboidDragX: 0
                    property real cuboidDragY: 0
                    property bool customVertexDragActive: false
                    property int customVertexDragIndex: -1
                    property bool customDepthDragActive: false
                    property bool poseDragActive: false
                    property string poseDragKind: ""
                    property string poseDragAxis: ""
                    property bool exportingWhiteboardImage: false
                    property url whiteboardImageExportUrl: ""

                    function hasFreeMovement() {
                        return freeMoveForward || freeMoveBackward
                            || freeMoveLeft || freeMoveRight
                            || freeMoveUp || freeMoveDown
                    }

                    function releaseFreeMovement() {
                        freeMoveForward = false
                        freeMoveBackward = false
                        freeMoveLeft = false
                        freeMoveRight = false
                        freeMoveUp = false
                        freeMoveDown = false
                        freeMoveSpeed = 0
                        freeMoveStartedAt = 0
                        freeMoveLastStepAt = 0
                        freeCameraMovementTimer.stop()
                    }

                    function setFreeMovement(direction, active) {
                        const hadMovement = hasFreeMovement()
                        if (direction === "forward")
                            freeMoveForward = active
                        else if (direction === "backward")
                            freeMoveBackward = active
                        else if (direction === "left")
                            freeMoveLeft = active
                        else if (direction === "right")
                            freeMoveRight = active
                        else if (direction === "up")
                            freeMoveUp = active
                        else if (direction === "down")
                            freeMoveDown = active
                        else
                            return false

                        const hasMovement = hasFreeMovement()
                        if (!hadMovement && hasMovement) {
                            const now = Date.now()
                            freeMoveStartedAt = now
                            freeMoveLastStepAt = now
                            freeMoveSpeed = freeMoveInitialSpeed
                            freeCameraMovementTimer.start()
                        } else if (!hasMovement) {
                            releaseFreeMovement()
                        }
                        return true
                    }

                    function freeMovementForKey(key) {
                        if (key === Qt.Key_Up || key === Qt.Key_W
                                || key === Qt.Key_Z)
                            return "forward"
                        if (key === Qt.Key_Down || key === Qt.Key_S)
                            return "backward"
                        if (key === Qt.Key_Left || key === Qt.Key_A
                                || key === Qt.Key_Q)
                            return "left"
                        if (key === Qt.Key_Right || key === Qt.Key_D)
                            return "right"
                        if (key === Qt.Key_E)
                            return "up"
                        if (key === Qt.Key_C)
                            return "down"
                        return ""
                    }

                    function handleCameraPresetKey(event, active) {
                        let preset = 0
                        if (event.key === Qt.Key_1)
                            preset = 1
                        else if (event.key === Qt.Key_2)
                            preset = 2
                        else if (event.key === Qt.Key_3)
                            preset = 3
                        else if (event.key === Qt.Key_7) {
                            event.accepted = true
                            if (active && !event.isAutoRepeat)
                                enableFreeCamera()
                            return true
                        } else {
                            return false
                        }
                        event.accepted = true
                        if (active && !event.isAutoRepeat
                                && window.viewer.runCount > 0
                                && window.viewer.carCameraAvailable) {
                            window.viewer.cameraPreset = preset
                            focusCurrentCar()
                        }
                        return true
                    }

                    function handleFreeCameraKey(event, active) {
                        if (!freeCamera)
                            return false
                        const direction = freeMovementForKey(event.key)
                        if (direction.length === 0)
                            return false
                        event.accepted = true
                        if (!event.isAutoRepeat)
                            setFreeMovement(direction, active)
                        return true
                    }

                    function stepFreeCameraMovement(timestamp) {
                        if (!freeCamera) {
                            releaseFreeMovement()
                            return false
                        }
                        if (!hasFreeMovement())
                            return false
                        const now = Number.isFinite(timestamp)
                                  ? timestamp : Date.now()
                        if (freeMoveStartedAt <= 0)
                            freeMoveStartedAt = now
                        if (freeMoveLastStepAt <= 0)
                            freeMoveLastStepAt = now
                        const elapsed = Math.max(
                            0, (now - freeMoveStartedAt) / 1000)
                        const delta = Math.max(
                            0,
                            Math.min(0.05,
                                     (now - freeMoveLastStepAt) / 1000))
                        freeMoveLastStepAt = now
                        freeMoveSpeed = Math.min(
                            freeMoveMaximumSpeed,
                            freeMoveInitialSpeed
                                + elapsed * freeMoveAcceleration)
                        if (delta <= 0)
                            return true

                        const yaw = orbitYaw * Math.PI / 180
                        const pitch = orbitPitch * Math.PI / 180
                        const pitchCos = Math.cos(pitch)
                        const forward = Qt.vector3d(
                            -Math.sin(yaw) * pitchCos,
                            Math.sin(pitch),
                            -Math.cos(yaw) * pitchCos)
                        const right = Qt.vector3d(
                            Math.cos(yaw), 0, -Math.sin(yaw))
                        let x = 0
                        let y = 0
                        let z = 0
                        if (freeMoveForward) {
                            x += forward.x
                            y += forward.y
                            z += forward.z
                        }
                        if (freeMoveBackward) {
                            x -= forward.x
                            y -= forward.y
                            z -= forward.z
                        }
                        if (freeMoveRight) {
                            x += right.x
                            z += right.z
                        }
                        if (freeMoveLeft) {
                            x -= right.x
                            z -= right.z
                        }
                        if (freeMoveUp)
                            y += 1
                        if (freeMoveDown)
                            y -= 1
                        const length = Math.sqrt(x * x + y * y + z * z)
                        if (length <= 0.000001)
                            return true
                        const distance = freeMoveSpeed * delta / length
                        freeCameraPosition = Qt.vector3d(
                            freeCameraPosition.x + x * distance,
                            freeCameraPosition.y + y * distance,
                            freeCameraPosition.z + z * distance)
                        return true
                    }

                    function zoomOrbit(wheelDeltaY) {
                        if (carCameraActive)
                            return false
                        leaveExactWhiteboardView()
                        const factor = Math.exp(-wheelDeltaY / 1200)
                        orbitDistance = Math.max(
                            3, Math.min(1000, orbitDistance * factor))
                        return true
                    }

                    function beginViewRotation() {
                        viewRotationTargetYaw = orbitYaw
                        viewRotationTargetPitch = orbitPitch
                        viewRotationSmoothing = false
                    }

                    function updateViewRotation(deltaX, deltaY) {
                        if (carCameraActive)
                            return
                        if (!freeCamera) {
                            orbitYaw -= deltaX
                            orbitPitch = Math.max(
                                -85, Math.min(85, orbitPitch - deltaY))
                            viewRotationTargetYaw = orbitYaw
                            viewRotationTargetPitch = orbitPitch
                            viewRotationSmoothing = false
                            return
                        }
                        viewRotationTargetYaw -= deltaX * 0.5
                        viewRotationTargetPitch = Math.max(
                            -85,
                            Math.min(85,
                                     viewRotationTargetPitch - deltaY * 0.5))
                        viewRotationSmoothing = true
                    }

                    function stepViewRotation(frameTime) {
                        if (!viewRotationSmoothing)
                            return false
                        const seconds = Math.max(
                            0,
                            Math.min(0.05,
                                     Number.isFinite(frameTime)
                                     ? frameTime : 1 / 60))
                        const blend = 1 - Math.exp(-24 * seconds)
                        orbitYaw += (viewRotationTargetYaw - orbitYaw) * blend
                        orbitPitch += (viewRotationTargetPitch - orbitPitch)
                            * blend
                        if (Math.abs(viewRotationTargetYaw - orbitYaw) < 0.001
                                && Math.abs(viewRotationTargetPitch
                                            - orbitPitch) < 0.001) {
                            orbitYaw = viewRotationTargetYaw
                            orbitPitch = viewRotationTargetPitch
                            viewRotationSmoothing = false
                        }
                        return true
                    }

                    function enableFreeCamera() {
                        window.viewer.releaseManualInputs()
                        orbitalCamera = false
                        const wasCarCamera = carCameraActive
                        const forward = cameraForward
                        const position = freeCamera
                            ? freeCameraPosition
                            : wasCarCamera
                              ? window.viewer.carCameraPosition
                              : Qt.vector3d(
                                    cameraTarget.x
                                        - forward.x * orbitDistance,
                                    cameraTarget.y
                                        - forward.y * orbitDistance,
                                    cameraTarget.z
                                        - forward.z * orbitDistance)
                        if (wasCarCamera) {
                            const dx = window.viewer.carCameraTarget.x
                                     - position.x
                            const dy = window.viewer.carCameraTarget.y
                                     - position.y
                            const dz = window.viewer.carCameraTarget.z
                                     - position.z
                            orbitDistance = Math.max(
                                0.1, Math.sqrt(dx * dx + dy * dy + dz * dz))
                            orbitPitch = Math.asin(
                                Math.max(-1, Math.min(1, forward.y)))
                                * 180 / Math.PI
                            orbitYaw = Math.atan2(-forward.x, -forward.z)
                                * 180 / Math.PI
                        }
                        beginViewRotation()
                        freeCameraPosition = Qt.vector3d(
                            position.x, position.y, position.z)
                        exactWhiteboardBoardIndex = -1
                        exactWhiteboardBoardId = ""
                        cameraFieldOfView = 55
                        freeCamera = true
                        cuboidFocused = false
                    }

                    function leaveExactWhiteboardView() {
                        if (exactWhiteboardBoardIndex < 0)
                            return
                        exactWhiteboardBoardIndex = -1
                        exactWhiteboardBoardId = ""
                        cameraFieldOfView = 55
                    }

                    function enableOrbitalCamera() {
                        window.viewer.releaseManualInputs()
                        releaseFreeMovement()
                        beginViewRotation()
                        exactWhiteboardBoardIndex = -1
                        exactWhiteboardBoardId = ""
                        cameraFieldOfView = 55
                        freeCamera = false
                        cuboidFocused = false
                        orbitalCamera = true
                    }

                    function focusCurrentCar() {
                        releaseFreeMovement()
                        beginViewRotation()
                        exactWhiteboardBoardIndex = -1
                        exactWhiteboardBoardId = ""
                        cameraFieldOfView = 55
                        freeCamera = false
                        cuboidFocused = false
                        orbitalCamera = false
                    }

                    function resetCameraFocus() {
                        focusCurrentCar()
                        hasObjectFocus = false
                        lastFocusedWhiteboardIndex = -1
                        lastFocusedWhiteboardId = ""
                        cuboidFocusCenter = Qt.vector3d(0, 0, 0)
                    }

                    function whiteboardIndexForId(id) {
                        if (id.length === 0)
                            return -1
                        const boards = window.viewer.whiteboard.boards
                        for (let index = 0; index < boards.length; ++index) {
                            if (boards[index].id === id)
                                return index
                        }
                        return -1
                    }

                    function synchronizeWhiteboardFocus() {
                        if (lastFocusedWhiteboardId.length === 0)
                            return
                        const index = whiteboardIndexForId(
                            lastFocusedWhiteboardId)
                        const board = index >= 0
                            ? window.viewer.whiteboard.boards[index] : null
                        if (!board || !board.isCurrentMap) {
                            if (exactWhiteboardBoardId ===
                                    lastFocusedWhiteboardId) {
                                enableFreeCamera()
                            }
                            lastFocusedWhiteboardIndex = -1
                            lastFocusedWhiteboardId = ""
                            hasObjectFocus = false
                            return
                        }
                        lastFocusedWhiteboardIndex = index
                        if (exactWhiteboardBoardId ===
                                lastFocusedWhiteboardId) {
                            exactWhiteboardBoardIndex = index
                        }
                    }

                    function focusLastObject() {
                        if (!hasObjectFocus)
                            return
                        const whiteboardIndex = whiteboardIndexForId(
                            lastFocusedWhiteboardId)
                        if (whiteboardIndex >= 0) {
                            const board = window.viewer.whiteboard.boards[
                                whiteboardIndex]
                            if (board && board.isCurrentMap) {
                                restoreWhiteboardView(board)
                                return
                            }
                        }
                        releaseFreeMovement()
                        beginViewRotation()
                        exactWhiteboardBoardIndex = -1
                        exactWhiteboardBoardId = ""
                        cameraFieldOfView = 55
                        freeCamera = false
                        cuboidFocused = true
                        orbitalCamera = false
                    }

                    function focusCuboid(center, size) {
                        releaseFreeMovement()
                        beginViewRotation()
                        exactWhiteboardBoardIndex = -1
                        exactWhiteboardBoardId = ""
                        lastFocusedWhiteboardIndex = -1
                        lastFocusedWhiteboardId = ""
                        cameraFieldOfView = 55
                        cuboidFocusCenter = center
                        hasObjectFocus = true
                        freeCamera = false
                        cuboidFocused = true
                        orbitalCamera = false
                        orbitDistance = Math.max(
                            3,
                            Math.min(1000,
                                     Math.max(size.x, size.y, size.z) * 2.4))
                    }

                    function captureWhiteboardView() {
                        const camera = viewCamera.scenePosition
                        const target = cameraTarget
                        const viewportWidth = Math.max(1, width)
                        const viewportHeight = Math.max(1, height)
                        const contentX = 0
                        const contentY = Math.min(
                            1, whiteboardOverlay.boardTop / viewportHeight)
                        const contentWidth = 1
                        const contentHeight = Math.max(
                            0.000001, 1 - contentY)
                        const planeDistance = Math.max(
                            viewCamera.clipNear * 2,
                            Math.min(
                                Math.max(0.05, orbitDistance - 0.01),
                                Math.max(0.05, orbitDistance * 0.32)))
                        const fullHeight = 2 * Math.tan(
                            cameraFieldOfView * Math.PI / 360)
                            * planeDistance
                        const fullWidth = fullHeight
                            * viewportWidth / viewportHeight
                        const planeWidth = fullWidth * contentWidth
                        const planeHeight = fullHeight * contentHeight
                        const contentCenterX =
                            contentX + contentWidth * 0.5
                        const contentCenterY =
                            contentY + contentHeight * 0.5
                        const rightOffset =
                            fullWidth * (contentCenterX - 0.5)
                        const upOffset =
                            fullHeight * (0.5 - contentCenterY)
                        const yaw = orbitYaw * Math.PI / 180
                        const pitch = orbitPitch * Math.PI / 180
                        const right = Qt.vector3d(
                            Math.cos(yaw), 0, -Math.sin(yaw))
                        const up = Qt.vector3d(
                            Math.sin(yaw) * Math.sin(pitch),
                            Math.cos(pitch),
                            Math.cos(yaw) * Math.sin(pitch))
                        return {
                            "projection": "perspective-vertical",
                            "fieldOfView": cameraFieldOfView,
                            "planeDistance": planeDistance,
                            "viewportWidth": viewportWidth,
                            "viewportHeight": viewportHeight,
                            "contentX": contentX,
                            "contentY": contentY,
                            "contentWidth": contentWidth,
                            "contentHeight": contentHeight,
                            "canvasWidth": Math.max(
                                1, whiteboardOverlay.width),
                            "canvasHeight": Math.max(
                                1,
                                whiteboardOverlay.height
                                - whiteboardOverlay.boardTop),
                            "targetX": target.x,
                            "targetY": target.y,
                            "targetZ": target.z,
                            "yaw": orbitYaw,
                            "pitch": orbitPitch,
                            "distance": orbitDistance,
                            "planeX": camera.x
                                      + cameraForward.x * planeDistance
                                      + right.x * rightOffset
                                      + up.x * upOffset,
                            "planeY": camera.y
                                      + cameraForward.y * planeDistance
                                      + right.y * rightOffset
                                      + up.y * upOffset,
                            "planeZ": camera.z
                                      + cameraForward.z * planeDistance
                                      + right.z * rightOffset
                                      + up.z * upOffset,
                            "planeWidth": planeWidth,
                            "planeHeight": planeHeight
                        }
                    }

                    function restoreWhiteboardView(board) {
                        releaseFreeMovement()
                        cuboidFocusCenter = Qt.vector3d(
                            board.targetX,
                            board.targetY,
                            board.targetZ)
                        hasObjectFocus = true
                        freeCamera = false
                        cuboidFocused = true
                        orbitalCamera = false
                        orbitYaw = board.yaw
                        orbitPitch = board.pitch
                        beginViewRotation()
                        orbitDistance = board.distance
                        cameraFieldOfView =
                            board.projectionVersion >= 1
                            ? board.fieldOfView : 55
                        exactWhiteboardBoardIndex =
                            board.projectionVersion >= 1
                            ? board.boardIndex : -1
                        exactWhiteboardBoardId =
                            board.projectionVersion >= 1
                            ? board.id : ""
                        lastFocusedWhiteboardIndex = board.boardIndex
                        lastFocusedWhiteboardId = board.id
                    }

                    function whiteboardPlaneFocusEnabled() {
                        return !window.viewer.whiteboard.active
                            && !freeCamera
                    }

                    Timer {
                        id: freeCameraMovementTimer
                        interval: 16
                        repeat: true
                        onTriggered:
                            viewport.stepFreeCameraMovement(Date.now())
                    }

                    FrameAnimation {
                        running: viewport.viewRotationSmoothing
                                 && window.visible
                        onTriggered:
                            viewport.stepViewRotation(frameTime)
                    }

                    function finishWhiteboardImageExport(success) {
                        whiteboardExportTimer.stop()
                        whiteboardExportWatchdog.stop()
                        whiteboardPlaneView.forcedBoardIndex = -1
                        whiteboardPlaneView.exportMode = false
                        exportingWhiteboardImage = false
                        whiteboardImageExportUrl = ""
                        window.viewer.whiteboard.finishBoardImageExport(
                                    success, true)
                    }

                    function exportWhiteboardBackground(index, fileUrl) {
                        if (exportingWhiteboardImage
                                || index < 0
                                || index >= window.viewer.whiteboard.boardCount) {
                            return false
                        }
                        const board = window.viewer.whiteboard.boards[index]
                        const path =
                                window.viewer.whiteboard.imageExportPath(fileUrl)
                        if (!board || !board.isCurrentMap
                                || path.length === 0
                                || !window.viewer.whiteboard.selectBoard(index)) {
                            return false
                        }
                        restoreWhiteboardView(board)
                        whiteboardImageExportUrl = fileUrl
                        whiteboardPlaneView.forcedBoardIndex = index
                        whiteboardPlaneView.exportMode = true
                        exportingWhiteboardImage = true
                        whiteboardExportTimer.restart()
                        return true
                    }

                    Timer {
                        id: whiteboardExportTimer
                        interval: 120
                        repeat: false
                        onTriggered: {
                            const path =
                                    window.viewer.whiteboard.imageExportPath(
                                        viewport.whiteboardImageExportUrl)
                            if (path.length === 0) {
                                viewport.finishWhiteboardImageExport(false)
                                return
                            }
                            const started = viewport.grabToImage(
                                function(result) {
                                    const saved = result !== null
                                            && window.viewer.whiteboard
                                                   .saveBoardBackgroundImage(
                                                       result.image,
                                                       viewport
                                                           .whiteboardImageExportUrl)
                                    viewport.finishWhiteboardImageExport(saved)
                                })
                            if (started) {
                                whiteboardExportWatchdog.restart()
                            } else {
                                viewport.finishWhiteboardImageExport(false)
                            }
                        }
                    }

                    Timer {
                        id: whiteboardExportWatchdog
                        interval: 10000
                        repeat: false
                        onTriggered:
                            viewport.finishWhiteboardImageExport(false)
                    }

                    function beginCuboidInteraction(kind, axis, x, y) {
                        if (window.controller.running)
                            return false
                        cuboidDragKind = kind
                        cuboidDragAxis = axis
                        cuboidDragX = x
                        cuboidDragY = y
                        cuboidDragActive = kind === "move"
                                           || kind === "resize"
                        return cuboidDragActive
                    }

                    function updateCuboidInteraction(x, y) {
                        if (!cuboidDragActive)
                            return
                        const dx = x - cuboidDragX
                        const dy = y - cuboidDragY
                        const scale = orbitDistance
                                      / Math.max(200, Math.min(width, height))
                        const yaw = orbitYaw * Math.PI / 180
                        const pitch = orbitPitch * Math.PI / 180
                        let amount = 0
                        if (cuboidDragAxis === "x") {
                            amount = (dx * Math.cos(yaw)
                                      - dy * Math.sin(yaw)
                                        * Math.sin(pitch)) * scale
                        } else if (cuboidDragAxis === "y") {
                            amount = -dy * Math.cos(pitch) * scale
                        } else {
                            amount = (-dx * Math.sin(yaw)
                                      - dy * Math.cos(yaw)
                                        * Math.sin(pitch)) * scale
                        }
                        if (cuboidDragKind === "resize") {
                            window.controller.cuboidTargets.resizeSelected(
                                cuboidDragAxis, amount)
                        } else if (cuboidDragAxis === "x") {
                            window.controller.cuboidTargets.translateSelected(
                                amount, 0, 0)
                        } else if (cuboidDragAxis === "y") {
                            window.controller.cuboidTargets.translateSelected(
                                0, amount, 0)
                        } else {
                            window.controller.cuboidTargets.translateSelected(
                                0, 0, amount)
                        }
                        cuboidDragX = x
                        cuboidDragY = y
                    }

                    function endCuboidInteraction() {
                        cuboidDragActive = false
                        cuboidDragKind = ""
                        cuboidDragAxis = ""
                    }

                    function customPlanePoint(x, y) {
                        const target =
                            window.controller.customVolumeTargets.selectedTarget
                        const view = window.rayTracingEnabled
                                   ? rayTracingTrajectoryOverlay
                                   : rasterMapView
                        const nearPoint = view.mapTo3DScene(
                            Qt.vector3d(x, y, 1))
                        const farPoint = view.mapTo3DScene(
                            Qt.vector3d(x, y, 2))
                        const direction = farPoint.minus(nearPoint)
                        let numerator = 0
                        let denominator = 0
                        if (target.plane === "xy") {
                            numerator = target.origin.z - nearPoint.z
                            denominator = direction.z
                        } else if (target.plane === "yz") {
                            numerator = target.origin.x - nearPoint.x
                            denominator = direction.x
                        } else {
                            numerator = target.origin.y - nearPoint.y
                            denominator = direction.y
                        }
                        if (Math.abs(denominator) < 0.000001)
                            return target.origin
                        const amount = numerator / denominator
                        return nearPoint.plus(direction.times(amount))
                    }

                    function beginCustomInteraction(kind, index, x, y) {
                        if (window.controller.running
                            || window.controller.customVolumeDrawing)
                            return false
                        cuboidDragX = x
                        cuboidDragY = y
                        customVertexDragIndex = index
                        customVertexDragActive = kind === "custom-vertex"
                        customDepthDragActive = kind === "custom-depth"
                        return customVertexDragActive
                               || customDepthDragActive
                    }

                    function updateCustomInteraction(x, y) {
                        if (customVertexDragActive) {
                            const point = customPlanePoint(x, y)
                            window.controller.customVolumeTargets.setVertexWorld(
                                customVertexDragIndex,
                                point.x,
                                point.y,
                                point.z)
                        } else if (customDepthDragActive) {
                            const target =
                                window.controller.customVolumeTargets.selectedTarget
                            const dx = x - cuboidDragX
                            const dy = y - cuboidDragY
                            const scale = orbitDistance
                                          / Math.max(200,
                                                     Math.min(width, height))
                            const yaw = orbitYaw * Math.PI / 180
                            const pitch = orbitPitch * Math.PI / 180
                            let amount = 0
                            if (target.plane === "xy") {
                                amount = (-dx * Math.sin(yaw)
                                          - dy * Math.cos(yaw)
                                            * Math.sin(pitch)) * scale
                            } else if (target.plane === "yz") {
                                amount = (dx * Math.cos(yaw)
                                          - dy * Math.sin(yaw)
                                            * Math.sin(pitch)) * scale
                            } else {
                                amount = -dy * Math.cos(pitch) * scale
                            }
                            window.controller.customVolumeTargets
                                  .resizeDepthSelected(amount)
                        }
                        cuboidDragX = x
                        cuboidDragY = y
                    }

                    function endCustomInteraction() {
                        customVertexDragActive = false
                        customDepthDragActive = false
                        customVertexDragIndex = -1
                    }

                    function beginPoseInteraction(kind, axis, x, y) {
                        if (window.controller.running)
                            return false
                        poseDragKind = kind
                        poseDragAxis = axis
                        cuboidDragX = x
                        cuboidDragY = y
                        poseDragActive = kind === "pose-move"
                                         || kind === "pose-rotate"
                        return poseDragActive
                    }

                    function updatePoseInteraction(x, y) {
                        if (!poseDragActive)
                            return
                        const dx = x - cuboidDragX
                        const dy = y - cuboidDragY
                        if (poseDragKind === "pose-rotate") {
                            let degrees = 0
                            if (poseDragAxis === "yaw")
                                degrees = dx * 0.5
                            else if (poseDragAxis === "pitch")
                                degrees = -dy * 0.5
                            else
                                degrees = (dx - dy) * 0.35
                            window.controller.poseTargets.rotateSelected(
                                poseDragAxis, degrees)
                        } else {
                            const scale = orbitDistance
                                          / Math.max(
                                              200, Math.min(width, height))
                            const yaw = orbitYaw * Math.PI / 180
                            const pitch = orbitPitch * Math.PI / 180
                            let amount = 0
                            if (poseDragAxis === "x") {
                                amount = (dx * Math.cos(yaw)
                                          - dy * Math.sin(yaw)
                                            * Math.sin(pitch)) * scale
                                window.controller.poseTargets
                                      .translateSelected(amount, 0, 0)
                            } else if (poseDragAxis === "y") {
                                amount = -dy * Math.cos(pitch) * scale
                                window.controller.poseTargets
                                      .translateSelected(0, amount, 0)
                            } else {
                                amount = (-dx * Math.sin(yaw)
                                          - dy * Math.cos(yaw)
                                            * Math.sin(pitch)) * scale
                                window.controller.poseTargets
                                      .translateSelected(0, 0, amount)
                            }
                        }
                        cuboidDragX = x
                        cuboidDragY = y
                    }

                    function endPoseInteraction() {
                        poseDragActive = false
                        poseDragKind = ""
                        poseDragAxis = ""
                    }

                    component CuboidEditorScene: Node {
                        id: cuboidScene
                        property bool interactive: false

                        Repeater3D {
                            model: window.controller.cuboidTargets

                            delegate: Node {
                                id: cuboidRoot

                                required property int index
                                required property vector3d targetCenter
                                required property vector3d targetSize
                                required property bool targetSelected
                                readonly property int targetIndex: index
                                readonly property bool targetActive:
                                    targetSelected
                                    && window.controller.evaluationTargetId
                                       === "volume-entry-time"
                                position: targetCenter

                                Model {
                                    objectName: "cuboidTargetModel"
                                    property int targetIndex:
                                        cuboidRoot.targetIndex
                                    property string editorKind: "select"
                                    property string editorAxis: ""
                                    source: "#Cube"
                                    scale: Qt.vector3d(
                                        cuboidRoot.targetSize.x / 100,
                                        cuboidRoot.targetSize.y / 100,
                                        cuboidRoot.targetSize.z / 100)
                                    pickable: cuboidScene.interactive
                                    castsShadows: false
                                    receivesShadows: false
                                    materials: DefaultMaterial {
                                        lighting: DefaultMaterial.NoLighting
                                        diffuseColor:
                                            cuboidRoot.targetActive
                                            ? "#35d978" : "#55a7d8"
                                        opacity:
                                            cuboidRoot.targetActive
                                            ? 0.27 : 0.13
                                        cullMode: Material.NoCulling
                                    }
                                }

                                Node {
                                    visible: cuboidRoot.targetActive
                                             && !window.controller.running
                                    readonly property real barLength:
                                        Math.max(1.6,
                                                 viewport.orbitDistance * 0.055)
                                    readonly property real thickness:
                                        Math.max(0.12,
                                                 viewport.orbitDistance * 0.005)
                                    readonly property real handleSize:
                                        Math.max(0.34,
                                                 viewport.orbitDistance * 0.014)

                                    Model {
                                        property int targetIndex:
                                            cuboidRoot.targetIndex
                                        property string editorKind: "move"
                                        property string editorAxis: "x"
                                        source: "#Cube"
                                        x: cuboidRoot.targetSize.x / 2
                                           + parent.barLength / 2
                                        scale: Qt.vector3d(
                                            parent.barLength / 100,
                                            parent.thickness / 100,
                                            parent.thickness / 100)
                                        pickable: cuboidScene.interactive
                                        materials: DefaultMaterial {
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            diffuseColor: "#e55353"
                                        }
                                    }
                                    Model {
                                        property int targetIndex:
                                            cuboidRoot.targetIndex
                                        property string editorKind: "move"
                                        property string editorAxis: "y"
                                        source: "#Cube"
                                        y: cuboidRoot.targetSize.y / 2
                                           + parent.barLength / 2
                                        scale: Qt.vector3d(
                                            parent.thickness / 100,
                                            parent.barLength / 100,
                                            parent.thickness / 100)
                                        pickable: cuboidScene.interactive
                                        materials: DefaultMaterial {
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            diffuseColor: "#62c96b"
                                        }
                                    }
                                    Model {
                                        property int targetIndex:
                                            cuboidRoot.targetIndex
                                        property string editorKind: "move"
                                        property string editorAxis: "z"
                                        source: "#Cube"
                                        z: cuboidRoot.targetSize.z / 2
                                           + parent.barLength / 2
                                        scale: Qt.vector3d(
                                            parent.thickness / 100,
                                            parent.thickness / 100,
                                            parent.barLength / 100)
                                        pickable: cuboidScene.interactive
                                        materials: DefaultMaterial {
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            diffuseColor: "#4c86e8"
                                        }
                                    }

                                    Model {
                                        objectName: "cuboidResizeHandleX"
                                        property int targetIndex:
                                            cuboidRoot.targetIndex
                                        property string editorKind: "resize"
                                        property string editorAxis: "x"
                                        source: "#Cube"
                                        x: cuboidRoot.targetSize.x / 2
                                           + parent.barLength
                                        scale: Qt.vector3d(
                                            parent.handleSize / 100,
                                            parent.handleSize / 100,
                                            parent.handleSize / 100)
                                        pickable: cuboidScene.interactive
                                        materials: DefaultMaterial {
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            diffuseColor: "#ff7770"
                                        }
                                    }
                                    Model {
                                        objectName: "cuboidResizeHandleY"
                                        property int targetIndex:
                                            cuboidRoot.targetIndex
                                        property string editorKind: "resize"
                                        property string editorAxis: "y"
                                        source: "#Cube"
                                        y: cuboidRoot.targetSize.y / 2
                                           + parent.barLength
                                        scale: Qt.vector3d(
                                            parent.handleSize / 100,
                                            parent.handleSize / 100,
                                            parent.handleSize / 100)
                                        pickable: cuboidScene.interactive
                                        materials: DefaultMaterial {
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            diffuseColor: "#87e58e"
                                        }
                                    }
                                    Model {
                                        objectName: "cuboidResizeHandleZ"
                                        property int targetIndex:
                                            cuboidRoot.targetIndex
                                        property string editorKind: "resize"
                                        property string editorAxis: "z"
                                        source: "#Cube"
                                        z: cuboidRoot.targetSize.z / 2
                                           + parent.barLength
                                        scale: Qt.vector3d(
                                            parent.handleSize / 100,
                                            parent.handleSize / 100,
                                            parent.handleSize / 100)
                                        pickable: cuboidScene.interactive
                                        materials: DefaultMaterial {
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            diffuseColor: "#78a6ff"
                                        }
                                    }
                                }
                            }
                        }
                    }

                    component CustomVolumeEditorScene: Node {
                        id: customVolumeScene
                        property bool interactive: false

                        Repeater3D {
                            model:
                                window.controller.customVolumeTargets.targets

                            delegate: Node {
                                id: customVolumeRoot

                                required property int index
                                required property var modelData
                                readonly property int targetIndex: index
                                readonly property bool targetSelected:
                                    modelData.selected
                                readonly property bool targetActive:
                                    targetSelected
                                    && window.controller.evaluationTargetId
                                       === "custom-volume-entry-time"

                                Model {
                                    objectName: "customVolumeTargetModel"
                                    property int targetIndex:
                                        customVolumeRoot.targetIndex
                                    property string editorKind:
                                        "custom-select"
                                    property int vertexIndex: -1
                                    visible: window.viewer.loaded
                                    geometry: customVolumeRoot.modelData.geometry
                                    pickable: customVolumeScene.interactive
                                    castsShadows: false
                                    receivesShadows: false
                                    materials: DefaultMaterial {
                                        lighting: DefaultMaterial.NoLighting
                                        diffuseColor:
                                            customVolumeRoot.targetActive
                                            ? "#f2aa45" : "#ca7ccc"
                                        opacity:
                                            customVolumeRoot.targetActive
                                            ? 0.3 : 0.14
                                        cullMode: Material.NoCulling
                                    }
                                }

                                Model {
                                    objectName: "customVolumeDrawingPlane"
                                    visible:
                                        window.viewer.loaded
                                        && customVolumeRoot.targetActive
                                        && window.controller.customVolumeDrawing
                                    position: customVolumeRoot.modelData.origin
                                    source: "#Cube"
                                    scale:
                                        customVolumeRoot.modelData.plane === "xy"
                                        ? Qt.vector3d(0.5, 0.5, 0.002)
                                        : customVolumeRoot.modelData.plane
                                          === "yz"
                                          ? Qt.vector3d(0.002, 0.5, 0.5)
                                          : Qt.vector3d(0.5, 0.002, 0.5)
                                    materials: DefaultMaterial {
                                        lighting: DefaultMaterial.NoLighting
                                        diffuseColor: "#e6b75d"
                                        opacity: 0.1
                                        cullMode: Material.NoCulling
                                    }
                                }

                                Node {
                                    visible:
                                        window.viewer.loaded
                                        && customVolumeRoot.targetSelected
                                        && window.controller.evaluationTargetId
                                           === "custom-volume-entry-time"
                                        && !window.controller
                                                  .customVolumeDrawing

                                    Model {
                                        objectName:
                                            "customVolumePlaneChoiceXY"
                                        property int targetIndex:
                                            customVolumeRoot.targetIndex
                                        property string editorKind:
                                            "custom-plane"
                                        property string planeChoice: "xy"
                                        property int vertexIndex: -1
                                        position:
                                            customVolumeRoot.modelData.origin
                                            .plus(Qt.vector3d(-6, 3, 0))
                                        source: "#Cube"
                                        scale: Qt.vector3d(
                                            0.04, 0.04, 0.002)
                                        pickable:
                                            customVolumeScene.interactive
                                        materials: DefaultMaterial {
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            diffuseColor: "#e36d6d"
                                            opacity: 0.22
                                        }
                                    }
                                    Model {
                                        objectName:
                                            "customVolumePlaneChoiceXZ"
                                        property int targetIndex:
                                            customVolumeRoot.targetIndex
                                        property string editorKind:
                                            "custom-plane"
                                        property string planeChoice: "xz"
                                        property int vertexIndex: -1
                                        position:
                                            customVolumeRoot.modelData.origin
                                            .plus(Qt.vector3d(0, 0, 0))
                                        source: "#Cube"
                                        scale: Qt.vector3d(
                                            0.04, 0.002, 0.04)
                                        pickable:
                                            customVolumeScene.interactive
                                        materials: DefaultMaterial {
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            diffuseColor: "#6ed47b"
                                            opacity: 0.22
                                        }
                                    }
                                    Model {
                                        objectName:
                                            "customVolumePlaneChoiceYZ"
                                        property int targetIndex:
                                            customVolumeRoot.targetIndex
                                        property string editorKind:
                                            "custom-plane"
                                        property string planeChoice: "yz"
                                        property int vertexIndex: -1
                                        position:
                                            customVolumeRoot.modelData.origin
                                            .plus(Qt.vector3d(6, 3, 0))
                                        source: "#Cube"
                                        scale: Qt.vector3d(
                                            0.002, 0.04, 0.04)
                                        pickable:
                                            customVolumeScene.interactive
                                        materials: DefaultMaterial {
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            diffuseColor: "#669cf0"
                                            opacity: 0.22
                                        }
                                    }
                                }

                                Repeater3D {
                                    model:
                                        customVolumeRoot.modelData.vertices

                                    delegate: Model {
                                        required property var modelData
                                        property int targetIndex:
                                            customVolumeRoot.targetIndex
                                        property string editorKind:
                                            "custom-vertex"
                                        property int vertexIndex:
                                            modelData.index
                                        visible:
                                            window.viewer.loaded
                                            && customVolumeRoot.targetActive
                                        position: modelData.world
                                        source: "#Sphere"
                                        scale: Qt.vector3d(
                                            Math.max(
                                                0.004,
                                                viewport.orbitDistance
                                                * 0.00016),
                                            Math.max(
                                                0.004,
                                                viewport.orbitDistance
                                                * 0.00016),
                                            Math.max(
                                                0.004,
                                                viewport.orbitDistance
                                                * 0.00016))
                                        pickable:
                                            customVolumeScene.interactive
                                        materials: DefaultMaterial {
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            diffuseColor: "#ffd06a"
                                        }
                                    }
                                }

                                Model {
                                    objectName: "customVolumeDepthHandle"
                                    property int targetIndex:
                                        customVolumeRoot.targetIndex
                                    property string editorKind: "custom-depth"
                                    property int vertexIndex: -1
                                    visible:
                                        window.viewer.loaded
                                        && customVolumeRoot.targetActive
                                        && !window.controller.customVolumeDrawing
                                    position:
                                        customVolumeRoot.modelData.depthHandle
                                    source: "#Cube"
                                    scale: Qt.vector3d(
                                        Math.max(
                                            0.005,
                                            viewport.orbitDistance
                                            * 0.0002),
                                        Math.max(
                                            0.005,
                                            viewport.orbitDistance
                                            * 0.0002),
                                        Math.max(
                                            0.005,
                                            viewport.orbitDistance
                                            * 0.0002))
                                    pickable: customVolumeScene.interactive
                                    materials: DefaultMaterial {
                                        lighting: DefaultMaterial.NoLighting
                                        diffuseColor: "#fff0a6"
                                    }
                                }
                            }
                        }
                    }

                    component PoseTargetEditorScene: Node {
                        id: poseScene
                        property bool interactive: false

                        Repeater3D {
                            model: window.controller.poseTargets.targets

                            delegate: Node {
                                id: poseRoot

                                required property int index
                                required property var modelData
                                readonly property int targetIndex: index
                                readonly property bool targetActive:
                                    modelData.selected
                                    && window.controller.evaluationTargetId
                                       === "pose-target"
                                position: modelData.position
                                visible: window.viewer.loaded

                                Node {
                                    rotation: poseRoot.modelData.rotation

                                    Repeater3D {
                                        model: window.viewer.carEllipsoids.length

                                        delegate: Node {
                                            required property int index
                                            readonly property var ellipsoid:
                                                window.viewer.carEllipsoids[index]

                                            position: ellipsoid.position
                                            rotation: ellipsoid.rotation
                                            scale: ellipsoid.radii

                                            Model {
                                                objectName:
                                                    "poseTargetCarModel"
                                                property int targetIndex:
                                                    poseRoot.targetIndex
                                                property string editorKind:
                                                    "pose-select"
                                                property string editorAxis: ""
                                                geometry:
                                                    window.viewer
                                                          .ellipsoidFilledGeometry
                                                pickable:
                                                    poseScene.interactive
                                                castsShadows: false
                                                receivesShadows: false
                                                materials: DefaultMaterial {
                                                    lighting:
                                                        DefaultMaterial
                                                        .NoLighting
                                                    diffuseColor:
                                                        poseRoot.targetActive
                                                        ? "#f2aa45"
                                                        : "#65a7d8"
                                                    opacity:
                                                        poseRoot.targetActive
                                                        ? 0.82 : 0.36
                                                    cullMode:
                                                        Material.NoCulling
                                                }
                                            }
                                        }
                                    }
                                }

                                Node {
                                    id: poseHandleRoot

                                    visible: poseRoot.targetActive
                                             && !window.controller.running
                                    readonly property real barLength:
                                        Math.max(
                                            1.8,
                                            viewport.orbitDistance * 0.05)
                                    readonly property real thickness:
                                        Math.max(
                                            0.12,
                                            viewport.orbitDistance * 0.004)
                                    readonly property real rotationHandleSize:
                                        Math.max(
                                            0.5,
                                            viewport.orbitDistance * 0.018)

                                    Repeater3D {
                                        model: [
                                            {
                                                "axis": "x",
                                                "position":
                                                    Qt.vector3d(
                                                        1.6
                                                        + poseHandleRoot
                                                              .barLength
                                                          / 2,
                                                        0,
                                                        0),
                                                "scale": Qt.vector3d(
                                                    poseHandleRoot.barLength
                                                    / 100,
                                                    poseHandleRoot.thickness
                                                    / 100,
                                                    poseHandleRoot.thickness
                                                    / 100),
                                                "color": "#e36d6d"
                                            },
                                            {
                                                "axis": "y",
                                                "position":
                                                    Qt.vector3d(
                                                        0,
                                                        1.1
                                                        + poseHandleRoot
                                                              .barLength
                                                          / 2,
                                                        0),
                                                "scale": Qt.vector3d(
                                                    poseHandleRoot.thickness
                                                    / 100,
                                                    poseHandleRoot.barLength
                                                    / 100,
                                                    poseHandleRoot.thickness
                                                    / 100),
                                                "color": "#6ed47b"
                                            },
                                            {
                                                "axis": "z",
                                                "position":
                                                    Qt.vector3d(
                                                        0,
                                                        0,
                                                        3.2
                                                        + poseHandleRoot
                                                              .barLength
                                                          / 2),
                                                "scale": Qt.vector3d(
                                                    poseHandleRoot.thickness
                                                    / 100,
                                                    poseHandleRoot.thickness
                                                    / 100,
                                                    poseHandleRoot.barLength
                                                    / 100),
                                                "color": "#669cf0"
                                            }
                                        ]

                                        delegate: Model {
                                            required property var modelData
                                            objectName:
                                                "poseTargetMoveHandle"
                                            property int targetIndex:
                                                poseRoot.targetIndex
                                            property string editorKind:
                                                "pose-move"
                                            property string editorAxis:
                                                modelData.axis
                                            position: modelData.position
                                            scale: modelData.scale
                                            source: "#Cube"
                                            pickable:
                                                poseScene.interactive
                                            materials: DefaultMaterial {
                                                lighting:
                                                    DefaultMaterial.NoLighting
                                                diffuseColor: modelData.color
                                            }
                                        }
                                    }

                                    Repeater3D {
                                        model: [
                                            {
                                                "axis": "roll",
                                                "position":
                                                    Qt.vector3d(-2.3, 0, 0),
                                                "color": "#ffadad"
                                            },
                                            {
                                                "axis": "pitch",
                                                "position":
                                                    Qt.vector3d(0, -1.8, 0),
                                                "color": "#a9efb2"
                                            },
                                            {
                                                "axis": "yaw",
                                                "position":
                                                    Qt.vector3d(0, 0, -3.9),
                                                "color": "#a8c7ff"
                                            }
                                        ]

                                        delegate: Model {
                                            required property var modelData
                                            objectName:
                                                "poseTargetRotationHandle"
                                            property int targetIndex:
                                                poseRoot.targetIndex
                                            property string editorKind:
                                                "pose-rotate"
                                            property string editorAxis:
                                                modelData.axis
                                            position: modelData.position
                                            scale: Qt.vector3d(
                                                poseHandleRoot
                                                      .rotationHandleSize
                                                / 100,
                                                poseHandleRoot
                                                      .rotationHandleSize
                                                / 100,
                                                poseHandleRoot
                                                      .rotationHandleSize
                                                / 100)
                                            source: "#Sphere"
                                            pickable:
                                                poseScene.interactive
                                            materials: DefaultMaterial {
                                                lighting:
                                                    DefaultMaterial.NoLighting
                                                diffuseColor: modelData.color
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    View3D {
                        id: rasterMapView
                        objectName: "rasterMapView"
                        anchors.fill: parent
                        visible: !window.rayTracingEnabled
                        camera: viewCamera

                        environment: SceneEnvironment {
                            objectName: "mapEnvironment"
                            backgroundMode: SceneEnvironment.SkyBox
                            antialiasingMode: SceneEnvironment.MSAA
                            antialiasingQuality: SceneEnvironment.Medium
                            tonemapMode: SceneEnvironment.TonemapModeAces
                            probeExposure: 1.0
                            skyboxBlurAmount: 0.0
                            specularAAEnabled: true

                            lightProbe: Texture {
                                objectName: "daySkyTexture"
                                source: "qrc:/environment/day_sky.png"
                                mappingMode: Texture.LightProbe
                                generateMipmaps: true
                            }
                        }

                        Node {
                            position: viewport.carCameraActive
                                      ? window.viewer.carCameraPosition
                                      : viewport.freeCamera
                                        ? viewport.freeCameraPosition
                                        : viewport.cameraTarget
                            rotation: viewport.carCameraActive
                                      ? window.viewer.carCameraRotation
                                      : Qt.quaternion(1, 0, 0, 0)

                            Node {
                                eulerRotation.x: viewport.carCameraActive
                                                 ? 0 : viewport.orbitPitch
                                eulerRotation.y: viewport.carCameraActive
                                                 ? 0 : viewport.orbitYaw

                                PerspectiveCamera {
                                    id: viewCamera
                                    objectName: "viewCamera"
                                    readonly property real clipDistance:
                                        viewport.carCameraActive
                                        ? window.viewer.carCameraPosition
                                                .minus(window.viewer
                                                       .carCameraTarget)
                                                .length()
                                        : viewport.orbitDistance
                                    readonly property var dynamicClipPlanes:
                                        window.viewer.cameraClipPlanes(
                                            scenePosition, clipDistance)

                                    z: viewport.carCameraActive
                                       || viewport.freeCamera
                                       ? 0 : viewport.orbitDistance
                                    clipNear: dynamicClipPlanes.x
                                    clipFar: dynamicClipPlanes.y
                                    fieldOfView: viewport.carCameraActive
                                        ? window.viewer.carCameraFieldOfView
                                        : viewport.cameraFieldOfView
                                    fieldOfViewOrientation:
                                        PerspectiveCamera.Vertical
                                }
                            }
                        }

                        DirectionalLight {
                            objectName: "mainMapLight"
                            eulerRotation.x: -52
                            eulerRotation.y: -32
                            brightness: 1.2
                            color: "#fff3d7"
                            castsShadow: false
                        }

                        DirectionalLight {
                            objectName: "fillMapLight"
                            eulerRotation.x: -20
                            eulerRotation.y: 145
                            brightness: 0.35
                            color: "#b9dbf2"
                            castsShadow: false
                        }

                        Instantiator {
                            id: visualMaterialCache
                            model: window.viewer.visualMaterials

                            delegate: PrincipledMaterial {
                                required property var modelData

                                objectName: "trackVisualMaterial"
                                Texture {
                                    id: replacementBaseMap
                                    objectName: "trackVisualBaseTexture"
                                    source: modelData.baseTexture
                                    tilingModeHorizontal: Texture.Repeat
                                    tilingModeVertical: Texture.Repeat
                                    generateMipmaps: true
                                    minFilter: Texture.Linear
                                    magFilter: Texture.Linear
                                    mipFilter: Texture.Linear
                                }

                                lighting:
                                    PrincipledMaterial.FragmentLighting
                                baseColor: window.renderMode ===
                                           "neutral"
                                           ? "#aeb3af"
                                           : (window.renderMode ===
                                              "material-debug"
                                              ? modelData.debugColor
                                              : "#ffffff")
                                baseColorMap: window.renderMode ===
                                              "textured"
                                              ? replacementBaseMap
                                              : null
                                roughness: window.renderMode ===
                                           "neutral"
                                           ? 0.74
                                           : modelData.roughness
                                metalness: window.renderMode ===
                                           "neutral"
                                           ? 0
                                           : modelData.metalness
                                cullMode: Material.NoCulling
                                vertexColorsEnabled:
                                    modelData.vertexColors
                                    && window.renderMode === "textured"
                                emissiveMap: window.renderMode ===
                                             "textured"
                                             && modelData.emissiveStrength > 0
                                             ? replacementBaseMap
                                             : null
                                emissiveFactor: window.renderMode ===
                                                "textured"
                                                ? Qt.vector3d(
                                                      modelData.emissiveStrength,
                                                      modelData.emissiveStrength,
                                                      modelData.emissiveStrength)
                                                : Qt.vector3d(0, 0, 0)
                            }
                        }

                        Repeater3D {
                            model: window.viewer.visualBatches

                            delegate: Model {
                                required property var modelData
                                readonly property int materialBindingIndex:
                                    modelData.materialBindingIndex
                                readonly property var sharedMaterial: {
                                    const cacheSize = visualMaterialCache.count
                                    return cacheSize > 0
                                           ? visualMaterialCache.objectAt(
                                                 materialBindingIndex)
                                           : null
                                }

                                objectName: "trackVisualModel"
                                visible: window.viewer.loaded
                                         && window.renderMode === "textured"
                                         && modelData.defaultVisible
                                geometry: modelData.geometry
                                castsShadows: false

                                materials: sharedMaterial
                                           ? [sharedMaterial] : []
                            }
                        }

                        CuboidEditorScene {
                            objectName: "rasterCuboidEditorScene"
                            visible: !window.controller.drawTargetsThroughBlocks
                            interactive: true
                        }

                        CustomVolumeEditorScene {
                            objectName: "rasterCustomVolumeEditorScene"
                            visible: !window.controller.drawTargetsThroughBlocks
                            interactive: true
                        }

                        PoseTargetEditorScene {
                            objectName: "rasterPoseTargetEditorScene"
                            visible: !window.controller.drawTargetsThroughBlocks
                            interactive: true
                        }

                        Model {
                            objectName: "trackFilledModel"
                            visible: window.viewer.loaded
                                     && (window.renderMode === "neutral"
                                         || window.renderMode === "collision"
                                         || window.renderMode ===
                                            "material-debug")
                            geometry: window.viewer.loaded
                                      ? window.viewer.trackFilledGeometry
                                      : null
                            materials: DefaultMaterial {
                                lighting: DefaultMaterial.NoLighting
                                vertexColorsEnabled: true
                                diffuseColor: window.renderMode === "neutral"
                                              ? "#aeb3af" : "white"
                                cullMode: Material.BackFaceCulling
                            }
                        }

                        Model {
                            objectName: "trackWireModel"
                            visible: window.viewer.loaded
                                     && window.renderMode === "wireframe"
                            geometry: window.viewer.loaded
                                      ? window.viewer.trackWireGeometry
                                      : null
                            materials: DefaultMaterial {
                                lighting: DefaultMaterial.NoLighting
                                diffuseColor: "#b8d9c7"
                                cullMode: Material.NoCulling
                            }
                        }

                        Repeater3D {
                            model: window.viewer.trajectoryPaths

                            delegate: Model {
                                required property var modelData

                                objectName: "trajectoryPathModel"
                                visible: window.viewer.loaded
                                         && (modelData.visible ?? true)
                                geometry: modelData.geometry
                                castsShadows: false
                                receivesShadows: false
                                materials: DefaultMaterial {
                                    lighting: DefaultMaterial.NoLighting
                                    diffuseColor: modelData.color
                                    opacity: modelData.opacity
                                    cullMode: Material.NoCulling
                                }
                            }
                        }

                        Repeater3D {
                            model: window.viewer.runCount

                            delegate: Node {
                                id: runCarRoot

                                required property int index
                                readonly property var runPose:
                                    window.viewer.runPoses[index]
                                readonly property int runIndex: index

                                objectName: "runCarRoot"
                                visible: !runPose.selected
                                position: runPose.position
                                rotation: runPose.rotation

                                Repeater3D {
                                    model: window.viewer.carEllipsoids.length

                                    delegate: Node {
                                        required property int index
                                        readonly property var ellipsoid:
                                            window.viewer.carEllipsoids[
                                                index]

                                        objectName: "runCarEllipsoidNode"
                                        position: ellipsoid.position
                                        rotation: ellipsoid.rotation
                                        scale: ellipsoid.radii

                                        Model {
                                            objectName: "runCarFilledModel"
                                            visible: window.renderMode !==
                                                     "wireframe"
                                            geometry: window.viewer
                                                .ellipsoidFilledGeometries[
                                                    runCarRoot.runIndex %
                                                    window.viewer
                                                        .ellipsoidFilledGeometries
                                                        .length]
                                            materials: DefaultMaterial {
                                                objectName: "runCarFilledMaterial"
                                                lighting:
                                                    DefaultMaterial.NoLighting
                                                vertexColorsEnabled: true
                                                diffuseColor: "white"
                                                cullMode:
                                                    Material.BackFaceCulling
                                            }
                                        }

                                        Model {
                                            objectName: "runCarWireModel"
                                            visible: window.renderMode ===
                                                     "wireframe"
                                            geometry:
                                                window.viewer.ellipsoidWireGeometry
                                            materials: DefaultMaterial {
                                                lighting:
                                                    DefaultMaterial.NoLighting
                                                diffuseColor: window.runColor(
                                                    runCarRoot.runIndex)
                                                cullMode: Material.NoCulling
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Node {
                            id: selectedRunCarRoot

                            objectName: "selectedRunCarRoot"
                            visible: window.viewer.loaded
                                     && window.viewer.runCount > 0
                                     && !(viewport.carCameraActive
                                          && window.viewer.hideSelectedCar)
                            position: window.viewer.carPosition
                            rotation: window.viewer.carRotation

                            Repeater3D {
                                model: window.viewer.carEllipsoids.length

                                delegate: Node {
                                    required property int index
                                    readonly property var ellipsoid:
                                        window.viewer.carEllipsoids[index]

                                    objectName: "selectedRunCarEllipsoidNode"
                                    position: ellipsoid.position
                                    rotation: ellipsoid.rotation
                                    scale: ellipsoid.radii

                                    Model {
                                        objectName: "selectedRunCarFilledModel"
                                        visible: window.renderMode !==
                                                 "wireframe"
                                        geometry: window.viewer
                                            .selectedEllipsoidFilledGeometry
                                        materials: DefaultMaterial {
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            vertexColorsEnabled: true
                                            diffuseColor: "white"
                                            cullMode:
                                                Material.BackFaceCulling
                                        }
                                    }

                                    Model {
                                        objectName: "selectedRunCarWireModel"
                                        visible: window.renderMode ===
                                                 "wireframe"
                                        geometry:
                                            window.viewer
                                                .ellipsoidWireGeometry
                                        materials: DefaultMaterial {
                                            lighting:
                                                DefaultMaterial.NoLighting
                                            diffuseColor: window.runColor(
                                                window.viewer
                                                    .selectedRunIndex)
                                            cullMode: Material.NoCulling
                                        }
                                    }
                                }
                            }
                        }
                    }

                    GpuRayTracingView {
                        id: gpuRayTracingView
                        objectName: "gpuRayTracingView"
                        anchors.fill: parent
                        z: 1
                        visible: window.rayTracingEnabled
                        active: window.rayTracingEnabled
                                && window.viewer.loaded
                        viewer: window.viewer
                        cameraPosition: viewCamera.scenePosition
                        cameraTarget: viewport.cameraTarget
                        cameraUp: viewCamera.up
                        fieldOfView: viewCamera.fieldOfView
                    }

                    View3D {
                        id: rayTracingTrajectoryOverlay
                        objectName: "rayTracingTrajectoryOverlay"
                        anchors.fill: parent
                        z: 1.5
                        camera: rayTracingOverlayCamera
                        visible: (window.rayTracingEnabled
                                  && window.viewer.trajectoryCount > 0)
                                 || ((window.rayTracingEnabled
                                      || window.controller
                                               .drawTargetsThroughBlocks)
                                     && (window.controller
                                                  .cuboidTargets.count > 0
                                         || window.controller
                                                  .customVolumeTargets.count
                                            > 0
                                         || window.controller
                                                  .poseTargets.count > 0))

                        environment: SceneEnvironment {
                            backgroundMode: SceneEnvironment.Transparent
                            antialiasingMode: SceneEnvironment.MSAA
                            antialiasingQuality: SceneEnvironment.Medium
                        }

                        Node {
                            position: viewport.carCameraActive
                                      ? window.viewer.carCameraPosition
                                      : viewport.freeCamera
                                        ? viewport.freeCameraPosition
                                        : viewport.cameraTarget
                            rotation: viewport.carCameraActive
                                      ? window.viewer.carCameraRotation
                                      : Qt.quaternion(1, 0, 0, 0)

                            Node {
                                eulerRotation.x: viewport.carCameraActive
                                                 ? 0 : viewport.orbitPitch
                                eulerRotation.y: viewport.carCameraActive
                                                 ? 0 : viewport.orbitYaw

                                PerspectiveCamera {
                                    id: rayTracingOverlayCamera
                                    readonly property var dynamicClipPlanes:
                                        window.viewer.cameraClipPlanes(
                                            scenePosition,
                                            viewCamera.clipDistance)

                                    z: viewport.carCameraActive
                                       || viewport.freeCamera
                                       ? 0 : viewport.orbitDistance
                                    clipNear: dynamicClipPlanes.x
                                    clipFar: dynamicClipPlanes.y
                                    fieldOfView: viewCamera.fieldOfView
                                    fieldOfViewOrientation:
                                        PerspectiveCamera.Vertical
                                }
                            }
                        }

                        Repeater3D {
                            model: window.viewer.trajectoryPaths

                            delegate: Model {
                                required property var modelData

                                objectName:
                                    "rayTracingTrajectoryPathModel"
                                visible: window.rayTracingEnabled
                                         && (modelData.visible ?? true)
                                geometry: modelData.geometry
                                castsShadows: false
                                receivesShadows: false
                                materials: DefaultMaterial {
                                    lighting: DefaultMaterial.NoLighting
                                    diffuseColor: modelData.color
                                    opacity: modelData.opacity
                                    cullMode: Material.NoCulling
                                }
                            }
                        }

                        CuboidEditorScene {
                            objectName: "rayTracingCuboidEditorScene"
                            visible: window.rayTracingEnabled
                                     || window.controller
                                              .drawTargetsThroughBlocks
                            interactive: true
                        }

                        CustomVolumeEditorScene {
                            objectName: "rayTracingCustomVolumeEditorScene"
                            visible: window.rayTracingEnabled
                                     || window.controller
                                              .drawTargetsThroughBlocks
                            interactive: true
                        }

                        PoseTargetEditorScene {
                            objectName: "rayTracingPoseTargetEditorScene"
                            visible: window.rayTracingEnabled
                                     || window.controller
                                              .drawTargetsThroughBlocks
                            interactive: true
                        }
                    }

                    WhiteboardPlanes {
                        id: whiteboardPlaneView
                        anchors.fill: parent
                        z: 1.75
                        model: window.viewer.whiteboard
                        cameraTarget: viewport.cameraTarget
                        cameraPosition: viewCamera.scenePosition
                        freeCamera: viewport.freeCamera
                        orbitYaw: viewport.orbitYaw
                        orbitPitch: viewport.orbitPitch
                        orbitDistance: viewport.orbitDistance
                        fieldOfView: viewCamera.fieldOfView
                        exactBoardIndex:
                            viewport.exactWhiteboardBoardIndex
                        contentTop: whiteboardOverlay.boardTop
                    }

                    Connections {
                        target: window.viewer.whiteboard

                        function onBoardsChanged() {
                            viewport.synchronizeWhiteboardFocus()
                        }
                    }

                    Connections {
                        target: window.controller

                        function onCuboidFocusRequested(center, size) {
                            viewport.focusCuboid(center, size)
                        }
                        function onCustomVolumeFocusRequested(center, size) {
                            viewport.focusCuboid(center, size)
                        }
                        function onPoseTargetFocusRequested(center, size) {
                            viewport.focusCuboid(center, size)
                        }
                    }

                    MouseArea {
                        id: manualInputFocus
                        objectName: "manualInputFocus"
                        anchors.fill: parent
                        z: 2
                        focus: false
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Canvas
                        Accessible.name: qsTr("Race preview")
                        Accessible.focusable: true
                        acceptedButtons: Qt.LeftButton
                        hoverEnabled: true
                        property real previousX: 0
                        property real previousY: 0

                        Keys.priority: Keys.BeforeItem
                        Keys.onPressed: event => {
                            viewport.handleCameraPresetKey(event, true)
                            if (!event.accepted)
                                viewport.handleFreeCameraKey(event, true)
                            if (!event.accepted)
                                window.handleManualKey(event, true)
                        }
                        Keys.onReleased: event => {
                            viewport.handleCameraPresetKey(event, false)
                            if (!event.accepted)
                                viewport.handleFreeCameraKey(event, false)
                            if (!event.accepted)
                                window.handleManualKey(event, false)
                        }
                        onActiveFocusChanged: {
                            if (!activeFocus
                                && window.viewer.manualDriving) {
                                window.viewer.releaseManualInputs()
                            }
                            if (!activeFocus)
                                viewport.releaseFreeMovement()
                        }

                        onPressed: mouse => {
                            if (window.viewer.manualDriving
                                || window.viewer.takeOverOnInput
                                || viewport.freeCamera)
                                manualInputFocus.forceActiveFocus()
                            previousX = mouse.x
                            previousY = mouse.y
                            viewport.beginViewRotation()
                            if (window.controller.customVolumeDrawing) {
                                const point = viewport.customPlanePoint(
                                    mouse.x, mouse.y)
                                window.controller.customVolumeTargets
                                      .addVertexWorld(
                                          point.x, point.y, point.z)
                                viewport.cuboidPointerCaptured = true
                                return
                            }
                            if (viewport.whiteboardPlaneFocusEnabled()) {
                                const boardIndex =
                                    whiteboardPlaneView.pickBoard(
                                        mouse.x, mouse.y)
                                if (boardIndex >= 0
                                    && window.viewer.whiteboard
                                             .selectBoard(boardIndex)) {
                                    viewport.restoreWhiteboardView(
                                        window.viewer.whiteboard
                                              .boards[boardIndex])
                                    viewport.cuboidPointerCaptured = true
                                    return
                                }
                            }
                            const view = window.rayTracingEnabled
                                         || window.controller
                                                  .drawTargetsThroughBlocks
                                         ? rayTracingTrajectoryOverlay
                                         : rasterMapView
                            const hit = view.pick(mouse.x, mouse.y).objectHit
                            if (hit && hit.targetIndex !== undefined
                                && !window.controller.running) {
                                viewport.cuboidPointerCaptured = true
                                if (hit.editorKind
                                    && hit.editorKind.indexOf("pose-")
                                       === 0) {
                                    window.controller.poseTargets
                                          .selectTarget(hit.targetIndex)
                                    window.controller.evaluationTargetId =
                                        "pose-target"
                                    viewport.beginPoseInteraction(
                                        hit.editorKind,
                                        hit.editorAxis,
                                        mouse.x,
                                        mouse.y)
                                } else if (hit.editorKind
                                    && hit.editorKind.indexOf("custom-")
                                       === 0) {
                                    window.controller.customVolumeTargets
                                          .selectTarget(hit.targetIndex)
                                    window.controller.evaluationTargetId =
                                        "custom-volume-entry-time"
                                    if (hit.editorKind === "custom-plane") {
                                        window.controller.customVolumeTargets
                                              .setPlane(
                                                  hit.targetIndex,
                                                  hit.planeChoice)
                                    }
                                    viewport.beginCustomInteraction(
                                        hit.editorKind,
                                        hit.vertexIndex,
                                        mouse.x,
                                        mouse.y)
                                } else {
                                    window.controller.cuboidTargets
                                          .selectTarget(hit.targetIndex)
                                    window.controller.evaluationTargetId =
                                        "volume-entry-time"
                                }
                                if (hit.editorKind === "move"
                                    || hit.editorKind === "resize") {
                                    viewport.beginCuboidInteraction(
                                        hit.editorKind,
                                        hit.editorAxis,
                                        mouse.x,
                                        mouse.y)
                                }
                                return
                            }
                        }
                        onPositionChanged: mouse => {
                            if (!(mouse.buttons & Qt.LeftButton))
                                return
                            if (viewport.cuboidDragActive) {
                                viewport.updateCuboidInteraction(
                                    mouse.x, mouse.y)
                                previousX = mouse.x
                                previousY = mouse.y
                                return
                            }
                            if (viewport.customVertexDragActive
                                || viewport.customDepthDragActive) {
                                viewport.updateCustomInteraction(
                                    mouse.x, mouse.y)
                                previousX = mouse.x
                                previousY = mouse.y
                                return
                            }
                            if (viewport.poseDragActive) {
                                viewport.updatePoseInteraction(
                                    mouse.x, mouse.y)
                                previousX = mouse.x
                                previousY = mouse.y
                                return
                            }
                            if (viewport.cuboidPointerCaptured)
                                return
                            viewport.leaveExactWhiteboardView()
                            viewport.updateViewRotation(
                                mouse.x - previousX,
                                mouse.y - previousY)
                            previousX = mouse.x
                            previousY = mouse.y
                        }
                        onReleased: {
                            viewport.endCuboidInteraction()
                            viewport.endCustomInteraction()
                            viewport.endPoseInteraction()
                            viewport.cuboidPointerCaptured = false
                            if (window.viewer.manualDriving
                                || window.viewer.takeOverOnInput
                                || viewport.freeCamera)
                                manualInputFocus.forceActiveFocus()
                        }
                        onCanceled: {
                            viewport.endCuboidInteraction()
                            viewport.endCustomInteraction()
                            viewport.endPoseInteraction()
                            viewport.cuboidPointerCaptured = false
                        }
                        onWheel: wheel => {
                            if (viewport.zoomOrbit(wheel.angleDelta.y))
                                wheel.accepted = true
                        }
                    }

                    WhiteboardOverlay {
                        id: whiteboardOverlay
                        objectName: "whiteboardOverlay"
                        anchors.fill: parent
                        z: 2.5
                        model: window.viewer.whiteboard
                        available: window.viewer.loaded
                        captureViewpoint: function() {
                            return viewport.captureWhiteboardView()
                        }
                        restoreViewpoint: function(board) {
                            viewport.restoreWhiteboardView(board)
                        }
                        imageExportInProgress:
                            viewport.exportingWhiteboardImage
                        exportBackgroundImage: function(index, fileUrl) {
                            return viewport.exportWhiteboardBackground(
                                        index, fileUrl)
                        }
                        toolbarMaximumWidth: Math.max(
                            198, cameraFocusToolbar.x - 22)
                        visible: !viewport.exportingWhiteboardImage
                    }

                    Rectangle {
                        id: cameraFocusToolbar
                        objectName: "cameraFocusToolbar"

                        function layoutX(viewportWidth, drawingListOpen) {
                            return viewportWidth - width - 12
                        }

                        function layoutTopMargin(viewportWidth,
                                                 drawingListOpen) {
                            return 10
                        }

                        anchors.top: raceViewerHeader.bottom
                        anchors.topMargin: layoutTopMargin(
                                               parent.width,
                                               whiteboardOverlay
                                                       .drawingListOpen)
                        x: layoutX(parent.width,
                                   whiteboardOverlay.drawingListOpen)
                        z: 3
                        width: cameraFocusControls.implicitWidth + 12
                        height: 42
                        radius: 6
                        color: AppTheme.viewerOverlay
                        border.width: 1
                        border.color: AppTheme.viewerOverlayBorder
                        visible: window.viewer.loaded
                                 && !viewport.exportingWhiteboardImage

                        RowLayout {
                            id: cameraFocusControls
                            anchors.centerIn: parent
                            spacing: 4

                            ThemedButton {
                                objectName: "freeCameraButton"
                                Layout.preferredWidth: Math.max(58, implicitWidth)
                                Layout.preferredHeight: 32
                                text: qsTr("Free")
                                highlighted: viewport.freeCamera
                                Accessible.name: qsTr("Free camera")
                                onClicked: {
                                    viewport.enableFreeCamera()
                                    manualInputFocus.forceActiveFocus()
                                }
                                ToolTip.visible: hovered
                                ToolTip.delay: 350
                                ToolTip.text: qsTr("Free camera")
                            }

                            ThemedButton {
                                objectName: "orbitalCameraButton"
                                Layout.preferredWidth: Math.max(70, implicitWidth)
                                Layout.preferredHeight: 32
                                text: qsTr("Orbital")
                                enabled: window.viewer.runCount > 0
                                highlighted: enabled
                                             && viewport.orbitalCamera
                                Accessible.name: qsTr("Orbital camera")
                                onClicked: {
                                    viewport.enableOrbitalCamera()
                                    manualInputFocus.forceActiveFocus()
                                }
                                ToolTip.visible: hovered
                                ToolTip.delay: 350
                                ToolTip.text: qsTr(
                                    "Orbit around the car with drag and wheel")
                            }

                            ThemedButton {
                                objectName: "focusCarButton"
                                Layout.preferredWidth: Math.max(52, implicitWidth)
                                Layout.preferredHeight: 32
                                text: qsTr("Far")
                                enabled: window.viewer.runCount > 0
                                         && window.viewer.carCameraAvailable
                                highlighted: enabled
                                    && viewport.carCameraActive
                                    && window.viewer.cameraPreset === 1
                                Accessible.name: qsTr("Far car camera")
                                onClicked: {
                                    window.viewer.cameraPreset = 1
                                    viewport.focusCurrentCar()
                                    manualInputFocus.forceActiveFocus()
                                }
                                ToolTip.visible: hovered
                                ToolTip.delay: 350
                                ToolTip.text: qsTr("Far car camera (1)")
                            }

                            ThemedButton {
                                objectName: "nearCameraButton"
                                Layout.preferredWidth: Math.max(60, implicitWidth)
                                Layout.preferredHeight: 32
                                text: qsTr("Near")
                                enabled: window.viewer.runCount > 0
                                         && window.viewer.carCameraAvailable
                                highlighted: enabled
                                    && viewport.carCameraActive
                                    && window.viewer.cameraPreset === 2
                                Accessible.name: qsTr("Near car camera")
                                onClicked: {
                                    window.viewer.cameraPreset = 2
                                    viewport.focusCurrentCar()
                                    manualInputFocus.forceActiveFocus()
                                }
                                ToolTip.visible: hovered
                                ToolTip.delay: 350
                                ToolTip.text: qsTr("Near car camera (2)")
                            }

                            ThemedButton {
                                objectName: "internalCameraButton"
                                Layout.preferredWidth: Math.max(82, implicitWidth)
                                Layout.preferredHeight: 32
                                text: qsTr("Internal")
                                enabled: window.viewer.runCount > 0
                                         && window.viewer.carCameraAvailable
                                highlighted: enabled
                                    && viewport.carCameraActive
                                    && window.viewer.cameraPreset === 3
                                Accessible.name: qsTr("Internal car camera")
                                onClicked: {
                                    window.viewer.cameraPreset = 3
                                    viewport.focusCurrentCar()
                                    manualInputFocus.forceActiveFocus()
                                }
                                ToolTip.visible: hovered
                                ToolTip.delay: 350
                                ToolTip.text: qsTr("Internal car camera (3)")
                            }

                            ThemedButton {
                                objectName: "focusObjectButton"
                                Layout.preferredWidth: 64
                                Layout.preferredHeight: 32
                                text: qsTr("Target")
                                enabled: viewport.hasObjectFocus
                                highlighted: !viewport.freeCamera
                                             && viewport.cuboidFocused
                                Accessible.name: qsTr("Focus selected target")
                                onClicked: {
                                    viewport.focusLastObject()
                                    manualInputFocus.forceActiveFocus()
                                }
                                ToolTip.visible: hovered
                                ToolTip.delay: 350
                                ToolTip.text: qsTr("Focus selected target")
                            }
                        }
                    }

                    Rectangle {
                        id: scriptedTelemetry
                        objectName: "scriptedTelemetry"
                        anchors.top: cameraFocusToolbar.bottom
                        anchors.topMargin: 6
                        anchors.right: cameraFocusToolbar.right
                        z: 3
                        width: Math.min(
                                   480,
                                   Math.max(
                                       220,
                                       scriptedTelemetryContent
                                           .implicitWidth + 16))
                        height: Math.min(
                                    150,
                                    Math.max(
                                        34,
                                        scriptedTelemetryContent
                                            .implicitHeight + 12))
                        radius: 6
                        color: AppTheme.viewerOverlay
                        border.width: 1
                        border.color: AppTheme.viewerOverlayBorder
                        visible: window.viewer.loaded
                                 && !viewport.exportingWhiteboardImage

                        RowLayout {
                            id: scriptedTelemetryContent
                            anchors.fill: parent
                            anchors.margins: 6
                            spacing: 4

                            Label {
                                id: scriptedTelemetryText
                                objectName: "scriptedTelemetryText"
                                Layout.fillWidth: true
                                Layout.maximumWidth: 430
                                text: viewport.renderTelemetry(
                                          window.viewer.telemetryScript)
                                color: AppTheme.viewerOverlayText
                                font.pixelSize: 12
                                font.family: "monospace"
                                wrapMode: Text.Wrap
                                Accessible.name: qsTr("Scripted telemetry")
                                Accessible.description: text
                            }

                            ThemedToolButton {
                                id: editTelemetryButton
                                objectName: "editTelemetryButton"
                                Layout.preferredWidth: 26
                                Layout.preferredHeight: 26
                                text: "\u270e"
                                font.pixelSize: 15
                                Accessible.name: qsTr("Edit scripted telemetry")
                                onClicked: telemetryEditorDialog.open()
                                ToolTip.visible: hovered
                                ToolTip.delay: 350
                                ToolTip.text: qsTr("Edit scripted telemetry")
                            }
                        }
                    }

                    Rectangle {
                        id: raceViewerHeader
                        objectName: "raceViewerHeader"
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        z: 3
                        visible: !viewport.exportingWhiteboardImage
                        height: 52
                        color: AppTheme.viewerOverlay

                        RowLayout {
                            id: headerControlsRow
                            objectName: "headerControlsRow"
                            anchors.fill: parent
                            anchors.leftMargin: 14
                            anchors.rightMargin: 14
                            spacing: 10

                            ColumnLayout {
                                id: raceViewerTitleBlock
                                objectName: "raceViewerTitleBlock"
                                Layout.fillWidth: true
                                Layout.minimumWidth:
                                    raceViewerHeader.width < 650 ? 100 : 150
                                Layout.alignment: Qt.AlignVCenter
                                spacing: 0

                                Label {
                                    text: qsTr("Race Viewer")
                                    color: AppTheme.viewerOverlayText
                                    font.pixelSize: 17
                                    font.weight: Font.DemiBold
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: window.viewer.loaded
                                          ? qsTr("%1 triangles · %2 batches · %3 FPS")
                                                .arg(window.controller
                                                         .formatCompactNumber(
                                                             window.viewer
                                                               .visualTriangleCount))
                                                .arg(window.controller
                                                         .formatCompactNumber(
                                                             window.viewer
                                                               .visualBatchCount))
                                                .arg(Math.round(
                                                         window.measuredFps))
                                          : window.viewer.statusText
                                    color: AppTheme.viewerOverlayMuted
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }

                            ThemedCheckBox {
                                id: trajectoryVisibilityToggle

                                objectName: "trajectoryVisibilityToggle"
                                Layout.preferredWidth: 30
                                Layout.preferredHeight: 30
                                Layout.alignment: Qt.AlignVCenter
                                text: ""
                                enabled: window.viewer.hasTrajectoryForRun(
                                             window.viewer.selectedRunId)
                                         && !window.viewer.manualDriving
                                checked: {
                                    const paths = window.viewer.trajectoryPaths
                                    return window.viewer
                                        .trajectoryVisibleForRun(
                                            window.viewer.selectedRunId)
                                }
                                Accessible.name: qsTr(
                                    "Show selected run trajectory")
                                onClicked:
                                    window.viewer.setTrajectoryVisibleForRun(
                                        window.viewer.selectedRunId,
                                        checked)
                                ToolTip.visible: hovered
                                ToolTip.delay: 350
                                ToolTip.text: checked
                                    ? qsTr("Hide selected run trajectory")
                                    : qsTr("Show selected run trajectory")
                            }

                            StyledComboBox {
                                id: runSelector

                                objectName: "runSelector"
                                Layout.preferredWidth:
                                    raceViewerHeader.width < 650 ? 118 : 160
                                Layout.alignment: Qt.AlignVCenter
                                model: window.viewer.runOptions
                                textRole: "name"
                                valueRole: "id"
                                enabled: count > 0
                                         && !window.viewer.manualDriving

                                function synchronizeSelection() {
                                    const selected = indexOfValue(
                                        window.viewer.selectedRunId)
                                    if (selected >= 0
                                        && currentIndex !== selected) {
                                        currentIndex = selected
                                    }
                                }

                                Component.onCompleted:
                                    synchronizeSelection()
                                onModelChanged:
                                    Qt.callLater(synchronizeSelection)
                                onActivated: selectedIndex =>
                                    window.viewer.selectedRunId =
                                        valueAt(selectedIndex)

                                Connections {
                                    target: window.viewer

                                    function onRunsChanged() {
                                        Qt.callLater(
                                            runSelector.synchronizeSelection)
                                    }
                                    function onSelectedRunChanged() {
                                        runSelector.synchronizeSelection()
                                    }
                                }
                            }


                            ThemedButton {
                                id: clearPreviewTrajectoriesButton
                                objectName: "clearPreviewTrajectoriesButton"
                                Layout.preferredWidth:
                                    raceViewerHeader.width < 650 ? 82 : 104
                                Layout.alignment: Qt.AlignVCenter
                                visible: window.viewer.selectedRunId === "best"
                                enabled: {
                                    const paths = window.viewer.trajectoryPaths
                                    return visible
                                        && window.viewer
                                            .hasPreviewTrajectories()
                                        && !window.viewer.manualDriving
                                }
                                text: qsTr("Clear previews")
                                font.pixelSize: 10
                                onClicked:
                                    window.viewer.clearPreviewTrajectories()
                                ToolTip.visible: hovered
                                ToolTip.delay: 350
                                ToolTip.text: qsTr(
                                    "Remove all search preview trajectories")
                            }

                            StyledComboBox {
                                id: renderModeSelector
                                objectName: "renderModeSelector"
                                Layout.preferredWidth:
                                    raceViewerHeader.width < 650 ? 140 : 180
                                Layout.alignment: Qt.AlignVCenter
                                enabled: window.viewer.loaded
                                model: gpuRayTracingView.supported
                                       ? [
                                             { "text": qsTr("Textured"),
                                               "value": "textured" },
                                             { "text": qsTr("Textured (RT)"),
                                               "value": "textured-rt" },
                                             { "text": qsTr("Neutral"),
                                               "value": "neutral" },
                                             { "text": qsTr("Collision"),
                                               "value": "collision" },
                                             { "text": qsTr("Wireframe"),
                                               "value": "wireframe" },
                                             { "text": qsTr("High Contrast"),
                                               "value": "material-debug" }
                                         ]
                                       : [
                                             { "text": qsTr("Textured"),
                                               "value": "textured" },
                                             { "text": qsTr("Neutral"),
                                               "value": "neutral" },
                                             { "text": qsTr("Collision"),
                                               "value": "collision" },
                                             { "text": qsTr("Wireframe"),
                                               "value": "wireframe" },
                                             { "text": qsTr("High Contrast"),
                                               "value": "material-debug" }
                                         ]
                                textRole: "text"
                                valueRole: "value"
                                onActivated:
                                    window.renderMode = currentValue

                                ToolTip.visible:
                                    hovered
                                    && currentValue === "textured-rt"
                                ToolTip.delay: 350
                                ToolTip.text:
                                    currentValue === "textured-rt"
                                    ? gpuRayTracingView.status : ""
                            }

                            ThemedButton {
                                objectName: "resetViewButton"
                                Layout.alignment: Qt.AlignVCenter
                                Layout.preferredWidth:
                                    raceViewerHeader.width < 650 ? 86 : 100
                                text: qsTr("Reset view")
                                enabled: window.viewer.loaded
                                onClicked: {
                                    viewport.orbitYaw = 35
                                    viewport.orbitPitch = -20
                                    viewport.orbitDistance = 38
                                    viewport.focusCurrentCar()
                                }
                            }
                        }
                    }

                    Rectangle {
                        id: playbackDock
                        objectName: "playbackDock"
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 20
                        z: 3
                        visible: !viewport.exportingWhiteboardImage
                        width: 430
                        height: 58
                        radius: 16
                        color: AppTheme.viewerOverlay
                        border.width: 1
                        border.color: AppTheme.viewerOverlayBorder

                        RowLayout {
                            anchors.centerIn: parent
                            spacing: 8

                            ThemedToolButton {
                                id: jumpStartButton
                                objectName: "jumpStartButton"
                                Layout.preferredWidth: 42
                                Layout.preferredHeight: 42
                                implicitWidth: 42
                                implicitHeight: 42
                                text: ""
                                enabled: window.viewer.runCount > 0
                                         && !window.viewer.manualDriving
                                palette.buttonText: AppTheme.viewerOverlayText
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Go to start")
                                onClicked: window.viewer.jumpToStart()

                                contentItem: Item {
                                    Item {
                                        objectName: "jumpStartTransportIcon"
                                        anchors.centerIn: parent
                                        width: 18
                                        height: 18

                                        Rectangle {
                                            x: 2
                                            y: 2
                                            width: 3
                                            height: 14
                                            radius: 1
                                            color:
                                                jumpStartButton.effectiveTextColor
                                        }

                                        Canvas {
                                            anchors.fill: parent
                                            property color iconColor:
                                                jumpStartButton.effectiveTextColor
                                            antialiasing: true
                                            onIconColorChanged: requestPaint()
                                            onPaint: {
                                                const context = getContext("2d")
                                                context.clearRect(0, 0,
                                                                  width, height)
                                                context.fillStyle = iconColor
                                                context.beginPath()
                                                context.moveTo(15, 2)
                                                context.lineTo(6, 9)
                                                context.lineTo(15, 16)
                                                context.closePath()
                                                context.fill()
                                            }
                                        }
                                    }
                                }
                            }

                            ThemedToolButton {
                                id: playPauseButton
                                objectName: "playPauseButton"
                                Layout.preferredWidth: 42
                                Layout.preferredHeight: 42
                                implicitWidth: 42
                                implicitHeight: 42
                                text: ""
                                enabled: window.viewer.runCount > 0
                                         && !window.viewer.manualDriving
                                palette.buttonText:
                                    AppTheme.viewerOverlayText
                                ToolTip.visible: hovered
                                ToolTip.text: window.viewer.playing
                                              ? qsTr("Pause")
                                              : qsTr("Play")
                                onClicked: window.viewer.togglePlayback()

                                contentItem: Item {
                                    Canvas {
                                        objectName: "playTransportIcon"
                                        anchors.centerIn: parent
                                        width: 18
                                        height: 18
                                        visible: !window.viewer.playing
                                        property color iconColor:
                                            playPauseButton.effectiveTextColor
                                        antialiasing: true
                                        onIconColorChanged: requestPaint()
                                        onPaint: {
                                            const context = getContext("2d")
                                            context.clearRect(0, 0,
                                                              width, height)
                                            context.fillStyle = iconColor
                                            context.beginPath()
                                            context.moveTo(4, 2)
                                            context.lineTo(16, 9)
                                            context.lineTo(4, 16)
                                            context.closePath()
                                            context.fill()
                                        }
                                    }

                                    Item {
                                        objectName: "pauseTransportIcon"
                                        anchors.centerIn: parent
                                        width: 18
                                        height: 18
                                        visible: window.viewer.playing

                                        Rectangle {
                                            x: 3
                                            y: 2
                                            width: 4
                                            height: 14
                                            radius: 1
                                            color:
                                                playPauseButton.effectiveTextColor
                                        }

                                        Rectangle {
                                            x: 11
                                            y: 2
                                            width: 4
                                            height: 14
                                            radius: 1
                                            color:
                                                playPauseButton.effectiveTextColor
                                        }
                                    }
                                }
                            }

                            ThemedToolButton {
                                id: jumpEndButton
                                objectName: "jumpEndButton"
                                Layout.preferredWidth: 42
                                Layout.preferredHeight: 42
                                implicitWidth: 42
                                implicitHeight: 42
                                text: ""
                                enabled: window.viewer.runCount > 0
                                         && !window.viewer.manualDriving
                                palette.buttonText: AppTheme.viewerOverlayText
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Go to end")
                                onClicked: window.viewer.jumpToEnd()

                                contentItem: Item {
                                    Item {
                                        objectName: "jumpEndTransportIcon"
                                        anchors.centerIn: parent
                                        width: 18
                                        height: 18

                                        Canvas {
                                            anchors.fill: parent
                                            property color iconColor:
                                                jumpEndButton.effectiveTextColor
                                            antialiasing: true
                                            onIconColorChanged: requestPaint()
                                            onPaint: {
                                                const context = getContext("2d")
                                                context.clearRect(0, 0,
                                                                  width, height)
                                                context.fillStyle = iconColor
                                                context.beginPath()
                                                context.moveTo(3, 2)
                                                context.lineTo(12, 9)
                                                context.lineTo(3, 16)
                                                context.closePath()
                                                context.fill()
                                            }
                                        }

                                        Rectangle {
                                            x: 13
                                            y: 2
                                            width: 3
                                            height: 14
                                            radius: 1
                                            color:
                                                jumpEndButton.effectiveTextColor
                                        }
                                    }
                                }
                            }

                            ThemedButton {
                                id: manualDriveButton
                                objectName: "manualDriveButton"
                                Layout.preferredWidth: 84
                                Layout.preferredHeight: 42
                                text: window.viewer.manualDriving
                                      ? qsTr("Stop")
                                      : qsTr("Drive")
                                enabled: window.viewer.manualDriving
                                         || (window.viewer.loaded
                                             && !window.viewer.loading
                                             && !window.controller.running
                                             && !window.controller.extractingReplayInputs)
                                onClicked: {
                                    if (window.viewer.manualDriving) {
                                        window.viewer.stopManualDrive()
                                    } else {
                                        window.viewer.startManualDrive()
                                        if (window.viewer.manualDriving) {
                                            viewport.focusCurrentCar()
                                            manualInputFocus.forceActiveFocus()
                                        }
                                    }
                                }

                            }

                            ThemedCheckBox {
                                id: takeOverOnInputCheckBox
                                objectName: "takeOverOnInputCheckBox"
                                Layout.preferredWidth: 156
                                Layout.preferredHeight: 42
                                text: qsTr("Take Over on Input")
                                checked: window.viewer.takeOverOnInput
                                enabled: window.viewer.loaded
                                         && !window.viewer.loading
                                         && !window.viewer.manualDriving
                                         && !window.controller.running
                                         && !window.controller.extractingReplayInputs
                                onToggled:
                                    window.viewer.takeOverOnInput = checked

                                font.pixelSize: 11
                            }
                        }
                    }

                    Rectangle {
                        id: checkpointSplitOverlay
                        objectName: "checkpointSplitOverlay"
                        anchors.right: parent.right
                        anchors.rightMargin: 14
                        anchors.top: scriptedTelemetry.bottom
                        anchors.topMargin: 8
                        z: 3
                        width: 198
                        height: Math.max(
                                    0,
                                    Math.min(
                                        310,
                                        playbackDock.y - y - 12,
                                        Math.max(
                                            74,
                                            checkpointSplitHeader.height
                                            + checkpointSplitList
                                                  .contentHeight)))
                        visible: window.viewer.checkpointSplits.length > 0
                                 && !viewport.exportingWhiteboardImage
                        color: AppTheme.viewerOverlay
                        border.width: 1
                        border.color: AppTheme.viewerOverlayBorder
                        radius: 6
                        clip: true

                        Rectangle {
                            id: checkpointSplitHeader
                            objectName: "checkpointSplitHeader"
                            anchors.top: parent.top
                            anchors.left: parent.left
                            anchors.right: parent.right
                            height: 38
                            color: AppTheme.viewerOverlayControl

                            Label {
                                anchors.left: parent.left
                                anchors.leftMargin: 12
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Splits")
                                color: AppTheme.viewerOverlayText
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                            }
                        }

                        ListView {
                            id: checkpointSplitList
                            objectName: "checkpointSplitList"
                            anchors.top: checkpointSplitHeader.bottom
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            model: window.viewer.checkpointSplits
                            boundsBehavior: Flickable.StopAtBounds
                            clip: true

                            onCountChanged: {
                                if (count > 0)
                                    positionViewAtEnd()
                            }

                            ScrollBar.vertical: ScrollBar {
                                policy: checkpointSplitList.contentHeight
                                        > checkpointSplitList.height
                                        ? ScrollBar.AsNeeded
                                        : ScrollBar.AlwaysOff
                            }

                            delegate: Rectangle {
                                required property int index
                                required property var modelData
                                objectName: "checkpointSplitRow_" + index
                                width: checkpointSplitList.width
                                height: 36
                                color: modelData.isFinish
                                       ? (AppTheme.dark
                                          ? "#244b34" : "#d8f2e1")
                                       : "transparent"

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    height: 1
                                    color: AppTheme.viewerOverlayBorder
                                    opacity: 0.5
                                }

                                RowLayout {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 8

                                    Label {
                                        objectName:
                                            "checkpointSplitLabel_" + index
                                        Layout.fillWidth: true
                                        text: modelData.label
                                        color: modelData.isFinish
                                               ? (AppTheme.dark
                                                  ? "#8ee1ac" : "#23683b")
                                               : AppTheme.viewerOverlayText
                                        elide: Text.ElideRight
                                        font.pixelSize: 11
                                        font.weight: modelData.isFinish
                                                     ? Font.DemiBold
                                                     : Font.Normal
                                    }

                                    Label {
                                        objectName:
                                            "checkpointSplitTime_" + index
                                        Layout.alignment: Qt.AlignRight
                                        text: modelData.time
                                        color: modelData.isFinish
                                               ? (AppTheme.dark
                                                  ? "#8ee1ac" : "#23683b")
                                               : AppTheme.viewerOverlayMuted
                                        font.family: "monospace"
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        id: manualDriveStatus
                        objectName: "manualDriveStatus"
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: playbackDock.top
                        anchors.bottomMargin: 8
                        z: 3
                        width: 238
                        height: 36
                        radius: 6
                        visible: window.viewer.manualDriving
                                 && !viewport.exportingWhiteboardImage
                        color: AppTheme.viewerOverlay
                        border.width: 1
                        border.color: AppTheme.viewerOverlayBorder

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 8
                            spacing: 6

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Manual")
                                color: AppTheme.viewerOverlayText
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }

                            Repeater {
                                model: [
                                    { "symbol": "←",
                                      "active": window.viewer.manualLeft },
                                    { "symbol": "↑",
                                      "active":
                                          window.viewer.manualAccelerate },
                                    { "symbol": "↓",
                                      "active": window.viewer.manualBrake },
                                    { "symbol": "→",
                                      "active": window.viewer.manualRight }
                                ]

                                delegate: Rectangle {
                                    required property string symbol
                                    required property bool active
                                    Layout.preferredWidth: 26
                                    Layout.preferredHeight: 24
                                    radius: 4
                                    color: active ? AppTheme.accent
                                                  : AppTheme.viewerOverlayControl
                                    border.width: 1
                                    border.color:
                                        active ? AppTheme.accentBorder
                                               : AppTheme.viewerOverlayBorder

                                    Label {
                                        anchors.centerIn: parent
                                        text: symbol
                                        color: active ? AppTheme.textOnAccent
                                                      : AppTheme.viewerOverlayMuted
                                        font.pixelSize: 13
                                        font.weight: Font.DemiBold
                                    }
                                }
                            }
                        }
                    }

                    Column {
                        anchors.centerIn: parent
                        width: Math.min(parent.width - 60, 430)
                        spacing: 12
                        visible: !window.viewer.loaded
                                 && !viewport.exportingWhiteboardImage

                        BusyIndicator {
                            anchors.horizontalCenter: parent.horizontalCenter
                            running: window.viewer.loading
                            visible: running
                        }

                        Label {
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: window.viewer.loading
                                  ? window.viewer.statusText
                                  : qsTr("Select a replay or challenge and load its map from the settings panel.")
                            color: AppTheme.viewerOverlayText
                            wrapMode: Text.WordWrap
                            font.pixelSize: 16
                        }

                        Label {
                            width: parent.width
                            visible: !window.viewer.loading
                                     && window.viewer.statusText
                                        !== qsTr("No map loaded")
                            horizontalAlignment: Text.AlignHCenter
                            text: window.viewer.statusText
                            color: AppTheme.dark ? "#f0a19e" : "#e19b9b"
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                        }

                    }
                }
            }
        }

        Rectangle {
            id: settingsPanel

            objectName: "settingsPanel"
            SplitView.fillWidth: window.codeEditorExpanded
            SplitView.preferredWidth: window.codeEditorExpanded
                                      ? window.width : 390
            SplitView.minimumWidth: window.codeEditorExpanded ? 0 : 340
            SplitView.maximumWidth: window.codeEditorExpanded
                                    ? window.width : 480
            color: AppTheme.panel

            ScrollView {
                id: settingsScroll

                objectName: "settingsScroll"
                anchors.fill: parent
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                PanelWheelRedirector {
                    id: settingsWheelRedirector
                    objectName: "settingsWheelRedirector"
                    parent: settingsScroll.parent
                    anchors.fill: parent
                    flickable: settingsScroll.contentItem
                }

                ColumnLayout {
                    width: settingsScroll.availableWidth
                    spacing: 14

                    Item {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 12
                    }

                    ColumnLayout {
                        objectName: "packsDirectorySection"
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        spacing: 6

                        Label {
                            text: qsTr("Packs directory")
                            font.weight: Font.Medium
                        }

                        Rectangle {
                            objectName: "autoPacksSuggestion"
                            Layout.fillWidth: true
                            implicitHeight: autoPacksSuggestionLayout.implicitHeight
                                            + 16
                            radius: 8
                            color: AppTheme.accentSoft
                            border.width: 1
                            border.color: AppTheme.accentBorder
                            visible:
                                window.controller.autoDetectedPacksDirectory.length
                                > 0

                            RowLayout {
                                id: autoPacksSuggestionLayout
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 8

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2

                                    Label {
                                        objectName: "autoPacksSuggestionText"
                                        Layout.fillWidth: true
                                        text: qsTr("This location was found automatically and should work. Apply?")
                                        color: AppTheme.dark ? AppTheme.text
                                                          : "#284d35"
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: 11
                                        font.weight: Font.Medium
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: window.controller.autoDetectedPacksDirectory
                                        color: AppTheme.dark ? AppTheme.textMuted
                                                          : "#42654c"
                                        elide: Text.ElideMiddle
                                        font.family: "monospace"
                                        font.pixelSize: 9
                                    }
                                }

                                ThemedButton {
                                    objectName: "applyAutoPacksButton"
                                    text: qsTr("Apply")
                                    enabled: !window.controller.running
                                             && !window.controller.extractingReplayInputs
                                    onClicked:
                                        window.controller.applyAutoDetectedPacksDirectory()
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            TextField {
                                Layout.fillWidth: true
                                text: window.controller.packsDirectory
                                enabled: !window.controller.running
                                         && !window.controller.extractingReplayInputs
                                placeholderText: qsTr("Select installed Packs directory")
                                selectByMouse: true
                                onTextEdited:
                                    window.controller.packsDirectory = text
                            }

                            ThemedButton {
                                text: qsTr("Browse")
                                enabled: !window.controller.running
                                         && !window.controller.extractingReplayInputs
                                onClicked:
                                    window.controller.browseForPacksDirectory()
                            }
                        }

                    }

                    ColumnLayout {
                        id: replaySection

                        objectName: "replaySection"
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        spacing: 6

                        Label {
                            text: qsTr("Map source")
                            font.weight: Font.Medium
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            TextField {
                                objectName: "replayPathField"
                                Layout.fillWidth: true
                                text: window.controller.replayPath
                                enabled: !window.controller.running
                                         && !window.controller.extractingReplayInputs
                                placeholderText: qsTr("Select replay or challenge file")
                                selectByMouse: true
                                onTextEdited: window.controller.replayPath = text
                            }

                            ThemedButton {
                                objectName: "browseReplayButton"
                                text: qsTr("Browse")
                                enabled: !window.controller.running
                                         && !window.controller.extractingReplayInputs
                                onClicked:
                                    window.controller.browseForReplay()
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            ThemedButton {
                                objectName: "loadMapButton"
                                Layout.fillWidth: true
                                text: window.viewer.loading
                                      ? qsTr("Loading map...")
                                      : qsTr("Load map")
                                enabled: !window.viewer.loading
                                         && !window.viewer.manualDriving
                                         && !window.controller.running
                                         && !window.controller.extractingReplayInputs
                                         && window.controller.packsDirectory.length > 0
                                         && window.controller.replayPath.length > 0
                                highlighted: true
                                onClicked: window.viewer.loadMap(
                                    window.controller.packsDirectory,
                                    window.controller.replayPath,
                                    window.controller.simulationBackendId)
                            }

                            ThemedButton {
                                objectName: "extractReplayInputsButton"
                                Layout.fillWidth: true
                                text: window.controller.extractingReplayInputs
                                      ? qsTr("Extracting...")
                                      : qsTr("Extract inputs to script")
                                enabled: window.controller.canExtractReplayInputs
                                         && !window.viewer.loading
                                         && !window.viewer.manualDriving
                                onClicked: {
                                    if (window.controller.baseInputScript.trim().length > 0)
                                        replaceBaseInputScriptDialog.open()
                                    else
                                        window.controller.extractReplayInputs()
                                }
                            }
                        }

                        Label {
                            objectName: "replayInputStatusLabel"
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: window.controller.replayInputStatusText
                            color: text.indexOf(qsTr("failed")) >= 0
                                   || text.indexOf(qsTr("discarded")) >= 0
                                   ? AppTheme.error
                                   : AppTheme.success
                            wrapMode: Text.WordWrap
                            font.pixelSize: 11
                        }
                    }

                    ConfigurationSection {
                        objectName: "baseInputScriptSection"
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        title: qsTr("Base input script")

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Item {
                                Layout.fillWidth: true
                            }

                            ThemedButton {
                                id: copyCurrentRaceInputsButton
                                objectName: "copyCurrentRaceInputsButton"
                                text: qsTr("Copy current race")
                                enabled: window.viewer.canCopyCurrentInputs
                                         && !window.controller.running
                                         && !window.controller.extractingReplayInputs
                                onClicked: {
                                    window.controller.baseInputScript =
                                        window.viewer.currentInputScript()
                                    baseInputScriptArea.forceActiveFocus()
                                }
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr(
                                    "Replace the base input with the selected race through its current time")
                            }
                        }

                        ScrollView {
                            id: baseInputScriptScroll

                            objectName: "baseInputScriptScrollView"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 220
                            clip: true
                            ScrollBar.horizontal.policy:
                                ScrollBar.AsNeeded
                            ScrollBar.vertical.policy:
                                ScrollBar.AsNeeded

                            TextArea {
                                id: baseInputScriptArea

                                objectName: "baseInputScriptTextArea"
                                width: Math.max(
                                    baseInputScriptScroll.availableWidth,
                                    contentWidth + leftPadding + rightPadding)
                                text: window.controller.baseInputScript
                                enabled: !window.controller.running
                                         && !window.controller.extractingReplayInputs
                                selectByMouse: true
                                wrapMode: TextEdit.NoWrap
                                textFormat: TextEdit.PlainText
                                font.family: "monospace"
                                font.pixelSize: 12
                                color: enabled ? AppTheme.text
                                               : AppTheme.disabledText
                                placeholderText: qsTr("0.00 press up")
                                onActiveFocusChanged: {
                                    if (!activeFocus)
                                        window.commitBaseInputScript()
                                }
                                background: Rectangle {
                                    color: enabled ? AppTheme.surface
                                                   : AppTheme.disabledSurface
                                    border.width: 1
                                    border.color:
                                        window.controller.baseInputScriptError.length
                                        > 0 ? AppTheme.error
                                            : baseInputScriptArea.activeFocus
                                              ? AppTheme.focus : AppTheme.border
                                    radius: 6
                                }
                            }
                        }

                        Label {
                            objectName: "baseInputScriptErrorLabel"
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: window.controller.baseInputScriptError
                            color: AppTheme.error
                            wrapMode: Text.WordWrap
                            font.pixelSize: 11
                        }
                    }

                    RowLayout {
                        id: appearanceControls

                        objectName: "appearanceControls"
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Appearance")
                            color: AppTheme.textMuted
                            font.pixelSize: 11
                        }

                        ThemedSwitch {
                            id: darkModeToggle
                            objectName: "darkModeToggle"
                            text: qsTr("Dark mode")
                            checked: window.controller.darkMode
                            onToggled:
                                window.controller.darkMode = checked
                            Accessible.name: qsTr("Dark mode")
                            ToolTip.visible: hovered
                            ToolTip.text: checked
                                          ? qsTr("Use the default light theme")
                                          : qsTr("Use the dark theme")
                        }
                    }

                    TabBar {
                        id: toolTabs

                        objectName: "toolTabs"
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        onCurrentIndexChanged: {
                            if (currentIndex === 1) {
                                if (window.viewer.loaded)
                                    window.viewer.startSimulationDebugger()
                            } else {
                                window.codeEditorExpanded = false
                                window.viewer.stopSimulationDebugger()
                            }
                        }

                        ThemedTabButton {
                            id: bruteforceTabButton
                            objectName: "bruteforceTab"
                            text: qsTr("Bruteforce")

                        }

                        ThemedTabButton {
                            id: codeTabButton
                            objectName: "codeDebuggerTab"
                            text: qsTr("Code")

                        }
                    }

                    ColumnLayout {
                        id: bruteforceTabContent

                        objectName: "bruteforceTabContent"
                        Layout.fillWidth: true
                        visible: toolTabs.currentIndex === 0
                        spacing: 14

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        spacing: 6

                        Label {
                            text: qsTr("Physics backend")
                            font.weight: Font.Medium
                        }

                        StyledComboBox {
                            id: simulationBackendCombo

                            objectName: "simulationBackendCombo"
                            Layout.fillWidth: true
                            model: window.controller.simulationBackendOptions
                            textRole: "label"
                            valueRole: "id"
                            enabled: !window.controller.running
                                     && !window.viewer.loading

                            function synchronizeSelection() {
                                const selected = indexOfValue(
                                    window.controller.simulationBackendId)
                                if (selected >= 0 && currentIndex !== selected)
                                    currentIndex = selected
                            }

                            Component.onCompleted: synchronizeSelection()
                            onModelChanged: Qt.callLater(synchronizeSelection)
                            onActivated: selectedIndex =>
                                window.controller.simulationBackendId =
                                    valueAt(selectedIndex)

                            Connections {
                                target: window.controller

                                function onSimulationBackendIdChanged() {
                                    simulationBackendCombo.synchronizeSelection()
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: window.controller.simulationBackendId
                                  === "cuda"
                                  ? qsTr("Fastest runtime optimized for Stadium, needs a modern NVIDIA GPU and may break compatibility in other environments")
                                  : window.controller.simulationBackendId
                                    === "optimized-cpu"
                                    ? qsTr("Faster runtime optimized for Stadium, may break compatibility in other environments")
                                    : window.controller.simulationBackendId
                                      === "multi-threaded-cpu"
                                      ? qsTr("Runs independent optimized CPU simulations across multiple worker threads")
                                    : qsTr("Broadest compatibility")
                            color: AppTheme.textMuted
                            wrapMode: Text.WordWrap
                            font.pixelSize: 11
                        }

                        SettingTextField {
                            objectName: "simulationHorizonSettings"
                            fieldObjectName: "simulationHorizonField"
                            label: qsTr("Simulation horizon (ms)")
                            value: window.controller.simulationHorizonMs
                            running: window.controller.running
                            minimum: 10
                            maximum: 2147481040
                            dragStep: 1000
                            liveScrub: false
                            onEdited: value =>
                                window.controller.simulationHorizonMs = value
                        }

                        ThemedCheckBox {
                            objectName: "randomizeSeedsOnStartCheckBox"
                            text: qsTr("Randomize modifier seeds on Start")
                            checked: window.controller.randomizeSeedsOnStart
                            enabled: !window.controller.running
                            onToggled:
                                window.controller.randomizeSeedsOnStart =
                                    checked
                        }

                        ThemedCheckBox {
                            objectName: "drawTargetsThroughBlocksCheckBox"
                            text: qsTr("Draw targets through blocks")
                            checked:
                                window.controller.drawTargetsThroughBlocks
                            onToggled:
                                window.controller.drawTargetsThroughBlocks =
                                    checked
                        }

                        SettingTextField {
                            objectName: "cpuWorkerSettings"
                            visible: window.controller.simulationBackendId
                                     === "multi-threaded-cpu"
                            fieldObjectName: "cpuWorkerCountField"
                            label: qsTr("Worker threads")
                            value: window.controller.cpuWorkerCount
                            running: window.controller.running
                            minimum: 1
                            maximum: 256
                            onEdited: value =>
                                window.controller.cpuWorkerCount = value
                        }

                        SettingTextField {
                            objectName: "cudaParallelSampleSettings"
                            visible: window.controller.simulationBackendId
                                     === "cuda"
                            fieldObjectName: "cudaParallelSampleCountField"
                            label: qsTr("Parallel samples at a time")
                            value: window.controller.cudaParallelSampleCount
                            running: window.controller.running
                            minimum: 1
                            onEdited: value =>
                                window.controller.cudaParallelSampleCount = value
                        }

                        ThemedCheckBox {
                            objectName: "cudaCalibrationCheckBox"
                            visible: window.controller.simulationBackendId
                                     === "cuda"
                            text: qsTr("Calibrate for maximum throughput")
                            checked: window.controller.cudaCalibrationEnabled
                            enabled: !window.controller.running
                            onToggled:
                                window.controller.cudaCalibrationEnabled =
                                    checked
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        color: AppTheme.border
                    }

                    ConfigurationSection {
                        objectName: "conditionsSection"
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        title: qsTr("Conditions")

                        ScrollView {
                            id: conditionScriptScroll
                            objectName: "conditionScriptScrollView"
                            Layout.fillWidth: true
                            Layout.preferredHeight: 110
                            clip: true
                            ScrollBar.horizontal.policy: ScrollBar.AsNeeded
                            ScrollBar.vertical.policy: ScrollBar.AsNeeded

                            TextArea {
                                id: conditionScriptArea
                                objectName: "conditionScriptTextArea"
                                width: Math.max(
                                    conditionScriptScroll.availableWidth,
                                    contentWidth + leftPadding + rightPadding)
                                text: window.controller.conditionScript
                                enabled: !window.controller.running
                                selectByMouse: true
                                wrapMode: TextEdit.NoWrap
                                textFormat: TextEdit.PlainText
                                font.family: "monospace"
                                font.pixelSize: 12
                                color: enabled ? AppTheme.text
                                               : AppTheme.disabledText
                                placeholderText: qsTr("kmh(car.speed) >= 200")
                                onTextChanged: {
                                    if (window.controller.conditionScript !== text)
                                        window.controller.conditionScript = text
                                }
                                background: Rectangle {
                                    color: enabled ? AppTheme.surface
                                                   : AppTheme.disabledSurface
                                    border.width: 1
                                    border.color: conditionScriptArea.activeFocus
                                                  ? AppTheme.focus
                                                  : AppTheme.border
                                    radius: 6
                                }
                            }
                        }
                    }

                    ConfigurationSection {
                        objectName: "evaluationSection"
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        title: qsTr("Evaluation")

                        AlgorithmSelector {
                            objectName: "evaluationTargetSelector"
                            Layout.fillWidth: true
                            title: qsTr("Target")
                            comboObjectName: "evaluationTargetCombo"
                            options: window.controller.evaluationTargetOptions
                            selectedId: window.controller.evaluationTargetId
                            controller: window.controller
                            viewer: window.viewer
                            viewport: viewport
                            onSelectionRequested: id =>
                                window.controller.evaluationTargetId = id
                        }
                    }

                    ConfigurationSection {
                        objectName: "modifierSection"
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        title: qsTr("Input modifiers")

                        ModifierComposition {
                            Layout.fillWidth: true
                            controller: window.controller
                            options: window.controller.modifierOptions
                            passes: window.controller.modifierPasses
                        }
                    }

                    ConfigurationSection {
                        objectName: "searchSection"
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        title: qsTr("Search")

                        AlgorithmSelector {
                            objectName: "searchAlgorithmSelector"
                            Layout.fillWidth: true
                            title: qsTr("Algorithm")
                            comboObjectName: "searchAlgorithmCombo"
                            options: window.controller.searchAlgorithmOptions
                            selectedId: window.controller.searchAlgorithmId
                            controller: window.controller
                            onSelectionRequested: id =>
                                window.controller.searchAlgorithmId = id
                        }
                    }

                    ConfigurationSection {
                        objectName: "cudaSessionSpecializationSection"
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        visible: window.controller.simulationBackendId
                                 === "cuda"
                        title: qsTr("CUDA fast mode")

                        ThemedSwitch {
                            objectName: "cudaSessionSpecializationSwitch"
                            Layout.fillWidth: true
                            text: qsTr("Use the faster CUDA kernel")
                            checked: window.controller
                                .cudaSessionSpecializationEnabled
                            enabled: !window.controller.running
                            onToggled: window.controller
                                .cudaSessionSpecializationEnabled = checked
                            Accessible.name: text
                        }

                        Label {
                            objectName: "cudaSessionSpecializationWarning"
                            Layout.fillWidth: true
                            text: (window.controller
                                       .cudaSessionSpecializationEnabled
                                   ? qsTr("Fast mode is on. Its kernel is built once for this map. ")
                                   : qsTr("Fast mode is off. Its kernel will not be built. "))
                                  + qsTr("Fast mode is for normal Stadium runs. It can give wrong results or fail when the run uses stunts, respawns, car resets, or unusual map physics. Turn it off for those runs. Regular CUDA is safer, but slower.")
                            color: AppTheme.textMuted
                            wrapMode: Text.WordWrap
                            font.pixelSize: 11
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        visible: text.length > 0 && !window.controller.running
                        text: window.controller.validationMessage
                        color: AppTheme.error
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        spacing: 8

                        ThemedButton {
                            objectName: "startSearchButton"
                            Layout.fillWidth: true
                            text: qsTr("Start")
                            highlighted: true
                            enabled: window.controller.canStart
                                     && !window.viewer.manualDriving
                            onClicked: window.controller.startSearch()
                        }

                        ThemedButton {
                            objectName: "stopSearchButton"
                            Layout.fillWidth: true
                            text: window.controller.stopping
                                  ? qsTr("Stopping...")
                                  : qsTr("Stop")
                            enabled: window.controller.running
                                     && !window.controller.stopping
                            onClicked: window.controller.stopSearch()
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        color: AppTheme.border
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            text: window.controller.statusText
                            font.weight: Font.Medium
                            wrapMode: Text.WordWrap
                        }

                        RowLayout {
                            id: searchMetricsRow

                            objectName: "searchMetricsRow"
                            Layout.fillWidth: true
                            visible: window.controller.liveMetricsVisible
                            spacing: 6

                            Rectangle {
                                objectName: "iterationsMetricCard"
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                Layout.preferredHeight: 58
                                radius: 7
                                color: AppTheme.surfaceRaised
                                border.width: 1
                                border.color: AppTheme.border
                                clip: true

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    spacing: 2

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Iterations")
                                        color: AppTheme.textMuted
                                        font.pixelSize: 10
                                        horizontalAlignment: Text.AlignHCenter
                                    }

                                    Label {
                                        objectName: "iterationsMetricValue"
                                        Layout.fillWidth: true
                                        text: window.controller.iterationCountText
                                        color: AppTheme.text
                                        font.family: "monospace"
                                        font.pixelSize: 13
                                        font.weight: Font.DemiBold
                                        horizontalAlignment: Text.AlignHCenter
                                        elide: Text.ElideRight
                                    }
                                }
                            }

                            Rectangle {
                                objectName: "throughputMetricCard"
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                Layout.preferredHeight: 58
                                radius: 7
                                color: AppTheme.surfaceRaised
                                border.width: 1
                                border.color: AppTheme.border
                                clip: true

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    spacing: 2

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Throughput")
                                        color: AppTheme.textMuted
                                        font.pixelSize: 10
                                        horizontalAlignment: Text.AlignHCenter
                                    }

                                    Label {
                                        objectName: "throughputMetricValue"
                                        Layout.fillWidth: true
                                        text: window.controller.throughputText.length > 0
                                              ? window.controller.throughputText
                                                    + qsTr(" /s")
                                              : ""
                                        color: AppTheme.text
                                        font.family: "monospace"
                                        font.pixelSize: 13
                                        font.weight: Font.DemiBold
                                        horizontalAlignment: Text.AlignHCenter
                                        elide: Text.ElideRight
                                    }
                                }
                            }

                            Rectangle {
                                objectName: "elapsedMetricCard"
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                Layout.preferredHeight: 58
                                radius: 7
                                color: AppTheme.surfaceRaised
                                border.width: 1
                                border.color: AppTheme.border
                                clip: true

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    spacing: 2

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Elapsed")
                                        color: AppTheme.textMuted
                                        font.pixelSize: 10
                                        horizontalAlignment: Text.AlignHCenter
                                    }

                                    Label {
                                        objectName: "elapsedMetricValue"
                                        Layout.fillWidth: true
                                        text: window.controller.elapsedText
                                        color: AppTheme.text
                                        font.family: "monospace"
                                        font.pixelSize: 12
                                        font.weight: Font.DemiBold
                                        horizontalAlignment: Text.AlignHCenter
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }

                        ProgressBar {
                            Layout.fillWidth: true
                            from: 0
                            to: 1
                            value: window.controller.progressValue
                            indeterminate:
                                window.controller.progressIndeterminate
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: window.controller.resultText
                            wrapMode: Text.WordWrap
                            color: window.controller.statusText
                                           === qsTr("Search failed")
                                   ? AppTheme.error
                                   : AppTheme.text
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: window.controller.bestInputsText.length > 0
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true

                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Best input script")
                                    font.weight: Font.Medium
                                }

                                ThemedButton {
                                    objectName: "copyBestInputsButton"
                                    text: qsTr("Copy all")
                                    onClicked: {
                                        bestInputsArea.selectAll()
                                        bestInputsArea.copy()
                                        bestInputsArea.select(0, 0)
                                    }
                                }
                            }

                            ScrollView {
                                id: bestInputsScroll

                                objectName: "bestInputsScrollView"
                                Layout.fillWidth: true
                                Layout.preferredHeight: 260
                                clip: true
                                ScrollBar.horizontal.policy:
                                    ScrollBar.AsNeeded
                                ScrollBar.vertical.policy:
                                    ScrollBar.AsNeeded

                                TextArea {
                                    id: bestInputsArea

                                    objectName: "bestInputsTextArea"
                                    width: Math.max(
                                        bestInputsScroll.availableWidth,
                                        contentWidth + leftPadding + rightPadding)
                                    text: window.controller.bestInputsText
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: TextEdit.NoWrap
                                    textFormat: TextEdit.PlainText
                                    font.family: "monospace"
                                    font.pixelSize: 12
                                    color: AppTheme.text
                                    background: Rectangle {
                                        color: AppTheme.surface
                                        border.width: 1
                                        border.color: bestInputsArea.activeFocus
                                                      ? AppTheme.focus
                                                      : AppTheme.border
                                        radius: 6
                                    }
                                }
                            }
                        }
                    }

                    }

                    Item {
                        id: simulationDebuggerPanelHost

                        objectName: "simulationDebuggerPanelHost"
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        Layout.preferredHeight: Math.max(
                                                    simulationDebuggerPanel.implicitHeight,
                                                    settingsScroll.height - 185)
                        visible: toolTabs.currentIndex === 1
                                 && !window.codeEditorExpanded

                        SimulationDebuggerPanel {
                            id: simulationDebuggerPanel

                            objectName: "simulationDebuggerPanel"
                            parent: window.codeEditorExpanded
                                    ? settingsPanel
                                    : simulationDebuggerPanelHost
                            anchors.fill: parent
                            z: window.codeEditorExpanded ? 10 : 0
                            visible: toolTabs.currentIndex === 1
                            expanded: window.codeEditorExpanded
                            viewer: window.viewer
                            onExpansionRequested: function(expanded) {
                                window.codeEditorExpanded = expanded
                                if (!expanded) {
                                    Qt.callLater(function() {
                                        settingsScroll.contentItem.contentY =
                                            Math.max(
                                                0,
                                                simulationDebuggerPanelHost.y
                                                - toolTabs.height - 18)
                                    })
                                }
                            }
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 16
                    }
                }
            }
        }
    }
}
