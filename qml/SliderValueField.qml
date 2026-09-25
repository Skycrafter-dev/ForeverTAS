import QtQuick
import QtQuick.Controls

TextField {
    id: control

    property string value: ""
    property real from: 0
    property real to: 100
    property bool maximumEnabled: true
    property bool integer: false
    property int displayDecimals: -1
    property string suffix: ""
    property string accessibleName: ""
    property color fieldColor: AppTheme.surface
    property color fieldDisabledColor: AppTheme.disabledSurface
    property color fieldBorderColor: AppTheme.borderStrong
    property color fieldTextColor: AppTheme.text
    property color fieldDisabledTextColor: AppTheme.disabledText
    readonly property bool exactValueEditor: true
    readonly property bool inputValid: isValidText(text)
    readonly property color effectiveBorderColor:
        validationFailed ? AppTheme.error
                         : activeFocus ? AppTheme.focus : fieldBorderColor
    property bool validationFailed: false

    signal edited(string value)

    implicitWidth: suffix.length > 0 ? 82 : 70
    implicitHeight: 32
    leftPadding: 8
    rightPadding: suffixLabel.visible ? suffixLabel.width + 11 : 8
    horizontalAlignment: TextInput.AlignRight
    selectByMouse: true
    inputMethodHints: Qt.ImhFormattedNumbersOnly
    color: enabled ? fieldTextColor : fieldDisabledTextColor
    font.family: "monospace"
    Accessible.name: accessibleName
    Accessible.description: qsTr("Exact numeric value")

    function parsedText(candidate) {
        const trimmed = candidate.trim()
        if (trimmed.length === 0)
            return NaN
        const expression = control.integer
                ? /^[+-]?\d+$/
                : /^[+-]?(?:\d+(?:\.\d*)?|\.\d+)$/
        if (!expression.test(trimmed))
            return NaN
        return Number(trimmed)
    }

    function isValidText(candidate) {
        const number = parsedText(candidate)
        return Number.isFinite(number)
                && number >= control.from
                && (!control.maximumEnabled || number <= control.to)
    }

    function formatNumber(number) {
        const bounded = control.maximumEnabled
                ? Math.max(control.from, Math.min(control.to, number))
                : Math.max(control.from, number)
        if (control.integer)
            return Math.round(bounded).toString()
        return bounded === 0 ? "0" : bounded.toString()
    }

    function persistedText() {
        const number = Number(control.value)
        const full = Number.isFinite(number)
                ? control.formatNumber(number)
                : control.formatNumber(control.from)
        if (control.activeFocus || control.displayDecimals < 0
            || control.integer || !Number.isFinite(number))
            return full
        return Number(number.toFixed(control.displayDecimals)).toString()
    }

    function synchronizeText(force) {
        if ((force || !control.activeFocus)
                && control.text !== control.persistedText()) {
            control.text = control.persistedText()
        }
    }

    function synchronizeConfiguration() {
        if (!control.activeFocus)
            control.validationFailed = false
        control.synchronizeText(false)
    }

    function commitText() {
        if (!control.enabled || !control.isValidText(control.text)) {
            control.validationFailed = control.enabled
            if (!control.enabled)
                control.synchronizeText(true)
            return false
        }

        const canonical = control.formatNumber(
                    control.parsedText(control.text))
        control.text = canonical
        control.validationFailed = false
        const persisted = Number(control.value)
        if (!Number.isFinite(persisted)
                || persisted !== Number(canonical)) {
            control.edited(canonical)
        }
        return true
    }

    onValueChanged: synchronizeConfiguration()
    onFromChanged: synchronizeConfiguration()
    onToChanged: synchronizeConfiguration()
    onIntegerChanged: synchronizeConfiguration()
    onActiveFocusChanged: synchronizeText(true)
    onEnabledChanged: {
        if (!enabled) {
            validationFailed = false
            synchronizeText(true)
        }
    }
    Component.onCompleted: synchronizeText(true)

    onTextEdited: validationFailed = false
    onAccepted: {
        if (commitText())
            focus = false
    }
    onEditingFinished: {
        if (!commitText()) {
            synchronizeText(true)
            validationFailed = false
        }
    }
    Keys.onEscapePressed: event => {
        validationFailed = false
        synchronizeText(true)
        focus = false
        event.accepted = true
    }

    Label {
        id: suffixLabel

        visible: control.suffix.length > 0
        anchors.right: parent.right
        anchors.rightMargin: 7
        anchors.verticalCenter: parent.verticalCenter
        text: control.suffix
        color: control.enabled
               ? AppTheme.textMuted : control.fieldDisabledTextColor
        font.family: "monospace"
        font.pixelSize: 11
    }

    background: Rectangle {
        radius: 4
        color: control.enabled
               ? control.fieldColor : control.fieldDisabledColor
        border.width: control.activeFocus || control.validationFailed ? 2 : 1
        border.color: control.effectiveBorderColor
    }

    ToolTip.visible: validationFailed
    ToolTip.text: maximumEnabled
                  ? (integer
                     ? qsTr("Enter a whole number from %1 to %2.")
                           .arg(from).arg(to)
                     : qsTr("Enter a number from %1 to %2.")
                           .arg(from).arg(to))
                  : (integer
                     ? qsTr("Enter a whole number of at least %1.").arg(from)
                     : qsTr("Enter a number of at least %1.").arg(from))
}
