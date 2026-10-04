pragma Singleton

import QtQuick
import QtCore

// Global steering unit preference. Settings always store normalized values
// (-1..1); "native" shows and accepts the game's -65536..65536 scale, with the
// simulator's half-away-from-zero quantization.
QtObject {
    property alias unit: preferences.unit
    readonly property bool nativeUnits: unit === "native"
    property Settings storage: Settings {
        id: preferences
        category: "steeringDisplay"
        property string unit: "normalized"
    }

    function displayValue(value) {
        if (!nativeUnits || String(value).trim().length === 0) return String(value)
        const normalized = Number(value)
        return String(Math.sign(normalized) * Math.round(Math.abs(normalized) * 65536))
    }

    function storedValue(value) {
        return nativeUnits ? String(Number(value) / 65536) : String(value)
    }
}
