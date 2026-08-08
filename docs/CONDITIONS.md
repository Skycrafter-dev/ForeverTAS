# Condition Language Reference

ForeverTAS conditions decide which simulated ticks may be observed by the
selected evaluation target. They do not stop or alter the simulation.

Each enabled editor line is one **tick gate**. Choose AND (every enabled line
must pass) or OR (any enabled line may pass). An editor with no enabled gates
lets every simulated tick reach the target.

The **Simulation horizon** is still the outer time limit. ForeverTAS simulates
from the start through that horizon; at each tick in the target's observation
window it evaluates the condition gates before asking the target to observe the
tick. A false gate skips target observation for that tick only. It does not end
the run or extend the horizon.

## Using the builder

- Type directly in the Tick Gates editor. Use `Ctrl+Space` for completion.
  Completion exposes one hierarchy level at a time: choosing `car` inserts
  `car.` and immediately opens only its direct fields. Typing `car.` does the
  same without requiring a trailing space or another shortcut.
- Use Up/Down to choose a suggestion, Enter or Tab to insert it, and Escape to
  close the popup. Function signatures follow the cursor.
- Hover a recognized symbol or function to see its canonical definition,
  normalized type, unit, and description.
- Typing an opening parenthesis after a recognized function inserts the closing
  parenthesis automatically and leaves the cursor inside, for example `kmh(|)`.
- Select a line number to disable or re-enable that gate. Enabled lines are
  green; disabled lines are red and saved with a leading `//` marker.
- Use the segmented **AND | OR** control above the editor to change how enabled
  lines are combined.
- Completion uses familiar IDE-style badges: `{}` for an object namespace,
  `ƒ` for a function, `E` for an enum constant, `#` for a number, and `→`
  for a vector. Point-target
  values keep their actual vector type instead of inventing a separate target
  type. The pane on the right explains the selected item.
- Invalid lines are highlighted red in the editor and retain a red wave
  underline at the precise error.
- Use **Save file** to store the current conditions under a custom `.txt` name
  and **Load file** to restore one. Condition files are kept separate from
  saved input scripts. Replacing an existing name requires confirmation.

The saved text format is unchanged and remains compatible with existing BfV2
condition scripts.

## Grammar

One line has the form:

```text
scalar-expression comparison scalar-expression
```

Comparisons are `>`, `<`, `>=`, `<=`, and `=`. Arithmetic is `+`, `-`, `*`,
and `/`; parentheses control grouping. Numbers may be signed decimals or use
scientific notation. Names are case-insensitive.

Quoted text is accepted only as an external name inside `variable(...)`, for
example `variable("foo-bar")`. Strings are not first-class expression values
and cannot be compared or used in arithmetic.

There is intentionally no `==`, `!=`, or inline `AND`/`OR` token. Use `=` for
equality and put each comparison on its own line. A line beginning with `//`
is a disabled gate; the editor adds and removes this marker when its line
number is selected.

## Scalar objects

All distance and speed values use metres and seconds; rotations use radians.
Boolean objects produce `1` for true and `0` for false.

| Object | Meaning | Accepted aliases |
|---|---|---|
| `car.x`, `car.y`, `car.z` | Current world position components | `car.position.x/y/z` |
| `car.prev.x`, `car.prev.y`, `car.prev.z` | World position components one tick earlier | `car.prev.position.x/y/z` |
| `car.vel.x`, `car.vel.y`, `car.vel.z` | Current world linear-velocity components | `car.velocity.x/y/z` |
| `car.prev.vel.x/y/z` | World linear-velocity components one tick earlier | `car.prev.velocity.x/y/z` |
| `car.vel.pitch/yaw/roll` | Current angular-velocity components | `car.velocity.pitch/yaw/roll` |
| `car.prev.vel.pitch/yaw/roll` | Angular-velocity components one tick earlier | `car.prev.velocity.pitch/yaw/roll` |
| `car.localvel.x/y/z` | Current car-local velocity components | `car.localvelocity.x/y/z` |
| `car.prev.localvel.x/y/z` | Car-local velocity components one tick earlier | `car.prev.localvelocity.x/y/z` |
| `car.speed` | Current world speed magnitude | - |
| `car.prev.speed` | World speed magnitude one tick earlier | - |
| `car.localspeed` | Current local-speed magnitude | - |
| `car.prev.localspeed` | Local-speed magnitude one tick earlier | - |
| `car.yaw`, `car.pitch`, `car.roll` | Current orientation angles | `car.rotation.yaw/pitch/roll` |
| `car.prev.yaw/pitch/roll` | Orientation angles one tick earlier | `car.prev.rotation.yaw/pitch/roll` |
| `car.freewheel` | Engine is free-wheeling | - |
| `car.lateralcontact` | Car has lateral contact | - |
| `car.sliding` | Car is sliding | `car.is_sliding`, `car.is` |
| `car.gear` | Current gear | - |
| `car.rpm` | Engine RPM | - |
| `car.turning_rate` | Current turning rate | `car.tr` |
| `car.turbo_type` | Active turbo type identifier | `car.tt` |
| `car.turbo_boost_factor` | Active turbo multiplier | `car.tbf` |
| `iterations` | Current search iteration number | - |
| `last_improvement.time` | Search time when the last best result appeared | - |
| `last_restart.time` | Search time when the current search started | - |

