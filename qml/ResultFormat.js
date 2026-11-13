.pragma library

// Result values as History and Current best show them: the unit in the
// header, a column's values with the fewest decimals that still tell them
// apart, trailing zeros dropped, times like the race timer (1:14.04, 6.08, 1.2).
// Kinds (SearchSessionStore columns): "speed" (stored m/s, shown km/h), "time"
// (ms), "finishTime" (ns), "distance" (m), "number", "integer"; here also
// "drift" (mm) and "elapsed" (ms).

function unit(kind) {
    switch (kind) {
    case "speed": return "km/h"
    case "distance": return "m"
    case "drift": return "mm"
    default: return ""
    }
}

function header(label, kind) {
    const u = unit(kind)
    return u.length > 0 ? label + " (" + u + ")" : label
}

// A stored value in the unit it is shown in (times in seconds).
function shown(kind, value) {
    switch (kind) {
    case "speed": return value * 3.6
    case "time": case "elapsed": return value / 1000
    case "finishTime": return value / 1e9
    default: return value
    }
}

// The fewest and the most decimals a kind shows.
function bounds(kind) {
    switch (kind) {
    case "speed": return [2, 6]
    case "time": return [2, 6]
    case "finishTime": return [2, 9]
    case "distance": return [3, 6]
    case "drift": return [2, 4]
    case "integer": return [0, 0]
    case "elapsed": return [1, 1]
    default: return [3, 8]
    }
}

function isValue(value) {
    return value !== undefined && value !== null && value !== "" && isFinite(value)
}

function rounded(value, decimals) {
    return Number(value.toFixed(decimals))
}

// The decimals a column needs (stored values): the kind's fewest, more while two
// different values would look alike or a value that is not zero would show 0.
function decimals(kind, values) {
    const b = bounds(kind)
    const distinct = Array.from(new Set(values.filter(isValue)
                                              .map(v => shown(kind, Number(v)))))
                         .sort((x, y) => x - y)
    const alike = d => {
        for (let i = 1; i < distinct.length; ++i)
            if (rounded(distinct[i], d) === rounded(distinct[i - 1], d)) return true
        return false
    }
    let d = b[0]
    while (d < b[1] && alike(d)) ++d
    for (const v of distinct)
        while (v !== 0 && d < b[1] && rounded(v, d) === 0) ++d
    return d
}

function trimmed(text) {
    if (text.indexOf(".") < 0) return text
    text = text.replace(/0+$/, "")
    return text.endsWith(".") ? text.slice(0, -1) : text
}

// Seconds as the race timer, in its shortest form: 1:02:14.04, 1:14.04, 6.08, 1.2.
function raceTime(seconds, d) {
    const total = rounded(Math.abs(seconds), d)
    const hours = Math.floor(total / 3600)
    const minutes = Math.floor((total - hours * 3600) / 60)
    let text = trimmed((total - hours * 3600 - minutes * 60).toFixed(d))
    if (hours > 0 || minutes > 0) {
        if (text.indexOf(".") === 1 || text.length === 1) text = "0" + text
        text = (hours > 0 ? hours + ":" + (minutes < 10 ? "0" : "") : "") + minutes + ":" + text
    }
    return (seconds < 0 && total > 0 ? "-" : "") + text
}

// A stored value as text with the column's decimals; a dash when there is none.
function format(kind, value, d) {
    if (!isValue(value)) return "—"
    const v = shown(kind, Number(value))
    if (kind === "time" || kind === "finishTime" || kind === "elapsed") return raceTime(v, d)
    return trimmed(v.toFixed(d))
}
