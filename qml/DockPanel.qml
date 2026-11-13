import QtQuick

// One panel's content. DockSpace decides which frame (glass sheet) shows it
// and lays it out in that frame's body; the panel itself only says what it
// is called, whether it has anything to show, and how small it may get.
Item {
    id: panel

    readonly property bool isDockPanel: true
    property string panelId
    property string title
    // False hides the panel and gives its space back (for example the
    // checkpoint splits before a run has any).
    property bool available: true
    // How small the content may get; none by default: a panel can shrink to
    // its header, and what does not fit is cut off.
    property real minimumContentWidth: 0
    property real minimumContentHeight: 0
    property real padding: 0
    // Called instead of closing, for panels whose visibility something
    // else owns (the whiteboard's drawing list).
    property var closeHandler: null

    // Set by DockSpace.
    property Item frame: null
    readonly property bool onTop: frame !== null && frame.currentPanel === panelId
                                  && !frame.collapsed

    visible: onTop && frame.opacity > 0.01
    opacity: frame ? frame.opacity : 0
    x: frame ? frame.x + padding : 0
    y: frame ? frame.y + AppTheme.headerHeight + padding : 0
    width: frame ? Math.max(0, frame.width - 2 * padding) : 0
    height: frame ? Math.max(0, frame.height - AppTheme.headerHeight - 2 * padding) : 0
    z: frame ? frame.z + 0.5 : 0
    clip: true
}
