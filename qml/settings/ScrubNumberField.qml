import QtQuick
import QtQuick.Controls
import ".." as ThemeControls

TextField {
    id: control

    property string value: ""
    property string scrubLabel: "↔"
    property real dragStep: 1
    property int decimals: 0
    property bool integer: true
    property real minimum: -Number.MAX_VALUE
    property real maximum: Number.MAX_VALUE
    property real pixelsPerStep: 4
    property bool liveScrub: true
    property var valueParser: null
    property var valueFormatter: null
    readonly property bool scrubbable: true
    readonly property bool scrubbing: scrubArea.pressed

    signal edited(string value)

    leftPadding: 30
    horizontalAlignment: TextInput.AlignRight
    selectByMouse: true

    function clampNumber(number) {
        return Math.max(control.minimum, Math.min(control.maximum, number))
    }

    function formatNumber(number) {
        let numeric = control.clampNumber(number)
        if (valueFormatter) return valueFormatter(numeric)
        if (control.integer)
            return Math.round(numeric).toString()

        let formatted = numeric.toFixed(Math.max(0, control.decimals))
        while (formatted.indexOf(".") >= 0 && formatted.endsWith("0"))
            formatted = formatted.slice(0, -1)
        if (formatted.endsWith("."))
            formatted = formatted.slice(0, -1)
        return formatted === "-0" ? "0" : formatted
    }

    function displayValue() {
        const raw = control.value.toString()
        if (raw.trim().length === 0)
            return raw
        const numeric = parseNumber(raw)
        return Number.isFinite(numeric) ? control.formatNumber(numeric) : raw
    }

    function parseNumber(text) {
        return valueParser ? valueParser(text) : Number(text)
    }

    function synchronizeText() {
        const displayed = control.displayValue()
        if (!control.activeFocus && !control.scrubbing
                && control.text !== displayed) {
            control.text = displayed
        }
    }

    function scrubBySteps(stepCount, modifiers) {
        let step = control.dragStep
        if (!control.integer && (modifiers & Qt.ShiftModifier))
            step *= 0.1
        if (modifiers & Qt.ControlModifier)
            step *= 10

        let proposed = scrubArea.startValue + stepCount * step
        if (control.integer)
            proposed = Math.round(proposed)
        const formatted = control.formatNumber(proposed)
        if (formatted === control.text)
            return
        control.text = formatted
        if (control.liveScrub)
            control.edited(formatted)
    }

    function finishScrub() {
        if (!control.liveScrub && control.text !== control.value) {
            control.edited(control.text)
            return
        }
        control.synchronizeText()
    }

    onValueChanged: synchronizeText()
    onActiveFocusChanged: synchronizeText()
    Component.onCompleted: synchronizeText()

    onEditingFinished: {
        if (text !== displayValue())
            edited(text)
        else
            synchronizeText()
    }

    Label {
        id: scrubHandle

        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 16
        horizontalAlignment: Text.AlignHCenter
        text: control.scrubLabel
        color: control.enabled
               ? ThemeControls.AppTheme.textMuted
               : ThemeControls.AppTheme.disabledText
        font.pixelSize: 12
    }

    MouseArea {
        id: scrubArea

        anchors.fill: scrubHandle
        enabled: control.enabled
        hoverEnabled: true
        preventStealing: true
        cursorShape: Qt.SizeHorCursor

        property real startX: 0
        property real startValue: 0

        onPressed: mouse => {
            startX = mouse.x
            const current = control.parseNumber(control.text)
            const persisted = control.parseNumber(control.value)
            startValue = Number.isFinite(current)
                    ? current
                    : Number.isFinite(persisted) ? persisted : 0
        }

        onPositionChanged: mouse => {
            if (!pressed)
                return
            const steps = Math.round(
                (mouse.x - startX) / control.pixelsPerStep)
            control.scrubBySteps(steps, mouse.modifiers)
        }

        onReleased: control.finishScrub()
        onCanceled: control.synchronizeText()

        ToolTip.visible: containsMouse && !pressed
        ToolTip.text: qsTr("Drag horizontally to adjust")
    }
}
