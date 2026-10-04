# Condition and Custom-Target Reference

Generated from the parser's symbol catalog. Regenerate with `forevertas-condition-catalog-tests --write-doc docs/CONDITIONS.md`.

## Setup and Preflight

ForeverTAS runs standalone against a readable installed TMUF Packs directory and a replay or Challenge.Gbx. No in-game plugin is required for local search. The scenario supplies the map, not the control script. An empty base script is valid and means no scripted controls; Extract inputs to script imports replay controls. Exported scripts are text; applying them in a game is a separate workflow and may require an external tool.

Start remains disabled while the validation message identifies invalid paths, script syntax, horizon, target/pass windows, conditions, or an unavailable backend. Correct that message and recheck. Device/runtime errors discovered during loading are reported separately. Choose CPU Reference or CPU Optimized when a GPU route is unavailable; this is not an automatic fallback.

## Syntax

Conditions use one comparison per line; all lines must hold. Comparisons are `> < >= <= =`. Arithmetic is `+ - * /` with parentheses. Names are case-insensitive. Vectors are accepted by `distance`; vector literals contain three numbers, e.g. `(0, 0, 0)`. Boolean fields are numeric 0 or 1. Invalid expressions produce a line/column diagnostic.

Custom targets use `min EXPRESSION`, `max EXPRESSION`, or `target VALUE EXPRESSION`, one per line. Search-clock values are not car-state objectives and are rejected there. Previous values refer to the previous simulation observation, not another candidate. Conditions filter eligible observations; they do not stop the simulation.

`min time.ms` finds the earliest eligible observation in the evaluation window. `time.ms` is the public simulation timestamp in milliseconds, matching the viewer and evaluation windows, with 10 ms tick precision. It is not the input-command timestamp (which has a one-tick offset), wall-clock search time, or a sub-tick crossing time. No eligible observation means no objective value, not a zero-time success.

The Properties and functions control is available in both editors. Ctrl+Space filters it to the token at the caret; selecting an entry inserts the canonical spelling. Aliases remain valid.

## Symbols

