# Bug fixes 4.1, second batch (feat41-bugs2)

Five user reports. What was found, what changed, how it was tested. Nothing here was run on the reporters' hardware (an RSP1A on a Pi 3B+,
a Windows 10 PC with a Xiegu G90, a Raspberry Pi 5); the CI runs are listed at the end.

## #496: OnAir Web does nothing when Play is pressed

Cause: `onair-server --web --web-lan` without `--autostart` does not start the receiver; the user's `/api/v1/status` shows
`"running":false,"wanted":false,"error":""`, so the SDRplay was never asked to start. The web page joined with a capture rate of 0, its tune
was answered "the receiver is not running" (the red line in the screenshot), and when a capture came later the page never tuned again, so it
stayed at 0 Hz with no sound. The SDRplay native path on ARM was not reached at all.

Changes:
- `WebHub::setReceiverControl`: a tune while no samples come starts the receiver when the listener may tune (`web_control`) and
  `web_listener_start` is on (default). The start runs on its own thread; the page gets `not_running` "starting it now", a failed start goes
  to every listener with the radio's reason ("could not start: ..."), a second press while it starts does not start it twice.
- Not allowed to tune, or `web_listener_start=0`: the page is told that the owner starts it (`/app` Start, `--autostart`).
- The page (web/onair-web/app.js) tunes again when the first `capture` arrives, and pressing Listen on a stopped receiver asks for it.
- New error code `not_running` (docs/ONAIR-WEB-PROTOCOL.md), new key `web_listener_start` (docs/ONAIR-WEB.md).
- Test: tests/server/test_web_hub.cpp `testStoppedReceiver`: no starter, a failing start (both listeners see the SDRplay error, one start),
  a working start (capture, tune, a 1 kHz USB tone heard), `web_control=nobody` and `web_listener_start=0` (no start).

## #495: Hamlib missing in the Windows release

Cause: tools/package/make_windows_cross.sh turned Hamlib on only when the Windows build prefix held a Hamlib DLL; the release prefix has none,
so `DECT2_HAVE_HAMLIB` was off and "Add Hamlib rig..." and the DLL search were compiled out.

Changes:
- core/src/source/hamlib_api.h: the Hamlib calls OnAir uses, from `<hamlib/rig.h>` or (new `DECT2_HAMLIB_DYNAMIC`) loaded at run time from
  `libhamlib-4.dll` / `libhamlib.so.4` / `libhamlib.4.dylib`, searched like the radio drivers (program folder, OnAir codecs folders, PATH).
  The declarations follow Hamlib 4.7.2's rig.h (types, enum values, the leading fields of `struct rig_caps`).
- CMake: on Windows without Hamlib at build time the run-time mode is chosen by itself; the cross script always passes Hamlib on.
- The entry is shown; without the DLL its window says what to copy where and looks again every 5 s. The log says
  `Hamlib: <path> loaded at run time`. docs/OPTIONAL-LIBRARIES.md and Settings > Optional libraries list it.
- Tests: test_hamlib_rig / hamlib_dial / rig_setup / rig_control run the dummy rig through the loaded library, or check the clear
  "not found" path when it is absent. quick.yml has a new `packages` input for the extra packages (libhamlib-dev, MSYS2 hamlib).

## #491: the tutorial cannot be skipped on a fresh install

From the video: on a fresh 3.9 install with no radio both the first-run tour (1 / 11) and "Get me listening" (1 / 3) opened at once; the
wizard was drawn over the tour and the tour's Next and Skip did nothing. An install that came from 3.0 has a radio remembered, so "Get me
listening" never opened there.

Change: one guide at a time (`tour::guideInFront`: setup, tour, Get me listening, lesson card). The others stay open and come when the one in
front closes; Skip closes the tour and saves `wizardDone` as before. Test: tests/app/test_tour.cpp. Not reproduced on Windows itself.

## #488: audio stays on the old frequency after a frequency change

Found: "Hear this channel" (the audio monitor, used in every mode without its own sound) took its channel from the radio's dial only. A click
on the spectrum, or a frequency typed in the big readout inside the captured band, moves only the decode (`Engine::setDecodeShiftHz`): the
marker and the mode's receiver moved, the monitor kept playing the old frequency until stop and start (start puts the dial on the frequency).

Change: `monitorShiftHz()` adds the decode shift (not for a recording, whose dial already is its VFO). Test: tests/audio/test_audio_monitor.cpp,
two NFM stations with noise, the new one 250 Hz off nominal, tune offsets 0 / +20 / -15 kHz, clicks +37.5 / -52.5 / +25 kHz: the clicked
station is heard over the old one in all 9 cases, the 4.0 formula reproduces the bug in all 9, a recording's VFO is not shifted twice.
Simulated signals only; the reporter's mode and radio are not known, so this is the one stale path found, not a confirmed match.

## #470: Raspberry Pi 5 draws with llvmpipe

The reporter's `glxinfo -B` shows llvmpipe too, so GLX on that desktop gives the processor. OnAir only asked for desktop OpenGL.

Change (Linux): when desktop OpenGL comes back as a software renderer and a GPU render node exists, OnAir tries OpenGL ES 3.0 through EGL
(GLFW EGL context, `#version 300 es` shaders with default precisions, ImGui's loader filled from GLFW) and keeps it when it reaches the GPU.
The log lists the GPU node and driver, a missing `render` group permission, and variables such as `LIBGL_ALWAYS_SOFTWARE`.
`ONAIR_GL=desktop` / `ONAIR_GL=gles`. docs/RASPBERRY_PI.md. Test: tests/app/test_gl_level.cpp (table and shader text). Not run on a Pi:
whether EGL reaches V3D on that system is not known; if `eglinfo -B` also shows llvmpipe the cause is outside OnAir.
