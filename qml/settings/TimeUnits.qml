import QtQuick
import ".." as ThemeControls

// Global time unit preference; every simulation-time field follows it.
SettingCombo {
    label: qsTr("Time units")
    comboObjectName: "timeUnitsPreference"
    options: [ { label: qsTr("Milliseconds"), value: "ms" },
               { label: qsTr("Seconds"), value: "seconds" },
               { label: qsTr("Clock"), value: "clock" } ]
    value: ThemeControls.DurationDisplay.unit
    onSelected: value => ThemeControls.DurationDisplay.unit = value
}
