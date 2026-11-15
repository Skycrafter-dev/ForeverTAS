pragma Singleton

import QtQuick

// Liquid glass, dark. Panels are glass sheets over the race view, so the
// surface colours here are light washes over the glass rather than solid
// fills; text and accent colours are the old dark theme's.
QtObject {
    // Set once by Main: what the glass shows through (the race view) and a
    // blurred copy of it.
    property Item backdrop: null
    property Item blurred: null
    // The panels, blurred, for popup glass; captured only while such a
    // popup is open (panelViewers counts them). panelMap: where the panel
    // layer sits in backdrop px (x, y, width, height).
    property Item panelsBlurred: null
    property int panelViewers: 0
    property vector4d panelMap: Qt.vector4d(0, 0, 1, 1)
    // False on the software renderer, which has no custom shaders: panels
    // are then drawn as plain dark sheets.
    property bool glassAvailable: true

    // View > Zoom scales every panel, menu and popup (not the race view).
    property real zoom: 1.0
    // View > Glass look.
    property real darkening: defaultDarkening   // how much the glass darkens what is behind it
    property real blur: defaultBlur             // how blurred the race view is through the glass
    property real edgeBend: defaultEdgeBend     // how far the rim bends the race view, px
    readonly property real defaultDarkening: 0.3
    readonly property real defaultBlur: 0.75
    readonly property real defaultEdgeBend: 16
    // Shadows under panels and popups (1 = full), and corner roundness of
    // every glass sheet and control (px at a panel's corner).
    property real shadowStrength: defaultShadowStrength
    property real shadowSize: defaultShadowSize
    property real cornerRadius: defaultCornerRadius
    readonly property real defaultShadowStrength: 0.6
    readonly property real defaultShadowSize: 0.45
    readonly property real defaultCornerRadius: 16
    // Radii elsewhere are given for the default corner and scale with it.
    readonly property real cornerScale: cornerRadius / 16

    readonly property color glassTint: "#0d1210"
    readonly property real panelRadius: cornerRadius
    readonly property real controlRadius: 7 * cornerScale
    readonly property real gap: 8
    readonly property real margin: 10
    readonly property real headerHeight: 32

    readonly property color window: "#0f1311"
    readonly property color panel: "transparent"
    readonly property color panelAlternate: Qt.rgba(1, 1, 1, 0.035)
    readonly property color surface: Qt.rgba(1, 1, 1, 0.06)
    readonly property color surfaceAlternate: Qt.rgba(1, 1, 1, 0.035)
    readonly property color surfaceRaised: Qt.rgba(1, 1, 1, 0.09)
    readonly property color control: Qt.rgba(1, 1, 1, 0.08)
    readonly property color controlHover: Qt.rgba(1, 1, 1, 0.13)
    readonly property color controlPressed: Qt.rgba(1, 1, 1, 0.04)
    readonly property color border: Qt.rgba(1, 1, 1, 0.10)
    readonly property color borderStrong: Qt.rgba(1, 1, 1, 0.18)

    readonly property color text: "#f0f3ef"
    readonly property color textMuted: "#aeb8b0"
    readonly property color textFaint: "#7f8981"
    readonly property color textOnAccent: "#101411"
    readonly property color disabledSurface: Qt.rgba(1, 1, 1, 0.03)
    readonly property color disabledText: "#737b74"

    readonly property color accent: "#45b778"
    readonly property color accentHover: "#58c98c"
    readonly property color accentPressed: "#359962"
    readonly property color accentSoft: Qt.rgba(0.27, 0.72, 0.47, 0.18)
    readonly property color accentBorder: "#4d9369"
    readonly property color focus: "#73d59d"
    readonly property color selection: Qt.rgba(0.27, 0.72, 0.47, 0.26)

    readonly property color success: "#62d090"
    readonly property color successSoft: Qt.rgba(0.38, 0.82, 0.56, 0.16)
    readonly property color warning: "#e9b45b"
    readonly property color warningSoft: Qt.rgba(0.91, 0.71, 0.36, 0.16)
    readonly property color error: "#f07b78"
    readonly property color errorSoft: Qt.rgba(0.94, 0.48, 0.47, 0.16)
    readonly property color info: "#69afe5"
    readonly property color infoSoft: Qt.rgba(0.41, 0.69, 0.90, 0.16)

    // Popups and menus get their own glass; these are the fill behind it
    // and the fallback without shaders.
    readonly property color overlay: Qt.rgba(0.07, 0.09, 0.08, 0.86)
    readonly property color overlayStrong: Qt.rgba(0.07, 0.09, 0.08, 0.94)
    readonly property color overlayBorder: Qt.rgba(1, 1, 1, 0.16)
    // What used to sit straight on the race view now sits on glass panels.
    readonly property color viewerOverlay: "transparent"
    readonly property color viewerOverlayStrong: Qt.rgba(1, 1, 1, 0.05)
    readonly property color viewerOverlayText: "#f0f3ef"
    readonly property color viewerOverlayMuted: "#aeb8b0"
    readonly property color viewerOverlayControl: Qt.rgba(1, 1, 1, 0.08)
    readonly property color viewerOverlayBorder: Qt.rgba(1, 1, 1, 0.12)

    readonly property color tooltipBackground: Qt.rgba(0.08, 0.10, 0.09, 0.95)
    readonly property color tooltipText: "#f0f3ef"
    readonly property color scrim: "#80000000"

    readonly property color codeSurface: Qt.rgba(0, 0, 0, 0.22)
    readonly property color codeAlternate: Qt.rgba(1, 1, 1, 0.025)
    readonly property color codeActive: Qt.rgba(0.27, 0.72, 0.47, 0.20)
    readonly property color codeModified: Qt.rgba(0.27, 0.72, 0.47, 0.13)
    readonly property color codeDraft: Qt.rgba(0.91, 0.71, 0.36, 0.18)
    readonly property color codeLineNumber: "#89938b"
    readonly property color codeBreakpoint: "#f07b78"
}
