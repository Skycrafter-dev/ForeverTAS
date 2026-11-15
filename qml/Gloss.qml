import QtQuick

// The glass body of a control on a panel: a tinted fill with a lit rim. It
// draws inside a 3 px inset, so place it with anchors.margins: -3.
Item {
    id: root

    property color fill: AppTheme.control
    property real radius: AppTheme.controlRadius
    property real rimLight: 0.5
    property real glow: 0
    property real shadowAlpha: 0
    property color borderColor: "transparent"
    property real borderWidth: 0

    ShaderEffect {
        anchors.fill: parent
        visible: AppTheme.glassAvailable

        property vector2d itemSize: Qt.vector2d(width, height)
        property real radius: root.radius
        property vector4d fillVec: Qt.vector4d(root.fill.r, root.fill.g, root.fill.b, root.fill.a)
        property real rimLight: root.rimLight
        property real lightAngle: -2.05
        property real glow: root.glow
        property real shadowAlpha: root.shadowAlpha * AppTheme.shadowStrength

        fragmentShader: "qrc:/glass/shaders/gloss.frag.qsb"
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: 3
        radius: root.radius
        color: AppTheme.glassAvailable ? "transparent" : root.fill
        border.width: root.borderWidth > 0 ? root.borderWidth : (AppTheme.glassAvailable ? 0 : 1)
        border.color: root.borderWidth > 0 ? root.borderColor : AppTheme.border
    }
}
