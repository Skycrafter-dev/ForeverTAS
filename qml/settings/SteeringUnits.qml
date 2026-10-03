import QtQuick

SettingCombo {
    id: root
    property bool nativeUnits: false
    label: qsTr("Steering units")
    options: [
        { label: qsTr("Normalized (-1..1)"), value: "normalized" },
        { label: qsTr("Native (-65536..65536)"), value: "native" }
    ]
    value: nativeUnits ? "native" : "normalized"
    onSelected: value => nativeUnits = value === "native"

    function displayValue(value) {
        if (!nativeUnits) return String(value)
        const normalized = Number(value)
        return String(Math.sign(normalized) * Math.round(Math.abs(normalized) * 65536))
    }

    function storedValue(value) {
        return nativeUnits ? String(Number(value) / 65536) : String(value)
    }
}
