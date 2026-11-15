# Gates: honest section scoring

The `gate-entry` evaluation target scores a section of a run by when the car
crosses a gate, and only counts crossings that meet the gate's limits. The
same gates can be evaluated by `simulate` (parameter `gates`), so a script can
be checked exactly as a search would score it.

## A gate

A gate is a vertical plane across the track through `(x, y, z)`, facing the
heading `dir`. Headings use the telemetry yaw convention: degrees, 0 = +Z,
90 = +X. A gate is usually placed on a reference run: take the car's position
and yaw at the time where the section should end.

| Field | Default | Meaning |
| --- | --- | --- |
| `x`, `y`, `z` | required | Gate centre (world metres). |
| `dir` | required | Direction of travel through the gate (degrees). |
| `hw` | 8 | Half width: sets `lmin = -hw`, `lmax = hw`. |
| `lmin`, `lmax` | -8, 8 | Lateral window, metres from the centre; + = left of travel. |
| `ymin`, `ymax` | -4, 8 | Height window relative to `y`. |
| `vmin`, `vmax` | none | Speed window (km/h) at the crossing instant. |
| `yawmin`, `yawmax` | none | Heading window (absolute degrees, counter-clockwise from `yawmin` to `yawmax`; wrap-safe). Give both or neither. |
| `wheels` | 0 | Minimum wheels on the ground on the tick after the crossing. |

Only a genuine crossing counts: the previous tick is before the plane and the
current tick is on or past it. The crossing time is interpolated between the
two ticks, and the position, speed and heading checked against the limits are
interpolated to the same instant. Crossings of the infinite plane more than
10 m outside the lateral window, or 8 m outside the height window, are treated
as crossings somewhere else on the map and ignored.

## Several gates: waypoints

Gates are given in order. Every gate but the last is a waypoint that must be
passed within its limits, in order; the last gate is the one whose crossing
time is minimised. If a waypoint is crossed outside its limits, the run fails
there. Use waypoints to require "through the seam at 104-112 degrees and at
least 240 km/h, then out of the hairpin at heading 0 +-2" in one search.

Waypoints must lie after the start of the modifier windows: the search only
observes ticks from the earliest mutation time onwards.

## Soft scoring (default)

With `soft` on, every candidate gets a finite score, so a search can make
progress towards a gate that nothing reaches yet:

* valid crossing of the last gate: score = crossing time (simulation ms);
* anything else: score = 10,000,000 + 1,000,000 x (gates not passed)
  + 100 x penalty.

The penalty is either the size of the limit violation of a crossing that
missed (lateral and height metres + `yawWeight` x degrees + `speedWeight` x
km/h + 2 per missing wheel, capped at 4999), or 5000 + the closest approach
in metres to the next gate's opening if it was never crossed. So, at the
same gate, crossing it badly always ranks above not reaching it, and each
passed waypoint outranks both. Any valid result beats any miss. Gate numbers
in miss descriptions are 1-based; the `gate` field in `simulate` output is
0-based.
Results are labelled "Gate entry: ..." or "Gate miss: ..." so a miss is never
mistaken for a solution. With `soft` off, only valid crossings produce a
result, like the volume-entry targets.

## Settings (`configure_search`)

```json
{"target": {"id": "gate-entry", "settings": {
  "gates": [{"x": 516.0, "y": 16.8, "z": 418.7, "dir": 117.6, "hw": 6,
             "yawmin": 104, "yawmax": 112, "vmin": 240},
            {"x": 569.5, "y": 9.6, "z": 468.6, "dir": -0.3, "hw": 4,
             "yawmin": -2.3, "yawmax": 1.7, "vmin": 219}],
  "soft": true, "yawWeight": 0.5, "speedWeight": 0.5}}}
```

`gates` may also be given in the stored compact form, one gate per `|`:
`x=516,y=16.8,z=418.7,dir=117.6,hw=6,yawmin=104,yawmax=112,vmin=240|...`.
Leave `condition_script` empty unless something else needs filtering: the
gate limits replace the hand-written crossing, speed and heading conditions.

The target runs on the CPU backends (reference, optimized and multi-threaded).

## Checking scripts with `simulate`

`simulate` accepts the same `gates` (plus `gate_soft`, `yaw_weight`,
`speed_weight`, `gate_from_ms`). Each result then contains:

* `gates`: for every gate independently, its first crossing near the opening
  (`valid`, `t_ms` on the script clock, `sim_ms`, `speed_kmh`, `yaw`, `lat`,
  `height`, `wheels`, and `penalty`/`violations` when outside its limits);
* `gate_target`: the sequential score the `gate-entry` target would give
  (`valid`, `score`, `t_ms`, `sim_ms`, `description`).

`t_ms` is on the same clock as telemetry and input scripts; `sim_ms` is the
simulation clock used by search results (`sim_ms = t_ms + 10` plus the race
start offset).

## Fast evaluation: batches and snapshots

`simulate` keeps its physics sandbox between calls (it is rebuilt only when
the scenario changes or a longer horizon is needed), so a single run costs
roughly the simulated time and no more. Two further speed-ups:

* **Batches.** Pass `scripts` (up to 256) instead of `script`. They run in
  parallel on cloned sandboxes (`threads`, default: hardware threads - 1, at
  most 32) and come back as `{"results": [...], "threads": n}` in input
  order, each with its `index`. Results are identical to one-at-a-time runs.
  Gates, `horizon_ms` and `from_snapshot` apply to every script.
* **Snapshots.** `snapshot_at_ms: T` (single script, multiple of 10, script
  clock) returns a `snapshot_id` holding the exact physics state at T. Later
  calls with `from_snapshot: id` resume from it instead of simulating from
  zero. The script's inputs at or before T must match the snapshot's
  exactly (the call is rejected otherwise, naming the first difference);
  only later inputs may differ. Results are identical to a full run.
  Snapshots last until the scenario changes; the oldest are dropped beyond 64.

A typical section loop: snapshot just before the section, then evaluate
batches of section variants with `from_snapshot`, `horizon_ms` set just past
the section's exit gate, and `gates` scoring the exit.
