import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: root

    property string label
    property var options: []
    property string value
    property bool running: false
    property string comboObjectName: ""
    signal selected(string value)

    Layout.fillWidth: true
    spacing: 12

    function valueIndex() {
        for (let index = 0; index < options.length; ++index) {
            if (options[index].value === value)
                return index
        }
        return -1
    }

    Label {
        Layout.fillWidth: true
        text: root.label
        wrapMode: Text.WordWrap
    }

    StyledComboBox {
        objectName: root.comboObjectName
        Layout.preferredWidth: 160
        model: root.options
        textRole: "label"
        valueRole: "value"
        boundIndex: root.valueIndex()
        enabled: !root.running
        onActivated: selectedIndex =>
            root.selected(valueAt(selectedIndex).toString())
    }
}
