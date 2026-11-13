import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "ResultFormat.js" as ResultFormat

// Search results as a table (History, Current best): copy, the target's
// result columns, Approximate and Drift for approximate searches, Attempts,
// Elapsed, Restart. Rows are SearchSessionStore rows; the live row (the
// restart still running) stays on top, the others sort by a column (best
// result first to begin with). The best row is marked.
ColumnLayout {
    id: root
    property var rows: []
    property var liveRow: null
    property bool sortable: true
    property double selectedRestart: -1
    // row: the index in rows, or -1 for the live row
    signal chosen(int row)
    signal copyRequested(int row)

    property string sortColumn: ""
    property bool sortAscending: true
    spacing: 0

    readonly property var columns: buildColumns(rows, liveRow)
    readonly property var body: buildBody(columns, rows, liveRow, sortColumn, sortAscending)

    function hasLive(row) {
        return row !== null && row !== undefined && row.columns !== undefined
    }

    function buildColumns(rows, liveRow) {
        const source = hasLive(liveRow) ? liveRow : rows.length > 0 ? rows[0] : null
        if (source === null) return []
        const all = rows.slice()
        if (hasLive(liveRow)) all.push(liveRow)
        const result = source.columns ?? []
        const columns = [{ id: "copy" }]
        for (let i = 0; i < result.length; ++i) {
            const c = result[i]
            const objective = c.objective
            columns.push({
                id: "m" + i, label: ResultFormat.header(c.label, c.kind), kind: c.kind,
                better: c.better, tooltip: c.tooltip ?? "",
                value: r => (r.metrics ?? [])[i],
                sortValue: objective !== undefined
                           ? (r => (r.objectiveScores ?? [])[objective])
                           : (r => (r.metrics ?? [])[i])
            })
        }
        if (all.some(r => r.approximate !== undefined)) {
            const first = result.length > 0 ? result[0] : { label: "", kind: "number", better: "" }
            columns.push({
                id: "approximate", label: ResultFormat.header(qsTr("Approximate"), first.kind),
                kind: first.kind, better: first.better, decimalsWith: "m0",
                tooltip: qsTr("What the approximate physics expected for this result"),
                value: r => r.approximate ? r.approximate.score : null
            })
            columns.push({
                id: "drift", label: ResultFormat.header(qsTr("Drift"), "drift"), kind: "drift",
                better: "lower",
                tooltip: qsTr("How far the exact run strayed from the approximate one"),
                value: r => r.approximate ? r.approximate.driftMm : null
            })
        }
        columns.push({ id: "attempts", label: qsTr("Attempts"), kind: "integer",
                       value: r => Number(r.attemptsExact ?? r.attempts),
                       text: r => String(r.attemptsText ?? r.attempts),
                       cellTip: r => String(r.attemptsExactText ?? "") })
        columns.push({ id: "elapsed", label: qsTr("Elapsed"), kind: "elapsed",
                       value: r => r.elapsedMs })
        columns.push({ id: "restart", label: qsTr("Restart"), kind: "integer",
                       value: r => r.restart,
                       text: r => r.running ? qsTr("Running") : String(r.restart) })
        // decimals per column over every row (Approximate with the result)
        for (const column of columns) {
            if (column.kind === undefined) continue
            const values = all.map(column.value)
            if (column.decimalsWith !== undefined)
                values.push(...all.map(columns.find(c => c.id === column.decimalsWith).value))
            column.decimals = ResultFormat.decimals(column.kind, values)
        }
        for (const column of columns) {
            const partner = columns.find(c => c.decimalsWith === column.id)
            if (partner !== undefined) column.decimals = partner.decimals
        }
        return columns
    }

    function compare(column, ascending, a, b) {
        const key = column.sortValue ?? column.value
        const av = Number(key(a)), bv = Number(key(b))
        const aMissing = !isFinite(av), bMissing = !isFinite(bv)
        if (aMissing !== bMissing) return aMissing ? 1 : -1
        if (!aMissing && av !== bv) return (av < bv ? -1 : 1) * (ascending ? 1 : -1)
        const attempts = Number(a.attemptsExact ?? a.attempts) - Number(b.attemptsExact ?? b.attempts)
        return attempts !== 0 ? attempts : Number(a.restart) - Number(b.restart)
    }

    function bestFirst(column) {
        return column.better !== "higher"
    }

    function buildBody(columns, rows, liveRow, sortColumn, sortAscending) {
        if (columns.length === 0) return []
        const sortBy = columns.find(c => c.id === sortColumn) ?? columns[1]
        const ascending = sortColumn === "" ? bestFirst(sortBy) : sortAscending
        const order = rows.map((r, i) => ({ row: r, source: i }))
        order.sort((x, y) => compare(sortBy, ascending, x.row, y.row))
        if (hasLive(liveRow)) order.unshift({ row: liveRow, source: -1 })
        // the best row: by the first result column, when it has a better way
        const first = columns[1]
        let best = null
        if (first !== undefined && first.better !== "" && first.better !== undefined) {
            for (const entry of order) {
                if (!ResultFormat.isValue((first.sortValue ?? first.value)(entry.row))) continue
                if (best === null || compare(first, first.better === "lower", entry.row, best.row) < 0)
                    best = entry
            }
        }
        return order.map(entry => ({
            source: entry.source,
            row: entry.row,
            best: entry === best && order.length > 1,
            cells: columns.map(c => c.id === "copy" ? ""
                                    : c.text !== undefined ? c.text(entry.row)
                                    : ResultFormat.format(c.kind, c.value(entry.row), c.decimals)),
            tips: columns.map(c => c.cellTip !== undefined ? c.cellTip(entry.row) : "")
        }))
    }

    function sortBy(column) {
        if (!sortable || column.id === "copy") return
        const current = sortColumn === "" ? "m0" : sortColumn
        if (current === column.id) {
            const wasAscending = sortColumn === "" ? bestFirst(column) : sortAscending
            sortColumn = column.id
            sortAscending = !wasAscending
        } else {
            sortColumn = column.id
            sortAscending = column.id === "restart" || column.id === "attempts"
                            || column.id === "elapsed" ? true : bestFirst(column)
        }
    }

    function arrow(column) {
        const current = sortColumn === "" ? "m0" : sortColumn
        if (!sortable || current !== column.id) return ""
        const ascending = sortColumn === "" ? bestFirst(column) : sortAscending
        return ascending ? " ↑" : " ↓"
    }

    TextEdit { id: countClipboard; visible: false }
    FontMetrics { id: cellMetrics; font.family: "monospace" }
    FontMetrics { id: headerMetrics; font.weight: Font.Medium }

    function columnWidth(index) {
        const column = columns[index]
        if (column.id === "copy") return 34
        let w = headerMetrics.advanceWidth(column.label + " ↑") + 18
        for (const line of body)
            w = Math.max(w, cellMetrics.advanceWidth(line.cells[index]) + 18)
        return Math.ceil(w)
    }

    readonly property var widths: {
        const result = []
        for (let i = 0; i < columns.length; ++i) result.push(columnWidth(i))
        return result
    }

    RowLayout {
        spacing: 0
        visible: root.columns.length > 0
        Repeater {
            model: root.columns
            Item {
                required property var modelData
                required property int index
                Layout.preferredWidth: root.widths[index] ?? 40
                Layout.preferredHeight: 32
                Label {
                    anchors.fill: parent
                    rightPadding: 9
                    horizontalAlignment: Text.AlignRight
                    verticalAlignment: Text.AlignVCenter
                    font.weight: Font.Medium
                    color: headerArea.containsMouse && root.sortable ? AppTheme.text : AppTheme.textMuted
                    text: parent.modelData.id === "copy" ? ""
                          : parent.modelData.label + root.arrow(parent.modelData)
                }
                MouseArea {
                    id: headerArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: root.sortable && parent.modelData.id !== "copy"
                                 ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: root.sortBy(parent.modelData)
                }
                ToolTip.visible: headerArea.containsMouse && (modelData.tooltip ?? "").length > 0
                ToolTip.text: modelData.tooltip ?? ""
            }
        }
    }

    Repeater {
        model: root.body
        Rectangle {
            id: line
            required property var modelData
            Layout.preferredWidth: cells.implicitWidth
            Layout.preferredHeight: 34
            color: modelData.source >= 0 && root.selectedRestart === Number(modelData.row.restart)
                   ? AppTheme.selection : AppTheme.surface
            Rectangle {
                visible: line.modelData.best
                width: 3
                height: parent.height
                color: AppTheme.accent
            }
            MouseArea {
                anchors.fill: parent
                onClicked: root.chosen(line.modelData.source)
            }
            Row {
                id: cells
                Repeater {
                    model: root.columns.length
                    Item {
                        id: cell
                        required property int index
                        readonly property var column: root.columns[index]
                        width: root.widths[index] ?? 40
                        height: 34
                        ThemedIconButton {
                            visible: cell.column.id === "copy"
                            anchors.centerIn: parent
                            implicitWidth: 26
                            implicitHeight: 26
                            icon.source: "qrc:/icons/copy.svg"
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Copy inputs")
                            onClicked: root.copyRequested(line.modelData.source)
                        }
                        Label {
                            visible: cell.column.id !== "copy"
                            anchors.fill: parent
                            rightPadding: 9
                            verticalAlignment: Text.AlignVCenter
                            horizontalAlignment: Text.AlignRight
                            font.family: "monospace"
                            font.bold: line.modelData.best && cell.column.id === "m0"
                            color: cell.column.id === "restart" && line.modelData.row.running
                                   ? AppTheme.accent : AppTheme.text
                            text: line.modelData.cells[cell.index]
                            MouseArea {
                                id: hover
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: cell.column.id === "attempts"
                                                 ? Qt.RightButton : Qt.NoButton
                                onClicked: countMenu.popup()
                            }
                            GlassMenu {
                                id: countMenu
                                ThemedMenuItem {
                                    text: qsTr("Copy exact count")
                                    onTriggered: {
                                        countClipboard.text = String(
                                            line.modelData.row.attemptsExact
                                            ?? line.modelData.row.attempts)
                                        countClipboard.selectAll()
                                        countClipboard.copy()
                                    }
                                }
                            }
                            ToolTip.visible: hover.containsMouse
                                             && line.modelData.tips[cell.index].length > 0
                            ToolTip.text: line.modelData.tips[cell.index]
                        }
                    }
                }
            }
        }
    }
}
