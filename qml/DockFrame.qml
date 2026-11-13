import QtQuick
import QtQuick.Controls

// The glass sheet around one group of tabbed panels: header with the tabs,
// float/dock, collapse and close buttons, and resize edges when floating.
// DockSpace creates one per group and moves it with place().
Item {
    id: frame

    property Item space
    property string groupId
    property var tabs: []
    property string currentPanel
    property bool floating: false
    property bool collapsed: false
    property bool dragging: false
    property bool resizing: false
    readonly property bool maximized: space !== null && space.maximizedPanel !== ""
                                      && currentPanel === space.maximizedPanel
    property bool retiring: false

    property real tx: 0
    property real ty: 0
    property real tw: 0
    property real th: 0
    function place(x0, y0, w0, h0) { tx = x0; ty = y0; tw = w0; th = h0 }

    readonly property bool settle: space !== null && space.animating && !dragging && !resizing
                                   && !space.splitting
    x: maximized ? space.area.x : tx
    y: maximized ? space.area.y : ty
    width: maximized ? space.area.width : tw
    height: maximized ? space.area.height : th
    Behavior on x { enabled: frame.settle; SpringAnimation { spring: 3.6; damping: 0.36; epsilon: 0.25 } }
    Behavior on y { enabled: frame.settle; SpringAnimation { spring: 3.6; damping: 0.36; epsilon: 0.25 } }
    Behavior on width { enabled: frame.settle; SpringAnimation { spring: 4.2; damping: 0.42; epsilon: 0.25 } }
    Behavior on height { enabled: frame.settle; SpringAnimation { spring: 4.2; damping: 0.42; epsilon: 0.25 } }

    opacity: retiring ? 0 : 1
    visible: opacity > 0.01
    Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

    function panelTitle(id) {
        const panel = space ? space.panel(id) : null
        return panel ? panel.title : id
    }

    // Clicks and wheel turns on the sheet never reach the race view below.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: frame.space.raise(frame)
        onWheel: wheel => wheel.accepted = true
    }

    Glass {
        anchors.fill: parent
        origin: Qt.point(frame.space.backdropOrigin.x + frame.x * AppTheme.zoom,
                         frame.space.backdropOrigin.y + frame.y * AppTheme.zoom)
        radius: Math.min(AppTheme.panelRadius, frame.height / 2)
        lift: frame.dragging ? 1 : 0
        shadowAlpha: frame.dragging ? 0.58 : frame.floating ? 0.44 : 0.34
        shadowRadius: frame.dragging ? 30 : frame.floating ? 22 : 16
        shadowOffset: frame.dragging ? 16 : frame.floating ? 9 : 6
    }

    // ---- header ---------------------------------------------------------
    Item {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: AppTheme.headerHeight

        MouseArea {
            id: headerArea
            objectName: "dockHeaderDragArea"
            anchors.fill: parent
            preventStealing: true
            cursorShape: frame.dragging ? Qt.ClosedHandCursor : Qt.ArrowCursor
            property point pressPoint
            property string pressedTab
            property bool moved: false
            onPressed: mouse => {
                pressPoint = mapToItem(frame.space, mouse.x, mouse.y)
                const tab = tabRow.childAt(mapToItem(tabRow, mouse.x, mouse.y).x,
                                           mapToItem(tabRow, mouse.x, mouse.y).y)
                pressedTab = tab && tab.panelId !== undefined ? tab.panelId : ""
                moved = false
                frame.space.raise(frame)
                if (pressedTab !== "" && pressedTab !== frame.currentPanel)
                    frame.space.layout.setCurrent(frame.groupId, pressedTab)
            }
            onPositionChanged: mouse => {
                if (!pressed)
                    return
                const p = mapToItem(frame.space, mouse.x, mouse.y)
                if (!moved && Math.hypot(p.x - pressPoint.x, p.y - pressPoint.y) > 5) {
                    moved = true
                    frame.space.beginDrag(frame, pressedTab, pressPoint)
                }
                if (moved)
                    frame.space.updateDrag(p)
            }
            onReleased: mouse => {
                if (moved)
                    frame.space.endDrag(mapToItem(frame.space, mouse.x, mouse.y))
                moved = false
            }
            onCanceled: {
                if (moved)
                    frame.space.endDrag()
                moved = false
            }
            onDoubleClicked: {
                if (frame.floating)
                    frame.space.setCollapsed(frame.groupId, !frame.collapsed)
            }
        }

        Row {
            id: tabRow
            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            width: Math.min(implicitWidth, buttons.x - 12)
            clip: true

            Repeater {
                model: frame.tabs
                Item {
                    readonly property string panelId: modelData
                    readonly property bool active: modelData === frame.currentPanel
                    width: tabLabel.implicitWidth + 20
                    height: AppTheme.headerHeight - 8

                    Gloss {
                        anchors.fill: parent
                        anchors.margins: -3
                        visible: frame.tabs.length > 1
                        fill: parent.active ? Qt.rgba(1, 1, 1, 0.13) : Qt.rgba(1, 1, 1, 0)
                        rimLight: parent.active ? 0.55 : 0
                        radius: 8 * AppTheme.cornerScale
                    }
                    Label {
                        id: tabLabel
                        anchors.centerIn: parent
                        text: frame.panelTitle(modelData)
                        color: parent.active ? AppTheme.text : AppTheme.textMuted
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                }
            }
        }

        Row {
            id: buttons
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 0

            DockHeaderButton {
                objectName: "dockCollapseButton"
                visible: frame.floating
                iconName: frame.collapsed ? "chevron-down" : "minus"
                tip: frame.collapsed ? qsTr("Expand") : qsTr("Collapse")
                onClicked: frame.space.setCollapsed(frame.groupId, !frame.collapsed)
            }
            DockHeaderButton {
                objectName: "dockFloatButton"
                visible: !frame.maximized
                iconName: frame.floating ? "panel-left" : "detach"
                tip: frame.floating ? qsTr("Dock") : qsTr("Float")
                onClicked: frame.space.toggleFloating(frame.groupId)
            }
            DockHeaderButton {
                objectName: "dockCloseButton"
                iconName: "x"
                tip: qsTr("Close %1").arg(frame.panelTitle(frame.currentPanel))
                onClicked: frame.space.closePanel(frame.currentPanel)
            }
        }
    }

    Rectangle {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        height: 1
        color: Qt.rgba(1, 1, 1, 0.08)
        visible: !frame.collapsed
    }

    // ---- resize edges (floating only) ------------------------------------
    component Edge: MouseArea {
        id: edge
        property int edges: 0
        visible: frame.floating && !frame.collapsed && !frame.maximized
        hoverEnabled: true
        preventStealing: true
        z: 5
        cursorShape: edges === (Qt.LeftEdge | Qt.TopEdge) || edges === (Qt.RightEdge | Qt.BottomEdge)
                     ? Qt.SizeFDiagCursor
                     : edges === (Qt.RightEdge | Qt.TopEdge) || edges === (Qt.LeftEdge | Qt.BottomEdge)
                       ? Qt.SizeBDiagCursor
                       : (edges & (Qt.LeftEdge | Qt.RightEdge)) ? Qt.SizeHorCursor : Qt.SizeVerCursor
        property point start
        property rect startRect
        onPressed: mouse => {
            start = mapToItem(frame.space, mouse.x, mouse.y)
            startRect = Qt.rect(frame.tx, frame.ty, frame.tw, frame.th)
            frame.resizing = true
            frame.space.raise(frame)
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            const p = mapToItem(frame.space, mouse.x, mouse.y)
            const dx = p.x - start.x, dy = p.y - start.y
            let l = startRect.x, t = startRect.y
            let r = startRect.x + startRect.width, b = startRect.y + startRect.height
            if (edges & Qt.LeftEdge) l += dx
            if (edges & Qt.RightEdge) r += dx
            if (edges & Qt.TopEdge) t += dy
            if (edges & Qt.BottomEdge) b += dy
            const wanted = Qt.rect(l, t, r - l, b - t)
            frame.space.schedule(function() { edge.resizeTo(wanted) })
        }
        function resizeTo(wanted) {
            const used = frame.space.layout.resizeFloating(frame.groupId, wanted, edges)
            frame.place(used.x, used.y, used.width, used.height)
        }
        onReleased: {
            frame.space.applyPendingMove()
            frame.resizing = false
        }
        onCanceled: {
            frame.space.applyPendingMove()
            frame.resizing = false
        }
    }
    readonly property real edgeSize: 8
    Edge { edges: Qt.LeftEdge; x: -5; y: 12; width: frame.edgeSize; height: frame.height - 24 }
    Edge { edges: Qt.RightEdge; x: frame.width - 3; y: 12; width: frame.edgeSize; height: frame.height - 24 }
    Edge { edges: Qt.TopEdge; x: 12; y: -5; width: frame.width - 24; height: frame.edgeSize }
    Edge { edges: Qt.BottomEdge; x: 12; y: frame.height - 3; width: frame.width - 24; height: frame.edgeSize }
    Edge { edges: Qt.LeftEdge | Qt.TopEdge; x: -4; y: -4; width: 16; height: 16 }
    Edge { edges: Qt.RightEdge | Qt.TopEdge; x: frame.width - 12; y: -4; width: 16; height: 16 }
    Edge { edges: Qt.LeftEdge | Qt.BottomEdge; x: -4; y: frame.height - 12; width: 16; height: 16 }
    Edge { edges: Qt.RightEdge | Qt.BottomEdge; x: frame.width - 12; y: frame.height - 12; width: 16; height: 16 }
}
