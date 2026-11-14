import QtQuick

// The race's keys and gamepad buttons: which do what (the user's choice,
// saved), and what a key event in the race view or a button does with them.
QtObject {
    id: controls
    objectName: "raceControls"

    required property var controller
    required property var viewer

    // Keyboards whose letters sit like AZERTY ones: the defaults follow the
    // keys' places (QWERTY Q is AZERTY A).
    readonly property bool azerty: controller.keyboardIsAzerty()
    property int savestateCount: 4
    // action id -> [{ key, modifiers } or { pad }] (pad: an SDL gamepad button)
    property var bindings: ({})
    property int revision: 0

    readonly property var fixedActions: [
        { id: "accelerate", label: qsTr("Accelerate") },
        { id: "brake", label: qsTr("Brake") },
        { id: "left", label: qsTr("Steer left") },
        { id: "right", label: qsTr("Steer right") },
        { id: "respawn", label: qsTr("Respawn") },
        { id: "restart", label: qsTr("Restart (play the inputs from the start)") },
        { id: "pause", label: qsTr("Pause / play") },
        { id: "rewind", label: qsTr("Rewind (held)") },
        { id: "stepBack", label: qsTr("One tick back") },
        { id: "stepForward", label: qsTr("One tick forward") },
        { id: "slower", label: qsTr("Slower") },
        { id: "faster", label: qsTr("Faster") }
    ]
    // Every action, savestates included, in the order shown.
    readonly property var actions: {
        const list = fixedActions.slice()
        for (let i = 1; i <= savestateCount; ++i)
            list.push({ id: "load" + i, label: qsTr("Load savestate %1").arg(i) })
        for (let i = 1; i <= savestateCount; ++i)
            list.push({ id: "save" + i, label: qsTr("Save savestate %1").arg(i) })
        return list
    }

    // SDL_GamepadButton values.
    readonly property var pad: ({ a: 0, b: 1, x: 2, y: 3, back: 4, guide: 5, start: 6, leftStick: 7,
                                  rightStick: 8, leftShoulder: 9, rightShoulder: 10, up: 11, down: 12,
                                  left: 13, right: 14 })
    // The gamepad's own defaults: driving stays on the stick, the triggers, A and B (GamepadInput).
    function defaultPadBindings() {
        const button = b => ({ pad: b })
        return {
            respawn: [button(pad.y)],
            restart: [button(pad.back)],
            pause: [button(pad.start)],
            rewind: [button(pad.leftShoulder)],
            stepBack: [button(pad.left)],
            stepForward: [button(pad.right)],
            slower: [button(pad.down)],
            faster: [button(pad.up)]
        }
    }
    function defaultBindings() {
        const key = k => ({ key: k, modifiers: 0 })
        const result = {
            accelerate: [key(Qt.Key_Up)],
            brake: [key(Qt.Key_Down)],
            left: [key(Qt.Key_Left)],
            right: [key(Qt.Key_Right)],
            respawn: [key(Qt.Key_Return), key(Qt.Key_Backspace)],
            restart: [key(Qt.Key_Delete)],
            pause: [key(Qt.Key_Space)],
            rewind: [key(Qt.Key_R)],
            stepBack: [key(Qt.Key_D)],
            stepForward: [key(Qt.Key_F)],
            slower: [key(azerty ? Qt.Key_A : Qt.Key_Q)],
            faster: [key(Qt.Key_E)]
        }
        const pads = defaultPadBindings()
        for (const id in pads)
            result[id] = result[id].concat(pads[id])
        for (let i = 1; i <= 12; ++i) {
            result["load" + i] = [key(Qt.Key_F1 + i - 1)]
            result["save" + i] = [{ key: Qt.Key_F1 + i - 1, modifiers: Qt.ShiftModifier }]
        }
        return result
    }

    function load() {
        savestateCount = Math.max(1, Math.min(12, Number(controller.uiSetting("savestateCount", 4))))
        const defaults = defaultBindings()
        const stored = String(controller.uiSetting("raceKeys", ""))
        let saved = {}
        try {
            saved = JSON.parse(stored || "{}") || {}
        } catch (error) {
            saved = {}
        }
        const merged = {}
        for (const id in defaults)
            merged[id] = Array.isArray(saved[id]) ? saved[id] : defaults[id]
        // Keys saved before the gamepad had actions: its defaults join them, on the buttons nothing uses.
        const padsAdded = !stored || Boolean(controller.uiSetting("racePadDefaults", false))
        if (!padsAdded) {
            const pads = defaultPadBindings()
            const used = b => Object.values(merged).some(list => list.some(o => sameBinding(o, b)))
            for (const id in pads)
                if (!merged[id].some(b => b.pad !== undefined))
                    merged[id] = merged[id].concat(pads[id].filter(b => !used(b)))
        }
        bindings = merged
        viewer.savestateCount = savestateCount
        ++revision
        // The first launch keeps the keys picked for this keyboard; later
        // ones keep what was set, whatever the keyboard.
        if (!stored || !padsAdded) {
            controller.setUiSetting("racePadDefaults", true)
            save()
        }
    }
    function save() {
        controller.setUiSetting("raceKeys", JSON.stringify(bindings))
        controller.setUiSetting("savestateCount", savestateCount)
        viewer.savestateCount = savestateCount
        ++revision
    }
    function setBinding(action, index, binding) {
        const next = Object.assign({}, bindings)
        const list = (next[action] || []).slice()
        if (binding)
            list[index] = binding
        else
            list.splice(index, 1)
        next[action] = list.filter(b => b)
        // A key or button does one thing: it leaves any other action it was on.
        if (binding) {
            for (const id in next) {
                if (id === action)
                    continue
                next[id] = next[id].filter(b => !sameBinding(b, binding))
            }
        }
        bindings = next
        save()
    }
    function setSavestates(count) {
        savestateCount = Math.max(1, Math.min(12, count))
        save()
    }
    function resetAll() {
        bindings = defaultBindings()
        savestateCount = 4
        save()
    }

    function sameBinding(a, b) {
        if (a.pad !== undefined || b.pad !== undefined)
            return a.pad === b.pad
        return a.key === b.key && a.modifiers === b.modifiers
    }

    readonly property int modifierMask: Qt.ShiftModifier | Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier
    function bindingFromEvent(event) {
        return { key: event.key, modifiers: event.modifiers & modifierMask }
    }
    function keyName(key) {
        const names = {}
        names[Qt.Key_Up] = "↑"; names[Qt.Key_Down] = "↓"
        names[Qt.Key_Left] = "←"; names[Qt.Key_Right] = "→"
        names[Qt.Key_Return] = qsTr("Enter"); names[Qt.Key_Enter] = qsTr("Keypad Enter")
        names[Qt.Key_Delete] = qsTr("Delete"); names[Qt.Key_Backspace] = qsTr("Backspace")
        names[Qt.Key_Space] = qsTr("Space"); names[Qt.Key_Tab] = qsTr("Tab")
        names[Qt.Key_Escape] = qsTr("Esc"); names[Qt.Key_Insert] = qsTr("Insert")
        names[Qt.Key_Home] = qsTr("Home"); names[Qt.Key_End] = qsTr("End")
        names[Qt.Key_PageUp] = qsTr("Page Up"); names[Qt.Key_PageDown] = qsTr("Page Down")
        if (names[key] !== undefined)
            return names[key]
        if (key >= Qt.Key_F1 && key <= Qt.Key_F35)
            return "F" + (key - Qt.Key_F1 + 1)
        if (key >= 0x20 && key < 0x7f)
            return String.fromCharCode(key)
        return "#" + key
    }
    function padName(button) {
        const names = ["A", "B", "X", "Y", qsTr("Back"), qsTr("Guide"), qsTr("Start"), qsTr("Left stick"),
                       qsTr("Right stick"), "LB", "RB", qsTr("D-pad ↑"), qsTr("D-pad ↓"), qsTr("D-pad ←"),
                       qsTr("D-pad →")]
        return button < names.length ? names[button] : qsTr("Button %1").arg(button)
    }
    function bindingText(binding) {
        if (!binding)
            return ""
        if (binding.pad !== undefined)
            return "🎮 " + padName(binding.pad)
        let text = ""
        if (binding.modifiers & Qt.ControlModifier) text += "Ctrl+"
        if (binding.modifiers & Qt.AltModifier) text += "Alt+"
        if (binding.modifiers & Qt.MetaModifier) text += "Meta+"
        if (binding.modifiers & Qt.ShiftModifier) text += "Shift+"
        return text + keyName(binding.key)
    }
    // The first key of an action, as shown in hints.
    function keyText(action) {
        revision
        const list = bindings[action] || []
        return list.length ? bindingText(list[0]) : ""
    }

    // The action a key event is bound to: the exact keys first; a key bound
    // without modifiers also answers with Shift (steering while Shift is
    // held), and a release answers whatever modifiers are left.
    function actionForKey(key, eventModifiers, pressed) {
        const modifiers = eventModifiers & modifierMask
        let loose = ""
        for (const id in bindings) {
            if (!actions.some(a => a.id === id))
                continue
            for (const b of bindings[id]) {
                if (b.key !== key)
                    continue
                if (b.modifiers === modifiers)
                    return id
                if (!pressed || (b.modifiers === 0 && modifiers === Qt.ShiftModifier))
                    loose = loose || id
            }
        }
        // The keypad's Enter is Enter.
        if (!loose && key === Qt.Key_Enter)
            return actionForKey(Qt.Key_Return, eventModifiers, pressed)
        return loose
    }
    function actionFor(event, pressed) {
        return actionForKey(event.key, event.modifiers, pressed)
    }
    // handle() for a key given by its code (tests, on-screen keys).
    function handleKey(key, modifiers, pressed, autoRepeat) {
        return handle({ key: key, modifiers: modifiers, isAutoRepeat: autoRepeat, accepted: false },
                      pressed)
    }

    // The action a gamepad button is bound to.
    function actionForPad(button) {
        for (const id in bindings) {
            if (!actions.some(a => a.id === id))
                continue
            if (bindings[id].some(b => b.pad === button))
                return id
        }
        return ""
    }
    // A gamepad button (GamepadInput.buttonChanged); true when it was bound.
    function handlePad(button, pressed) {
        const action = actionForPad(button)
        if (!action.length)
            return false
        run(action, pressed, false)
        return true
    }

    // Handles a key in the race view; true when it was a race key.
    function handle(event, pressed) {
        const action = actionFor(event, pressed)
        if (!action.length)
            return false
        event.accepted = true
        run(action, pressed, event.isAutoRepeat)
        return true
    }

    function run(action, pressed, autoRepeat) {
        const once = pressed && !autoRepeat
        switch (action) {
        case "accelerate": case "brake": case "left": case "right":
            if (!autoRepeat)
                viewer.setManualInput(action, pressed)
            break
        case "respawn":
            if (once && viewer.manualDriving)
                viewer.respawnManualDrive()
            break
        case "restart":
            if (once)
                viewer.restartRun()
            break
        case "pause":
            if (once)
                viewer.togglePlayback()
            break
        case "rewind":
            if (once)
                viewer.beginRewind()
            else if (!pressed && !autoRepeat)
                viewer.endRewind()
            break
        case "stepBack":
            if (pressed)
                viewer.stepTick(-1)
            break
        case "stepForward":
            if (pressed)
                viewer.stepTick(1)
            break
        case "slower":
            if (once)
                viewer.slower()
            break
        case "faster":
            if (once)
                viewer.faster()
            break
        default:
            if (once && action.startsWith("load"))
                viewer.loadState(Number(action.substring(4)) - 1)
            else if (once && action.startsWith("save"))
                viewer.saveState(Number(action.substring(4)) - 1)
        }
    }

    Component.onCompleted: load()
}
