# Synthetic content (4.1)

The built-in "synthetic" source makes a test signal for every mode. Until 4.1 the picture and sound inside it were fixed (a test card and a
beep, a 1 kHz tone, the SSTV test picture). **Synthetic content** lets you pick your own files. It is one setting in the synthetic
source's options (the "content" row of the toolbar, below the mode's own test-signal options): **Built-in** (the default, nothing changes),
**Picture file** or **Video file**, plus an **Audio file** (empty: the video's own sound, else the built-in sound). It is saved in the settings
(`synthMediaKind`, `synthMediaPath`, `synthMediaAudio`). A mode that cannot carry files shows "built-in" and the reason in a tooltip; if a file
cannot be read or encoded, the built-in content carries on and the row says why.

Code: `core/include/dect2/source/synth_media.h` (what each mode takes: `synthMediaSupport()`), `core/src/source/synth_media.cpp` (FFmpeg
decoding), the transport-stream encoder in `core/src/tv/demo_ts.cpp` (MPEG-2 video + MP2 sound, the same encoder the transmitter's test
programme uses, the file looped), and the hooks in each generator. Test: `tests/source/test_synth_media.cpp` (ctest `synth_media`).

| Mode (test signal) | Picture | Sound | How |
|---|---|---|---|
| DVB-T, DVB-T2 | yes | yes | transport stream made from the file (T2 carries it in PLP 0) |
| DVB-C, DVB-C2 | yes | yes | transport stream (`demoTsSource`) |
| DVB-S, DVB-S2 | yes | yes | transport stream |
| DVB-SH | yes | yes | transport stream |
| ATSC 1.0 | yes | yes | transport stream |
| ISDB-T, ISDB-TSB | yes | yes | transport stream in the main layer |
| ISDB-S | yes | yes | transport stream in the main stream |
| DTMB | yes | yes | transport stream |
| Analog TV (PAL, NTSC, SECAM, B/G, I, D/K, M, ...) | yes | yes | the picture fills the frame (black bars, no distortion) on the vision carrier; the sound modulates the sound carrier |
| FM broadcast | - | yes | the sound file is the stereo programme (RDS stays) |
| General receiver: AM, NFM, WFM, DSB | - | yes | the sound replaces the test tone (SSB keeps its tone) |
| HF SSTV | yes | - | the picture is scaled to the size of the chosen SSTV mode |

Not possible, and why (they keep the built-in content; OnAir ships no encoder for them and never ships patented ones):

- DAB / DAB+: needs an MPEG audio encoder for DAB frames and the DAB+ HE-AAC encoder.
- DRM, DRM+: needs an AAC / xHE-AAC encoder.
- HD Radio (FM and AM): the patented HDC codec.
- ATSC 3.0: HEVC video and AC-4 / AAC sound inside ROUTE and ALP; the test card is a fixed test payload.
- NBTV / mechanical TV: keeps its built-in picture (32 lines).
- Digital voice modes (DMR, P25, NXDN, D-STAR, dPMR, Yaesu, TETRA): need the user-installed vocoders (docs/CODECS.md); their test signals carry a fixed voice frame.
- Data modes (ADS-B, AIS, ACARS, pagers, ...): no picture or sound inside.

Limits: a picture or a video is scaled to 640 x 360 for the transport-stream modes and encoded once (at most 20 s of a video, looped; a
picture 4 s, or the length of its sound file); the sound of the sound modes is at most the first 120 s of the file, looped. The first
encode of a file takes about a second.