| Name | Aliases | Type | Units | Context | Meaning |
| --- | --- | --- | --- | --- | --- |
| `car.position` | `car.pos` | vector | m | conditions and custom targets | Current tick: world position |
| `car.velocity` | `car.vel` | vector | m/s | conditions and custom targets | Current tick: world velocity |
| `car.localvelocity` | `car.localvel` | vector | m/s | conditions and custom targets | Current tick: car-local velocity |
| `car.position.x` | `car.x` | scalar | m | conditions and custom targets | Current tick: world position component |
| `car.velocity.x` | `car.vel.x` | scalar | m/s | conditions and custom targets | Current tick: world velocity component |
| `car.localvelocity.x` | `car.localvel.x` | scalar | m/s | conditions and custom targets | Current tick: car-local velocity component |
| `car.velocity.pitch` | `car.vel.pitch` | scalar | rad/s | conditions and custom targets | Current tick: angular velocity component |
| `car.position.y` | `car.y` | scalar | m | conditions and custom targets | Current tick: world position component |
| `car.velocity.y` | `car.vel.y` | scalar | m/s | conditions and custom targets | Current tick: world velocity component |
| `car.localvelocity.y` | `car.localvel.y` | scalar | m/s | conditions and custom targets | Current tick: car-local velocity component |
| `car.velocity.yaw` | `car.vel.yaw` | scalar | rad/s | conditions and custom targets | Current tick: angular velocity component |
| `car.position.z` | `car.z` | scalar | m | conditions and custom targets | Current tick: world position component |
| `car.velocity.z` | `car.vel.z` | scalar | m/s | conditions and custom targets | Current tick: world velocity component |
| `car.localvelocity.z` | `car.localvel.z` | scalar | m/s | conditions and custom targets | Current tick: car-local velocity component |
| `car.velocity.roll` | `car.vel.roll` | scalar | rad/s | conditions and custom targets | Current tick: angular velocity component |
| `car.speed` |  | scalar | m/s | conditions and custom targets | Current tick: world velocity magnitude |
| `car.localspeed` |  | scalar | m/s | conditions and custom targets | Current tick: car-local velocity magnitude |
| `car.yaw` | `car.rotation.yaw` | scalar | rad | conditions and custom targets | Current tick: yaw angle |
| `car.pitch` | `car.rotation.pitch` | scalar | rad | conditions and custom targets | Current tick: pitch angle |
| `car.roll` | `car.rotation.roll` | scalar | rad | conditions and custom targets | Current tick: roll angle |
| `car.prev.position` | `car.prev.pos` | vector | m | conditions and custom targets | Previous tick: world position |
| `car.prev.velocity` | `car.prev.vel` | vector | m/s | conditions and custom targets | Previous tick: world velocity |
| `car.prev.localvelocity` | `car.prev.localvel` | vector | m/s | conditions and custom targets | Previous tick: car-local velocity |
| `car.prev.position.x` | `car.prev.x` | scalar | m | conditions and custom targets | Previous tick: world position component |
| `car.prev.velocity.x` | `car.prev.vel.x` | scalar | m/s | conditions and custom targets | Previous tick: world velocity component |
| `car.prev.localvelocity.x` | `car.prev.localvel.x` | scalar | m/s | conditions and custom targets | Previous tick: car-local velocity component |
| `car.prev.velocity.pitch` | `car.prev.vel.pitch` | scalar | rad/s | conditions and custom targets | Previous tick: angular velocity component |
| `car.prev.position.y` | `car.prev.y` | scalar | m | conditions and custom targets | Previous tick: world position component |
| `car.prev.velocity.y` | `car.prev.vel.y` | scalar | m/s | conditions and custom targets | Previous tick: world velocity component |
| `car.prev.localvelocity.y` | `car.prev.localvel.y` | scalar | m/s | conditions and custom targets | Previous tick: car-local velocity component |
| `car.prev.velocity.yaw` | `car.prev.vel.yaw` | scalar | rad/s | conditions and custom targets | Previous tick: angular velocity component |
| `car.prev.position.z` | `car.prev.z` | scalar | m | conditions and custom targets | Previous tick: world position component |
| `car.prev.velocity.z` | `car.prev.vel.z` | scalar | m/s | conditions and custom targets | Previous tick: world velocity component |
| `car.prev.localvelocity.z` | `car.prev.localvel.z` | scalar | m/s | conditions and custom targets | Previous tick: car-local velocity component |
| `car.prev.velocity.roll` | `car.prev.vel.roll` | scalar | rad/s | conditions and custom targets | Previous tick: angular velocity component |
| `car.prev.speed` |  | scalar | m/s | conditions and custom targets | Previous tick: world velocity magnitude |
| `car.prev.localspeed` |  | scalar | m/s | conditions and custom targets | Previous tick: car-local velocity magnitude |
| `car.prev.yaw` | `car.prev.rotation.yaw` | scalar | rad | conditions and custom targets | Previous tick: yaw angle |
| `car.prev.pitch` | `car.prev.rotation.pitch` | scalar | rad | conditions and custom targets | Previous tick: pitch angle |
| `car.prev.roll` | `car.prev.rotation.roll` | scalar | rad | conditions and custom targets | Previous tick: roll angle |
| `car.freewheel` |  | boolean | 0 or 1 | conditions and custom targets | Free-wheeling flag |
| `car.lateralcontact` |  | boolean | 0 or 1 | conditions and custom targets | Lateral-contact flag |
| `car.sliding` | `car.is_sliding`, `car.is` | boolean | 0 or 1 | conditions and custom targets | Car sliding flag |
| `car.gear` |  | integer | gear index | conditions and custom targets | Current gear |
| `car.rpm` |  | scalar | native engine value | conditions and custom targets | Engine RPM telemetry |
| `car.turning_rate` | `car.tr` | scalar | native engine value | conditions and custom targets | Turning-rate telemetry |
| `car.turbo_type` | `car.tt` | integer | engine enum | conditions and custom targets | Turbo type |
| `car.turbo_boost_factor` | `car.tbf` | scalar | factor | conditions and custom targets | Turbo boost multiplier |
| `car.cps` |  | integer | checkpoints | conditions and custom targets | Current-lap checkpoint count (not total across laps) |
| `time.ms` |  | scalar | simulation ms | conditions and custom targets | Observed simulation timestamp, matching the viewer and evaluation windows; 10 ms tick precision, not wall-clock or input-command time |
| `iterations` |  | integer | attempts | conditions only | Search iteration counter |
| `last_improvement.time` |  | scalar | wall-clock s | conditions only | Search clock at last improvement; use time_since for elapsed time |
| `last_restart.time` |  | scalar | wall-clock s | conditions only | Search clock at last restart; not simulation time |
| `car.wheels.frontleft.groundcontact` |  | boolean | 0 or 1 | conditions and custom targets | Wheel ground contact |
| `car.wheels.frontleft.is_sliding` | `car.wheels.frontleft.is` | boolean | 0 or 1 | conditions and custom targets | Wheel sliding flag |
| `car.wheels.frontleft.surface` |  | integer | engine enum | conditions and custom targets | Wheel contact surface ID |
| `car.wheels.frontright.groundcontact` |  | boolean | 0 or 1 | conditions and custom targets | Wheel ground contact |
| `car.wheels.frontright.is_sliding` | `car.wheels.frontright.is` | boolean | 0 or 1 | conditions and custom targets | Wheel sliding flag |
| `car.wheels.frontright.surface` |  | integer | engine enum | conditions and custom targets | Wheel contact surface ID |
| `car.wheels.backleft.groundcontact` |  | boolean | 0 or 1 | conditions and custom targets | Wheel ground contact |
| `car.wheels.backleft.is_sliding` | `car.wheels.backleft.is` | boolean | 0 or 1 | conditions and custom targets | Wheel sliding flag |
| `car.wheels.backleft.surface` |  | integer | engine enum | conditions and custom targets | Wheel contact surface ID |
| `car.wheels.backright.groundcontact` |  | boolean | 0 or 1 | conditions and custom targets | Wheel ground contact |
| `car.wheels.backright.is_sliding` | `car.wheels.backright.is` | boolean | 0 or 1 | conditions and custom targets | Wheel sliding flag |
| `car.wheels.backright.surface` |  | integer | engine enum | conditions and custom targets | Wheel contact surface ID |

## Functions

| Function | Aliases | Result | Units | Context | Meaning | Example |
| --- | --- | --- | --- | --- | --- | --- |
| `kmh` |  | scalar | km/h | conditions and custom targets | Convert m/s to km/h | `kmh(car.speed)` |
| `deg` |  | scalar | degrees | conditions and custom targets | Convert radians to degrees | `deg(car.yaw)` |
| `distance` |  | scalar | input vector units | conditions and custom targets | Euclidean distance between two vectors | `distance(car.position, (0,0,0))` |
| `time_since` |  | scalar | wall-clock s | conditions only | Elapsed search time since a search timestamp; not simulation time | `time_since(last_improvement.time)` |
| `variable` | `var` | vector | m | point-target conditions | Active point-target position; unavailable for other targets | `variable("bf_target_point")` |

`variable`/`var` resolve constants supplied by the caller. The application currently supplies only the vector `bf_target_point`, and only to conditions when Point target is selected. Other names are rejected, not silently zero.
