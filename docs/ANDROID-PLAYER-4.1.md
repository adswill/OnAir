# Android player 4.1 (#435)

The Watch panel of the Android app plays the TV modes inside the app, as the desktop does. Nothing was tested on a real phone: the
code is checked by the Kotlin unit tests and the Android CI build only.

## What exists (from 4.0) and what 4.1 adds

| Part | 4.0 | 4.1 |
|---|---|---|
| Digital TV (DVB-T/T2, DVB-C, DVB-S/S2, ISDB-T, DTMB, ATSC ...) | service list from the core's PSI/SI, `TsDemux` -> framers -> MediaCodec on a SurfaceView, sound on AudioTrack, A/V sync | full screen, a clearer line for AVS/AVS+/AVS2/AVS3 and DRA |
| Video | H.264, HEVC, MPEG-2, MPEG-1 with the phone's MediaCodec | unchanged |
| Sound | AAC (ADTS and LATM), AC-3, E-AC-3 with MediaCodec when the phone has them; MP2 with the phone's decoder or the built-in layer II decoder | unchanged |
| Programme and sound track choice | chips per service and per track, remembered choice, language order | unchanged |
| Analogue TV (Analog TV, MAC, FPV video) | nothing (no transport stream) | the pictures the core's own receiver makes, as on the desktop |
| Full screen | no | yes, for both |

## Analogue TV

`native/src/jni_tv.cpp nativeAnalogFrame(which, meta, out)` hands over the newest `AtvFrame` of `Engine::atv()` (0), `Engine::mac()` (1) or
`Engine::fpv()` (2): RGBA with alpha forced to 255, plus `{seq, width, height, colour, untuned, field rate}`. It returns the bytes copied, 0
when nothing is newer, or `-(bytes needed)` when the buffer is too small. `tv/AnalogTvController.kt` asks a little faster than the frame rate
(`analogPollMs`: 25 pictures/s -> every 33 ms, at most 30/s, at least 5/s), copies into one of two ARGB_8888 bitmaps that take turns and the
panel draws it with `Image`. The sound is the receiver's own (the core's audio output). The mode families are 8 (Analog TV), 47 (MAC) and
101 (FPV video), all in the TV group, so the same Watch panel picks the analogue view by family (`analogSourceOf`).

## Full screen

`ui/panels/TvFullscreen.kt`: a full-size dialog, black, system bars hidden (swipe from the edge shows them for a moment), landscape while
open when the picture is wider than 1.2:1, restored on exit. Back or the Exit chip leaves; a tap shows the chip for 4 s. Only one copy of
the picture exists at a time: the panel shows "Showing full screen" while the dialog has the SurfaceView. `TvController.releaseSurface`
lets the player go only when the destroyed Surface is the one in use, because the dialog's Surface may arrive before the panel's is gone.

## Formats a phone cannot play

Codecs are user-installed and never shipped. `notHandledNote` gives the plain line:
- AVS, AVS+, AVS2, AVS3 video (stream types 0x42, 0xD2, 0xD4 or a codec name starting with AVS): no Android decoder, OnAir ships none, the
  desktop shows it with the AVS+ add-on the user installs (docs/CODECS.md).
- DRA sound: the same, with the DRA add-on.
- Anything else not handed over (AC-4, MPEG-H ...): the general line from 4.0.
- A handled format the phone has no decoder for (MPEG-2 or AC-3 on many phones) keeps its 4.0 line ("This phone has no ... decoder").

No VLC, no ExoPlayer/Media3: the platform's MediaCodec only, so THIRD_PARTY.md is unchanged.

## Tests

`android/app/src/test/.../tv/AnalogTvTest.kt` (JVM): source per family and the mode table still has those families in the TV group, frame
facts from the meta array (bad sizes, NaN, short arrays), aspect (768x576 and 640x480 4:3, MAC 1024x576 16:9, clamps), the line under the
picture, the poll rate limits, the AVS+/DRA/AC-4 lines and a DTMB service with AVS+ and DRA, and which codecs go to MediaCodec (MP2 with
the built-in decoder when the phone has none). `tools/check_jni.py` checks the new JNI call against the C++.

Not covered: the MediaCodec and Bitmap paths, the dialog and orientation (no device or emulator in CI). Validated on no real signal.
