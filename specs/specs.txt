Thanks. Here are my adapted specs. Save them in a temporary markdown file.

## Evaluation targets

### 1. Precise finish time

* Require the iteration to finish the race.
* Lower inclusive nanosecond upper bound wins.
* Require ForeverValidator's one-nanosecond finish-transition bracket; never
  fall back to a tick-rounded time.
* Display the upper bound with all nine fractional digits and omit unused
  hour and minute components.
* When the baseline does not finish, the first valid finish becomes the winner.

Note: checkpoint and precise finish-time brute force should function exactly
the same: a finish behaves like the last checkpoint of the race.

### 2. Volume entry time

* User selects a spatial area, like a cuboid.
* Minimize the first valid crossing time, using car position (not collision) inside cuboid.
* Support precise sub-tick crossing timing, through the same interpolation technique as precise checkpoint/finish.
* When the baseline does not reach it, the first iteration to reach it becomes the winner.

### 3. Velocity

* Maximize peak speed within an evaluation window.
* Support total speed or velocity projected along a chosen direction.
* Directional mode may require a minimum alignment with the target direction, expressed as a percentage from -100% to 100%, mapping directly to the cosine of angle difference.
* Select the best state reached anywhere in the window.
* Report the time at which the best velocity occurs.

### 4. Point target

* Minimize distance to a chosen world-space point within an evaluation window.
* Select the best state reached anywhere in the window.
* Report the time at which the lowest distance occurs.

### 5. Pose target

* Match a target position and orientation.
* Allow configurable weighting between position error and rotation error.
* Select the closest valid pose within the evaluation window.
* Report the time at which the lowest error (closest valid pose) occurs.

## Input modification algorithms

### 1. Existing-event perturbation

* Select existing input events within a time window.
* Configure a count range. (for example, between 1 and 15, then the randomness will pick a an amount each iteration)
* Apply bounded timing shifts. (user can put in "0.20 seconds" max time difference, meaning it generates a value between -0.20 and +0.20 and adds it to the timing of the input, but stays clamped within the input mutation window)
* Steering can use either a bounded delta or a new absolute value. (for example, either it adds a random value between X and -X (X specified by the user), or it *sets* a random value between Y1 and Y2 (both defined by the user)).
* Acceleration and brake events can toggle their state. (should be optional, for both brake and accel, in this algorithm)

### 3. Smooth steering deformation

* Apply one or more localized, smooth steering adjustments.
* Configure the active time window.
* Configure deformation count, temporal radius, and amplitude range.
* Blend each adjustment into surrounding ticks rather than creating a sharp discontinuity.

### 4. Input insertion

* Insert new steering, acceleration, or brake segments within a time window.
* Steering may use an absolute value or an offset from the current value, just like Existing-event perturbation
* Binary controls toggle from their current state. (insertions for brake and accel. configurable.)
* Support bounded insertion counts and hold durations. (a certain range of insertion amount. and hold duration meaning it will return to previous state after inserted input is held for X time. hold duration of 0 means no restoring previous input afterwards.)

### 5. Input deletion

* Remove existing input events within a time window.
* Independently enable steering, acceleration, and brake deletion.
* Configure the maximum removal count per channel.

### 6. Modifier composition

* Allow several configured modification algorithm passes to contribute to one iteration.
* Give each pass independent settings.
* Normalize the final event stream after all selected passes (sort, keep last of inputs of same type on same tick)
* Treat an iteration with no effective modification as unchanged.

## Shared mutation contract

All modifiers should (or after all modifiers, in a single pass, but before using for an iteration):

* Be deterministic from the configured seed and iteration index.
* Respect tick alignment and configured time bounds.
* Preserve a valid, chronologically ordered input stream.
* Clamp analog values.
* Report whether an effective change occurred.

## User timeline origin

User-entered and persisted absolute input timeline settings are zero-based.
Every setting key ending in `TimeMs` is translated to simulation time by adding
one physics tick at the component registry boundary; duration keys are not
translated. User `0 ms` therefore targets the first actionable input at
simulation `10 ms` for the 100 Hz search.