For each wheel name `frontleft`, `frontright`, `backleft`, and `backright`, the
following objects are available:

| Wheel object pattern | Meaning | Accepted aliases |
|---|---|---|
| `car.wheels.<wheel>.groundcontact` | Wheel has ground contact (`0` or `1`) | - |
| `car.wheels.<wheel>.is` | Wheel is sliding (`0` or `1`) | `car.wheels.<wheel>.is_sliding` |
| `car.wheels.<wheel>.surface` | Surface-type identifier under the wheel | - |

## Typed constants

Named constants remove the need to remember raw runtime IDs. Completion uses
the left side of a comparison to offer the relevant family: surface fields
offer `surface`, turbo type offers `turbo`, gear offers `gear`, and boolean
fields offer `true` / `false` directly. Constants remain numeric scalar values,
so existing scripts that use numbers continue to work unchanged.

| Family | Constants |
|---|---|
| Boolean | `false = 0`, `true = 1` |
| Gear sentinels | `gear.reverse = -1`, `gear.neutral = 0` |
| Turbo mode | `turbo.inactive = 0`, `turbo.direct = 1`, `turbo.roulette = 2` |

Surface material constants match the runtime `EPlugSurfaceMaterialId` values:

| Value | Constant | Value | Constant |
|---:|---|---:|---|
| 0 | `surface.concrete` | 16 | `surface.asphalt` |
| 1 | `surface.pavement` | 17 | `surface.wet_dirt_road` |
| 2 | `surface.grass` | 18 | `surface.wet_asphalt` |
| 3 | `surface.ice` | 19 | `surface.wet_pavement` |
| 4 | `surface.metal` | 20 | `surface.wet_grass` |
| 5 | `surface.sand` | 21 | `surface.snow` |
| 6 | `surface.dirt` | 22 | `surface.resonant_metal` |
| 7 | `surface.turbo` | 23 | `surface.golf_ball` |
| 8 | `surface.dirt_road` | 24 | `surface.golf_wall` |
| 9 | `surface.rubber` | 25 | `surface.golf_ground` |
| 10 | `surface.sliding_rubber` | 26 | `surface.turbo2` |
| 11 | `surface.test` | 27 | `surface.bumper` |
| 12 | `surface.rock` | 28 | `surface.not_collidable` |
| 13 | `surface.water` | 29 | `surface.free_wheeling` |
| 14 | `surface.wood` | 30 | `surface.turbo_roulette` |
| 15 | `surface.danger` |  |  |

Examples:

```text
car.wheels.frontleft.surface = surface.ice
car.turbo_type = turbo.roulette
car.freewheel = true
car.gear = gear.reverse
```

## Vector objects

Vectors may be passed to `distance`; vector literals use `(x, y, z)`.

| Object | Meaning | Accepted aliases |
|---|---|---|
| `car.pos` | Current world position | `car.position` |
| `car.prev.pos` | World position one tick earlier | `car.prev.position` |
| `car.vel` | Current world linear velocity | `car.velocity` |
| `car.prev.vel` | World linear velocity one tick earlier | `car.prev.velocity` |
| `car.localvel` | Current car-local velocity | `car.localvelocity` |
| `car.prev.localvel` | Car-local velocity one tick earlier | `car.prev.localvelocity` |

## Functions

| Function | Result | Example |
|---|---|---|
| `kmh(number)` | Converts metres per second to kilometres per hour | `kmh(car.speed) >= 200` |
| `deg(number)` | Converts radians to degrees | `deg(car.pitch) < 5` |
| `time_since(number)` | Current search time minus the supplied timestamp | `time_since(last_improvement.time) < 10` |
| `distance(vector, vector)` | Euclidean distance between two vectors | `distance(car.pos, (100, 20, 50)) < 5` |
| `variable(name)` | Looks up an external scalar or vector (`var` is an alias) | `distance(car.pos, variable(bf_target_point)) < 5` |

## Point Target

When **Evaluation -> Target -> Point target** is selected, ForeverTAS exposes
the configured target position as the external vector `bf_target_point`.
External values are read with `variable` (or `var`):

```text
distance(car.pos, variable(bf_target_point)) < 5
kmh(car.speed) > 150
```

In AND mode those two lines mean: evaluate the Point Target only on ticks where
the car is within 5 metres of the configured point **and** moving faster than
150 km/h. In OR mode, either line is enough.
Completion offers `bf_target_point` when Point Target is active and explains
why it is unavailable for other target types.

## Implementation source of truth

`condition_syntax.cpp` owns lexical analysis and the recoverable AST. Runtime
compilation, member completion, replacement ranges, argument types, and hover
documentation consume that tree; they do not rescan partially typed text with
their own dotted-name rules. Incomplete input such as `car.` is represented as
a member node with an empty final segment, so it remains a valid completion
site even while the complete condition line is invalid.

The editor sends `{source, cursor}` to the model as one document state. Every
completion carries a stable ID and document revision. Acceptance happens in
C++ and returns one complete replacement document plus its cursor; stale
suggestions are rejected. QML never removes and inserts token fragments on its
own, which prevents cursor/text signal ordering from leaving random suffixes.

The parser, AST compiler, completion model, and syntax highlighter resolve
names through `src/conditions/condition_language_catalog.cpp`. Additions must
include type, unit, aliases, documentation, insertion text, and a compiling
example there so the language and editor cannot silently drift apart.
