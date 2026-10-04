import QtQuick
import ".." as ThemeControls

// Global steering unit preference; every steering field follows it.
SettingCombo {
    label: qsTr("Steering units")
    comboObjectName: "steeringUnitsPreference"
    options: [
        { label: qsTr("Normalized (±1)"), value: "normalized" },
        { label: qsTr("Native (±65536)"), value: "native" }
    ]
    value: ThemeControls.SteeringDisplay.unit
    onSelected: value => ThemeControls.SteeringDisplay.unit = value
}
