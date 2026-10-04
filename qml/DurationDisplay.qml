pragma Singleton

import QtQuick
import QtCore

QtObject {
    property alias unit: preferences.unit
    property Settings storage: Settings {
        id: preferences
        category: "timeDisplay"
        property string unit: "ms"
    }

    function parse(text) {
        let source = String(text).trim().toLowerCase()
        let milliseconds = NaN
        if (/^\d+\s*(ms)?$/.test(source)) {
            milliseconds = Number(source.replace(/\s*ms$/, ""))
        } else {
            source = source.replace(/\s*s$/, "")
            const match = source.match(/^(\d+(?::\d{1,2}){0,2})(?:[.,](\d{1,3}))?$/)
            if (!match) return { valid: false, error: qsTr("Invalid duration.") }
            const parts = match[1].split(":").map(Number)
            if (parts.slice(1).some(part => part >= 60))
                return { valid: false, error: qsTr("Clock fields must be below 60.") }
            let seconds = 0
            for (const part of parts) seconds = seconds * 60 + part
            const fraction = Number((match[2] ?? "").padEnd(3, "0"))
            milliseconds = seconds * 1000 + fraction
        }
        if (!Number.isSafeInteger(milliseconds) || milliseconds < 0)
            return { valid: false, error: qsTr("Duration is out of range.") }
        if (milliseconds % 10 !== 0)
            return { valid: false, error: qsTr("Time must align to 10 ms.") }
        return { valid: true, milliseconds: String(milliseconds), error: "" }
    }

    // Editable text for a stored whole-tick millisecond value.
    function format(milliseconds) {
        const raw = String(milliseconds)
        if (!/^\d+$/.test(raw)) return raw
        const value = Number(raw)
        if (!Number.isSafeInteger(value) || value % 10 !== 0) return raw
        if (unit === "ms") return raw + " ms"
        const seconds = Math.floor(value / 1000)
        const fraction = String(value % 1000 / 10).padStart(2, "0")
        return unit === "clock"
            ? String(Math.floor(seconds / 60)).padStart(2, "0") + ":"
              + String(seconds % 60).padStart(2, "0") + "." + fraction
            : String(seconds) + "." + fraction + " s"
    }

    // Read-only text for any simulation time, including sub-tick estimates.
    function text(milliseconds) {
        const value = Number(milliseconds)
        if (!Number.isFinite(value)) return String(milliseconds)
        if (Number.isSafeInteger(value) && value >= 0 && value % 10 === 0)
            return format(String(value))
        const trimmed = number => String(Number(number.toFixed(6)))
        if (unit === "ms") return trimmed(value) + " ms"
        if (unit === "seconds") return trimmed(value / 1000) + " s"
        const sign = value < 0 ? "-" : ""
        const absolute = Math.abs(value) / 1000
        const minutes = Math.floor(absolute / 60)
        const seconds = absolute - minutes * 60
        const secondsText = trimmed(seconds)
        return sign + String(minutes).padStart(2, "0") + ":"
            + (seconds < 10 ? "0" : "") + secondsText
    }
}
