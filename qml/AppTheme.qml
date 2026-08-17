pragma Singleton

import QtQuick

QtObject {
    property bool dark: false

    readonly property color window: dark ? "#171a18" : "#eceeeb"
    readonly property color panel: dark ? "#1d211e" : "#f4f5f2"
    readonly property color panelAlternate: dark ? "#222723" : "#f7f8f5"
    readonly property color surface: dark ? "#292e2a" : "#ffffff"
    readonly property color surfaceAlternate: dark ? "#242925" : "#f3f5f1"
    readonly property color surfaceRaised: dark ? "#303631" : "#eef2ed"
    readonly property color control: dark ? "#343a35" : "#e1e5df"
    readonly property color controlHover: dark ? "#3d453e" : "#d9ded9"
    readonly property color controlPressed: dark ? "#252b27" : "#cbd2cc"
    readonly property color border: dark ? "#4a534b" : "#cbd1c8"
    readonly property color borderStrong: dark ? "#687269" : "#aeb8b0"

    readonly property color text: dark ? "#f0f3ef" : "#202421"
    readonly property color textMuted: dark ? "#aeb8b0" : "#667064"
    readonly property color textFaint: dark ? "#7f8981" : "#7b8278"
    readonly property color textOnAccent: dark ? "#101411" : "#ffffff"
    readonly property color disabledSurface: dark ? "#252925" : "#ecefe9"
    readonly property color disabledText: dark ? "#737b74" : "#92988f"

    readonly property color accent: dark ? "#45b778" : "#26734d"
    readonly property color accentHover: dark ? "#58c98c" : "#35865d"
    readonly property color accentPressed: dark ? "#359962" : "#1d5c3d"
    readonly property color accentSoft: dark ? "#234d35" : "#e7f2eb"
    readonly property color accentBorder: dark ? "#4d9369" : "#8eb49d"
    readonly property color focus: dark ? "#73d59d" : "#2f7b50"
    readonly property color selection: dark ? "#2f5d42" : "#dce9e0"

    readonly property color success: dark ? "#62d090" : "#2f7b50"
    readonly property color successSoft: dark ? "#203f2c" : "#e7f2eb"
    readonly property color warning: dark ? "#e9b45b" : "#9a5b28"
    readonly property color warningSoft: dark ? "#4a361e" : "#fff3d7"
    readonly property color error: dark ? "#f07b78" : "#a23434"
    readonly property color errorSoft: dark ? "#4b2928" : "#f6e3e2"
    readonly property color info: dark ? "#69afe5" : "#315f8f"
    readonly property color infoSoft: dark ? "#263d50" : "#e2edf5"

    readonly property color overlay: dark ? "#f21a1e1b" : "#edffffff"
    readonly property color overlayStrong: dark ? "#f5171a18" : "#f7ffffff"
    readonly property color overlayBorder: dark ? "#667169" : "#9da79f"
    readonly property color viewerOverlay: dark ? "#ed111513" : "#edf4f5f2"
    readonly property color viewerOverlayStrong: dark ? "#f4111513" : "#f7ffffff"
    readonly property color viewerOverlayText: dark ? "#f0f3ef" : "#202421"
    readonly property color viewerOverlayMuted: dark ? "#aeb8b0" : "#667064"
    readonly property color viewerOverlayControl: dark ? "#2b322e" : "#e1e5df"
    readonly property color viewerOverlayBorder: dark ? "#667169" : "#aeb8b0"

    readonly property color tooltipBackground: dark ? "#f0f3ef" : "#202421"
    readonly property color tooltipText: dark ? "#202421" : "#ffffff"
    readonly property color scrim: dark ? "#99000000" : "#66000000"

    readonly property color codeSurface: dark ? "#1a1e1b" : "#fbfcfa"
    readonly property color codeAlternate: dark ? "#202520" : "#f4f6f3"
    readonly property color codeActive: dark ? "#314638" : "#e3f2e7"
    readonly property color codeModified: dark ? "#2c4937" : "#e1f0e5"
    readonly property color codeDraft: dark ? "#4a3d24" : "#fff1cc"
    readonly property color codeLineNumber: dark ? "#89938b" : "#738178"
    readonly property color codeBreakpoint: dark ? "#f07b78" : "#b84141"
}
