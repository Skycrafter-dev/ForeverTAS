import QtQuick

// A sheet of liquid glass the size of this item: the race view behind it,
// bent at the rim, frosted and darkened in the middle, lit from the top left.
// It has to know where it sits over the backdrop: panels bind `origin`
// directly; popups, which move on their own, set `trackPosition`.
Item {
    id: root

    property Item backdrop: AppTheme.backdrop
    property Item blurred: AppTheme.blurred
    // Top-left of this item in backdrop pixels, and backdrop pixels per item
    // pixel (the UI zoom).
    property point origin: Qt.point(0, 0)
    property real originScale: AppTheme.zoom
    property bool trackPosition: false

    property real radius: AppTheme.panelRadius
    property real bevel: Math.min(18, radius * 0.95)
    property real refraction: AppTheme.edgeBend
    property real dispersion: 0.28
    property real frost: 1.0
    property real tintAmount: AppTheme.darkening
    property color tint: AppTheme.glassTint
    property real saturation: 1.45
    property real specular: 0.95
    property real shadowAlpha: 0.42
    property real shadowRadius: 20
    property real shadowOffset: 8
    property real lift: 0
    // Popups: the panels under the glass show through too, blurred.
    property bool seesPanels: false
    readonly property bool usingPanels: seesPanels && shaded && visible
                                        && AppTheme.panelsBlurred !== null
    onUsingPanelsChanged: AppTheme.panelViewers += usingPanels ? 1 : -1
    Component.onDestruction: {
        if (usingPanels)
            AppTheme.panelViewers -= 1
    }

    readonly property bool shaded: AppTheme.glassAvailable && backdrop !== null
                                   && width > 1 && height > 1
    // The theme's shadow settings scale every glass shadow.
    readonly property real effectiveShadowAlpha: shadowAlpha * AppTheme.shadowStrength
    readonly property real effectiveShadowRadius: Math.max(0.5, shadowRadius * AppTheme.shadowSize)
    readonly property real effectiveShadowOffset: shadowOffset * AppTheme.shadowSize
    readonly property real pad: Math.ceil(effectiveShadowRadius * 2.2)

    FrameAnimation {
        running: root.trackPosition && root.visible && root.backdrop !== null
        onTriggered: {
            const p = root.mapToItem(root.backdrop, 0, 0)
            if (p.x !== root.origin.x || p.y !== root.origin.y)
                root.origin = p
        }
    }

    ShaderEffect {
        visible: root.shaded
        x: -root.pad
        y: -root.pad
        width: root.width + 2 * root.pad
        height: root.height + 2 * root.pad

        property vector2d itemSize: Qt.vector2d(width, height)
        property vector4d shapeRect: Qt.vector4d(root.pad, root.pad, root.width, root.height)
        property vector2d srcOrigin: Qt.vector2d(root.origin.x - root.pad * root.originScale,
                                                 root.origin.y - root.pad * root.originScale)
        property vector2d srcScale: Qt.vector2d(root.originScale, root.originScale)
        property vector2d srcSize: Qt.vector2d(root.backdrop ? root.backdrop.width : 1,
                                               root.backdrop ? root.backdrop.height : 1)
        property real radius: Math.min(root.radius, root.width / 2, root.height / 2)
        property real bevel: Math.min(root.bevel, root.width / 2, root.height / 2)
        property real refraction: root.refraction
        property real dispersion: root.dispersion
        property real frost: root.frost * AppTheme.blur
        property real saturation: root.saturation
        property vector4d tint: Qt.vector4d(root.tint.r, root.tint.g, root.tint.b, root.tintAmount)
        property real lightAngle: -2.05
        property real specular: root.specular
        property real shadowAlpha: root.effectiveShadowAlpha
        property real shadowRadius: root.effectiveShadowRadius
        property real shadowOffset: root.effectiveShadowOffset
        property real lift: root.lift
        property var sharpSource: root.backdrop
        property var blurSource: root.blurred
        property var panelSource: root.usingPanels ? AppTheme.panelsBlurred : root.blurred
        property vector4d panelMap: AppTheme.panelMap
        property real panelMix: root.usingPanels ? 1 : 0

        fragmentShader: "qrc:/glass/shaders/glass.frag.qsb"
    }

    // Without shaders: a plain dark sheet with the same outline.
    Rectangle {
        visible: !root.shaded
        anchors.fill: parent
        radius: Math.min(root.radius, width / 2, height / 2)
        color: Qt.rgba(0.07, 0.09, 0.08, 0.92)
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, 0.14)
    }
}
