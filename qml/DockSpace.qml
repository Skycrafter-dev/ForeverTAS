import QtQuick
import QtCore
import ForeverTAS.Viewer 1.0

// Holds the DockPanel children and arranges them, ImGui style: panels dock
// to any edge of the window or of another panel, stack as tabs, or float.
// The race view gets what is left (layout.center). The arrangement itself
// lives in the C++ DockLayout; this item draws it and handles the mouse.
Item {
    id: space

    // The default arrangement for a given area size, in DockLayout's JSON
    // form; used on first run and by View > Reset layout.
    property var defaultLayout: function(width, height) { return null }
    property string savedState
    signal stateSaved(string state)

    property alias layout: dockLayout
    property alias hideAll: dockLayout.hideAll
    // Panel headers (tabs, float, close) show or hide panel by panel: a click
    // on a panel's top edge, or View > Panel headers for all of them.
    // Hidden, the panel's content takes the header's place.
    property var hiddenHeaders: []
    readonly property bool allHeadersShown: hiddenHeaders.length === 0
    function headerShown(panelId) { return hiddenHeaders.indexOf(panelId) < 0 }
    function setHeaderShown(panelId, shown) {
        if (headerShown(panelId) === shown)
            return
        hiddenHeaders = shown ? hiddenHeaders.filter(id => id !== panelId)
                              : hiddenHeaders.concat([panelId])
    }
    function setAllHeadersShown(shown) {
        hiddenHeaders = shown ? [] : panels().map(p => p.panelId)
    }
    // JSON: QSettings turns a one-item list into a plain string
    onHiddenHeadersChanged: AppSettings.setValue("dock/hiddenHeaders", JSON.stringify(hiddenHeaders))
    // The panel filling the whole area (Code's expand button); set by the
    // window. Moving or closing panels asks for it to end.
    property string maximizedPanel: ""
    signal maximizeEnded()
    // Where this item's top-left sits over the race view, in race view px
    // (set by the window, which knows where both are).
    property point backdropOrigin: Qt.point(0, 0)
    readonly property rect area: Qt.rect(AppTheme.margin, AppTheme.margin,
                                         Math.max(0, width - 2 * AppTheme.margin),
                                         Math.max(0, height - 2 * AppTheme.margin))
    // Panels glide to their new place after something the user did (a drop,
    // dock/float, close/open); window resizes and zoom apply at once.
    property bool animating: false
    function animateLayoutChange() {
        animating = true
        animationWindow.restart()
    }
    Timer {
        id: animationWindow
        interval: 600
        onTriggered: space.animating = false
    }
    property bool splitting: false
    property bool ready: false
    property int splitterSlots: 0
    Connections {
        target: dockLayout
        function onGeometryChanged() {
            space.splitterSlots = Math.max(space.splitterSlots, dockLayout.splitters.length)
        }
    }

    property var frames: ({})
    property Item dragFrame: null
    property point grabOffset: Qt.point(0, 0)
    property rect dragRect: Qt.rect(0, 0, 0, 0)
    property var dropTargets: []
    property string hoverTarget: "-"
    property string hoverSide: ""
    property string hitKey: ""
    readonly property real edgeReach: 14

    function panels() {
        const out = []
        for (let i = 0; i < children.length; ++i)
            if (children[i] && children[i].isDockPanel === true)
                out.push(children[i])
        return out
    }
    function panel(id) {
        for (const p of panels())
            if (p.panelId === id)
                return p
        return null
    }
    function isOpen(id) { return dockLayout.revision >= 0 && dockLayout.isOpen(id) }
    function setOpen(id, open) {
        animateLayoutChange()
        dockLayout.setOpen(id, open)
    }
    function toggleFloating(groupId) {
        animateLayoutChange()
        dockLayout.toggleFloating(groupId)
    }
    function setCollapsed(groupId, collapsed) {
        animateLayoutChange()
        dockLayout.setCollapsed(groupId, collapsed)
    }
    function closePanel(id) {
        if (maximizedPanel === id)
            maximizeEnded()
        animateLayoutChange()
        const p = panel(id)
        if (p && p.closeHandler)
            p.closeHandler()
        else
            dockLayout.setOpen(id, false)
    }
    function resetLayout() {
        if (maximizedPanel !== "")
            maximizeEnded()
        dockLayout.defaultState = JSON.stringify(defaultLayout(area.width, area.height))
        animateLayoutChange()
        dockLayout.resetLayout()
    }

    DockLayout {
        id: dockLayout
        area: space.area
        gap: AppTheme.gap
        headerHeight: AppTheme.headerHeight
        onGeometryChanged: space.syncFrames()
        onStateChanged: {
            if (space.ready)
                saveTimer.restart()
        }
    }

    // Saving rewrites the settings file: once the layout has stopped
    // changing, not on every pixel of a resize.
    Timer {
        id: saveTimer
        interval: 500
        onTriggered: space.stateSaved(dockLayout.state)
    }
    function flushState() {
        if (saveTimer.running) {
            saveTimer.stop()
            stateSaved(dockLayout.state)
        }
    }

    // Pointer moves can arrive many times a frame (fast mice); a drag,
    // resize or split applies only the latest one, once per frame.
    property var pendingMove: null
    function schedule(move) {
        pendingMove = move
    }
    function applyPendingMove() {
        const move = pendingMove
        pendingMove = null
        if (move)
            move()
    }
    FrameAnimation {
        running: space.pendingMove !== null
        onTriggered: space.applyPendingMove()
    }

    Component {
        id: frameComponent
        DockFrame {}
    }

    function createFrame(id, rect) {
        const frame = frameComponent.createObject(space, {
            space: space, groupId: id,
            tx: rect.x, ty: rect.y, tw: rect.width, th: rect.height
        })
        frames[id] = frame
        return frame
    }

    function syncFrames() {
        const groups = dockLayout.groups
        const seen = {}
        for (const g of groups) {
            let frame = frames[g.id]
            const fresh = !frame
            if (fresh)
                frame = createFrame(g.id, Qt.rect(g.x, g.y, g.width, g.height))
            frame.retiring = false
            frame.tabs = g.tabs
            frame.currentPanel = g.current
            frame.floating = g.floating
            frame.collapsed = g.collapsed
            frame.z = g.floating ? 100 + g.order : 10
            if (fresh || !frame.dragging)
                frame.place(g.x, g.y, g.width, g.height)
            frame.settled = true
            seen[g.id] = true
        }
        // The dragged group is out of the layout but keeps its frame (which
        // holds the mouse grab), from the start of the drag on.
        const dragId = dockLayout.dragging ? dockLayout.dragGroup : ""
        for (const id in frames) {
            if (seen[id] || id === dragId)
                continue
            retire(id)
        }
        for (const p of panels()) {
            const id = dockLayout.groupOf(p.panelId)
            p.frame = seen[id] || (id !== "" && id === dragId) ? frames[id] || null : null
        }
    }

    function retire(id) {
        const frame = frames[id]
        delete frames[id]
        if (!frame)
            return
        frame.retiring = true
        frame.destroy(200)
    }

    function raise(frame) {
        if (frame.floating && !frame.dragging)
            dockLayout.raise(frame.groupId)
    }

    // ---- dragging ---------------------------------------------------------
    function beginDrag(frame, tabPanel, pressPoint) {
        if (maximizedPanel !== "")
            maximizeEnded()
        const r = Qt.rect(frame.x, frame.y, frame.width, frame.height)
        const singleTab = tabPanel !== "" && frame.tabs.length > 1
        const tabs = singleTab ? [tabPanel] : frame.tabs.slice()
        const current = singleTab ? tabPanel : frame.currentPanel
        // A docked panel keeps its size when floated, within reason.
        const w = frame.floating && !singleTab ? r.width : Math.min(r.width, area.width * 0.55)
        const h = frame.floating && !singleTab ? r.height : Math.min(r.height, area.height * 0.7)
        grabOffset = Qt.point(Math.max(16, Math.min(pressPoint.x - r.x, w - 60)),
                              Math.max(8, Math.min(pressPoint.y - r.y, AppTheme.headerHeight - 8)))
        dragRect = Qt.rect(pressPoint.x - grabOffset.x, pressPoint.y - grabOffset.y, w, h)

        const id = dockLayout.beginDrag(frame.groupId, singleTab ? tabPanel : "", Qt.rect(0, 0, w, h))
        let drag = frames[id]
        if (!drag)
            drag = createFrame(id, r)
        drag.tabs = tabs
        drag.currentPanel = current
        drag.floating = true
        drag.collapsed = false
        drag.retiring = false
        drag.dragging = true
        drag.z = 1000
        dragFrame = drag
        for (const p of panels())
            if (tabs.indexOf(p.panelId) >= 0)
                p.frame = drag
        updateDrag(pressPoint)
    }

    function updateDrag(p) {
        if (dragFrame)
            schedule(function() { moveDrag(p) })
    }
    function moveDrag(p) {
        if (!dragFrame)
            return
        const r = Qt.rect(p.x - grabOffset.x, p.y - grabOffset.y, dragRect.width, dragRect.height)
        updateTargets(p)
        let hit = null
        for (const t of dropTargets)
            if (Math.abs(p.x - t.x) <= t.size / 2 + 6 && Math.abs(p.y - t.y) <= t.size / 2 + 6)
                hit = t
        // Pushing the pointer against a window edge docks there too: the
        // edge targets can be hard to reach past a large panel.
        if (!hit) {
            const side = p.x <= edgeReach ? "left"
                       : p.x >= width - edgeReach ? "right"
                       : p.y <= edgeReach ? "top"
                       : p.y >= height - edgeReach ? "bottom" : ""
            for (const t of dropTargets)
                if (side !== "" && t.edge === true && t.side === side)
                    hit = t
        }
        hoverSide = hit ? hit.side : ""
        hitKey = hit ? hit.target + "|" + hit.side : ""
        dockLayout.previewDrop(hit ? hit.target : "", hit ? hit.side : "")
        // Never outside the window.
        const a = area
        const clamped = Qt.rect(Math.max(a.x, Math.min(a.x + a.width - r.width, r.x)),
                                Math.max(a.y, Math.min(a.y + a.height - r.height, r.y)),
                                r.width, r.height)
        dragRect = hit ? clamped : dockLayout.snapDrag(clamped)
        dragFrame.place(dragRect.x, dragRect.y, dragRect.width, dragRect.height)
    }

    function endDrag(p) {
        if (!dragFrame)
            return
        pendingMove = null
        if (p !== undefined)
            moveDrag(p)
        const frame = dragFrame
        frame.dragging = false
        dragFrame = null
        dropTargets = []
        hoverTarget = "-"
        hoverSide = ""
        hitKey = ""
        animateLayoutChange()
        dockLayout.endDrag(dragRect)
        syncFrames()
    }

    // Which group the pointer is over and the targets it offers: the four
    // sides (split) and the middle (tabs); the race view offers the sides;
    // the window edges are always there.
    function updateTargets(p) {
        let over = null
        const groups = dockLayout.groups
        for (let i = groups.length - 1; i >= 0; --i) {
            const g = groups[i]
            if (p.x >= g.x && p.x <= g.x + g.width && p.y >= g.y && p.y <= g.y + g.height) {
                over = g
                break
            }
        }
        const c = dockLayout.center
        const key = over ? over.id : (p.x >= c.x && p.x <= c.x + c.width
                                      && p.y >= c.y && p.y <= c.y + c.height ? "center" : "")
        if (key === hoverTarget && dropTargets.length > 0)
            return
        hoverTarget = key
        const size = 38, step = 44
        const targets = []
        if (over || key === "center") {
            const r = over ? over : c
            const cx = r.x + r.width / 2, cy = r.y + r.height / 2
            if (!over || !over.floating) {
                targets.push({ target: key, side: "left", x: cx - step, y: cy, size: size })
                targets.push({ target: key, side: "right", x: cx + step, y: cy, size: size })
                targets.push({ target: key, side: "top", x: cx, y: cy - step, size: size })
                targets.push({ target: key, side: "bottom", x: cx, y: cy + step, size: size })
            }
            if (over)
                targets.push({ target: key, side: "center", x: cx, y: cy, size: size })
        }
        const a = area
        targets.push({ target: "", side: "left", x: a.x + 26, y: a.y + a.height / 2, size: size, edge: true })
        targets.push({ target: "", side: "right", x: a.x + a.width - 26, y: a.y + a.height / 2, size: size, edge: true })
        targets.push({ target: "", side: "top", x: a.x + a.width / 2, y: a.y + 26, size: size, edge: true })
        targets.push({ target: "", side: "bottom", x: a.x + a.width / 2, y: a.y + a.height - 26, size: size, edge: true })
        dropTargets = targets
    }

    Component.onCompleted: {
        try {
            const saved = JSON.parse(AppSettings.value("dock/hiddenHeaders", "[]"))
            if (Array.isArray(saved))
                hiddenHeaders = saved.filter(id => typeof id === "string")
        } catch (error) {}
        // before headers were per panel: all hidden or all shown
        if (String(AppSettings.value("dock/headersVisible", true)) === "false") {
            setAllHeadersShown(false)
            AppSettings.remove("dock/headersVisible")
        }
        dockLayout.defaultState = JSON.stringify(defaultLayout(area.width, area.height))
        for (const p of panels()) {
            const update = function() {
                dockLayout.setPanel(p.panelId, p.available,
                                    p.minimumContentWidth + 2 * p.padding,
                                    p.minimumContentHeight + 2 * p.padding + AppTheme.headerHeight)
            }
            update()
            p.availableChanged.connect(update)
            p.minimumContentWidthChanged.connect(update)
            p.minimumContentHeightChanged.connect(update)
        }
        dockLayout.state = savedState
        ready = true
    }

    // ---- where the dragged panel would land ---------------------------------
    Item {
        id: ghost
        z: 900
        readonly property rect r: dockLayout.previewRect
        x: r.x
        y: r.y
        width: r.width
        height: r.height
        opacity: space.dragFrame && r.width > 0 ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: 140 } }
        Behavior on x { enabled: space.dragFrame !== null; SpringAnimation { spring: 4; damping: 0.4 } }
        Behavior on y { enabled: space.dragFrame !== null; SpringAnimation { spring: 4; damping: 0.4 } }
        Behavior on width { enabled: space.dragFrame !== null; SpringAnimation { spring: 4; damping: 0.4 } }
        Behavior on height { enabled: space.dragFrame !== null; SpringAnimation { spring: 4; damping: 0.4 } }

        Glass {
            anchors.fill: parent
            origin: Qt.point(space.backdropOrigin.x + ghost.x * AppTheme.zoom,
                             space.backdropOrigin.y + ghost.y * AppTheme.zoom)
            tintAmount: 0.14
            frost: 0.4
            refraction: AppTheme.edgeBend * 1.6
            dispersion: 0.3
            shadowAlpha: 0
            specular: 1.3
            tint: AppTheme.accent
        }
        Rectangle {
            anchors.fill: parent
            radius: AppTheme.panelRadius
            color: "transparent"
            border.width: 1.5
            border.color: Qt.rgba(0.55, 0.95, 0.72, 0.6)
        }
    }

    // ---- drop targets --------------------------------------------------------
    // Made once and kept (a drag only moves and shows them): building nine
    // of them as each drag starts, and drawing their glass for the first
    // time then, made the first drag stutter.
    readonly property int dropTargetSlots: 9
    property bool warmingUp: true
    Timer {
        // their first frames, drawn all but invisibly once the window is up
        running: space.ready && space.warmingUp
        interval: 1500
        onTriggered: space.warmingUp = false
    }
    Repeater {
        model: space.dropTargetSlots
        Item {
            readonly property var modelData: index < space.dropTargets.length
                ? space.dropTargets[index]
                : { target: "-", side: "center", x: 30, y: 30, size: 38, edge: false }
            readonly property bool used: index < space.dropTargets.length
            visible: used || space.warmingUp
            opacity: used ? 1 : 0.004
            readonly property bool hot: used && space.hitKey === modelData.target + "|" + modelData.side
            z: 1100
            scale: hot ? 1.12 : 1
            Behavior on scale { NumberAnimation { duration: 90 } }
            x: modelData.x - modelData.size / 2
            y: modelData.y - modelData.size / 2
            width: modelData.size
            height: modelData.size
            Gloss {
                anchors.fill: parent
                anchors.margins: -3
                radius: 10 * AppTheme.cornerScale
                fill: Qt.rgba(0.27, 0.72, 0.47, parent.hot ? 0.62 : 0.30)
                rimLight: 0.8
                shadowAlpha: 0.5
            }
            // A little picture of the result: which part of the target the
            // panel would take.
            Rectangle {
                readonly property string side: modelData.side
                readonly property real inset: 7
                x: side === "right" ? parent.width / 2 : inset
                y: side === "bottom" ? parent.height / 2 : inset
                width: side === "left" || side === "right" ? parent.width / 2 - inset : parent.width - 2 * inset
                height: side === "top" || side === "bottom" ? parent.height / 2 - inset : parent.height - 2 * inset
                radius: 3 * AppTheme.cornerScale
                color: Qt.rgba(0.62, 1, 0.78, 0.85)
            }
            Rectangle {
                anchors.fill: parent
                anchors.margins: 4
                radius: 6 * AppTheme.cornerScale
                color: "transparent"
                border.width: modelData.edge === true ? 1 : 1.5
                border.color: Qt.rgba(1, 1, 1, 0.55)
            }
        }
    }

    // ---- split handles ---------------------------------------------------------
    Repeater {
        // never fewer: a drag empties the list for a moment, and the handles
        // would all be made again after it
        model: space.splitterSlots
        Item {
            id: handle
            visible: index < dockLayout.splitters.length
            readonly property var s: dockLayout.splitters[index] || ({ x: 0, y: 0, width: 0, height: 0, row: true })
            z: 50
            x: s.row ? s.x - 3 : s.x + 24
            y: s.row ? s.y + 24 : s.y - 3
            width: s.row ? s.width + 6 : Math.max(0, s.width - 48)
            height: s.row ? Math.max(0, s.height - 48) : s.height + 6
            Gloss {
                anchors.centerIn: parent
                width: handle.s.row ? 11 : 62
                height: handle.s.row ? 62 : 11
                radius: 3 * AppTheme.cornerScale
                fill: Qt.rgba(1, 1, 1, splitArea.pressed ? 0.75 : 0.45)
                rimLight: 0.6
                opacity: splitArea.containsMouse || splitArea.pressed ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 120 } }
            }
            MouseArea {
                id: splitArea
                objectName: "dockSplitHandle"
                anchors.fill: parent
                hoverEnabled: true
                preventStealing: true
                cursorShape: handle.s.row ? Qt.SizeHorCursor : Qt.SizeVerCursor
                property real offset
                onPressed: mouse => {
                    const p = mapToItem(space, mouse.x, mouse.y)
                    offset = (handle.s.row ? p.x - handle.s.x : p.y - handle.s.y)
                    space.splitting = true
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const p = mapToItem(space, mouse.x, mouse.y)
                    const s = handle.s
                    const along = (s.row ? p.x : p.y) - offset - s.start
                    const first = Math.max(s.minimumFirst, Math.min(s.total - s.minimumSecond, along))
                    const id = s.id, ratio = first / Math.max(1, s.total)
                    space.schedule(function() { dockLayout.setRatio(id, ratio) })
                }
                onReleased: {
                    space.applyPendingMove()
                    space.splitting = false
                }
                onCanceled: {
                    space.applyPendingMove()
                    space.splitting = false
                }
            }
        }
    }
}
