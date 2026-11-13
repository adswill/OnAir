# OnAir 4.0.1

A bug-fix release on top of 4.0, with two video generator features by @peterdewitte. Read **Known limits** at the end.

## Fixes

- **#496** OnAir Web did nothing when Play was pressed on a server started without `--autostart`. A listener who may tune now starts the receiver (setting `web_listener_start`, on by default); a failed start shows the radio's reason to every listener, and the page tunes again when the first samples arrive.
- **#495** Hamlib was missing from the Windows release. Hamlib is now loaded at run time (`libhamlib-4.dll` on Windows, searched in the program folder, the codecs folders and PATH); without the DLL the "Add Hamlib rig..." window says what to copy where. OnAir does not ship the DLL.
- **#491** On a fresh install the first-run tour and "Get me listening" opened at once and the tour could not be skipped. Only one guide is in front at a time now; the others follow when it closes.
- **#488** "Hear this channel" kept playing the old frequency after a click on the spectrum or a frequency typed inside the captured band. The audio monitor now follows the decoder.
- **#470** Raspberry Pi 5 drew with llvmpipe (software). On Linux, when desktop OpenGL gives a software renderer and a GPU exists, OnAir tries OpenGL ES 3.0 through EGL and keeps it when it reaches the GPU. `ONAIR_GL=desktop` or `ONAIR_GL=gles` forces one (docs/RASPBERRY_PI.md).
- **Linux start-up errors**: launched from the menu (Linux Mint), a fatal start error only went to the terminal, so OnAir seemed to do nothing. It now shows a dialog and writes `~/.config/onair/startup-error.txt` (`ONAIR_NO_DIALOG=1` turns the dialog off). docs/LINUX-MINT-4.0.md.
- **Menu bar**: the right-hand items are centred vertically again.
- **Airspy HF+**: a sample rate too wide for the radio (a TV or DAB channel) stopped the stream; the radio now keeps streaming at the rate it had and says why the rate was refused. The bias-tee entry is kept up to date at each open.
- **COSPAS-SARSAT**: the false-burst search could use more than its processor budget in a storm of steady tones; bursts that look like beacons are always searched.

## New features (by @peterdewitte)

- **#493 Video generator RTMP input**: OBS Studio on this PC or another one streams picture and sound to `rtmp://<this PC>:1935/live` (key `stream`), with no virtual camera or audio cable (#490).
- **#494 Multi-programme multiplex**: the Transmit panel's Multiplex can take several video generators as programmes of one transport stream (#492). docs/VIDEO-GENERATOR.md.

## Known limits

- **#470 (GLES fallback)** was not run on a Raspberry Pi: whether EGL reaches the GPU on that system is not known. If `eglinfo -B` also shows llvmpipe, the cause is outside OnAir.
- **#495 (Hamlib DLL)** was not tried on a Windows PC or with a real rig; the tests use Hamlib's dummy rig on the GitHub runners.
- #496, #491 and #488 were checked with tests on simulated signals and on GitHub runners, not on the reporters' hardware.

Full list: docs/CHANGELOG-4.0.md.
