# Gamepad Driving

Select a controller with the gamepad button in Base / Preferences. Left stick X steers;
right trigger or the south face button accelerates, and left trigger or the
east face button brakes. Trigger activation is digital, matching the existing
drive controls. Steering is rescaled outside the adjustable deadzone and
quantized to the engine's native -65536..65536 range.

Keyboard left/right takes precedence while held; acceleration and braking are
combined with the keyboard. Inputs use the same next-10-ms-tick recorder as
keyboard driving, including takeover. Copied inputs can be replayed or used as
the search baseline. Focus loss, device removal and device changes release
gamepad controls. Returning focus or reconnecting requires neutral controls
before driving resumes. Device identity includes its connection path; selecting
again may be necessary after changing USB ports.

SDL 3.2 or newer supplies controller mapping and hotplug support. CMake uses an
installed SDL3 or builds the pinned 3.2.30 revision statically. Windows installs
SDL3.dll for a shared SDL build; Linux AppImage dependency collection includes
shared SDL automatically. No SDL video window or rendering backend is used.
See the upstream [SDL CMake guide](https://wiki.libsdl.org/SDL3/README-cmake)
and [gamepad polling API](https://wiki.libsdl.org/SDL3/SDL_UpdateGamepads).

Automated tests use an SDL virtual gamepad, actual map simulation and recorded
script replay. Physical controllers and native Windows/macOS execution require
platform testing; SDL mapping coverage depends on the installed SDL version.
