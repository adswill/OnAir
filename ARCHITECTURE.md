# How OnAir is put together

OnAir is a C++20 program in two layers: a **receiver library** (`core/`) that knows nothing about windows, and a **graphical app** (`app/`) that draws what the library reports. The command-line tools in `tools/` use the same library.

## The path of a signal

```
radio ──► ring buffer ──► Engine ──► standard receiver ──► transport stream ──► Player ──► screen and speakers
(source)    (ring.h)    (analysis      T2 / DVB-T / ATSC / DAB     (ts.cpp)     (player.cpp)
                         thread)
```

1. **Sources** (`core/src/source*.cpp`) deliver complex samples from a radio or a file: HackRF natively, other radios through their own library (`source_native.cpp`) or SoapySDR (`source_soapy.cpp`), recordings and a built-in synthetic signal.
2. **Engine** (`engine.cpp`) owns the source, runs the analysis thread, feeds the spectrum analyser and the receiver that matches the chosen standard, watches the radio (unplug and reconnect), and publishes snapshots for the interface.
3. **Receivers**: DVB-T2 (`t2rx.cpp` finds the P1 preamble, tracks symbols, equalises; `t2l1.cpp` reads the signalling; `t2plp.cpp` runs the error correction), DVB-T (`dvbt_rx.cpp`), ATSC (`atsc_rx.cpp`), ATSC 3.0 (the `atsc3_*.cpp` files: `atsc3_sync.cpp` finds the bootstrap and cuts frames, `atsc3_frame.cpp` decodes a frame, then ALP, IP, ROUTE and `atsc3_remux.cpp` turn it into a transport stream), ISDB-T (`isdbt_rx.cpp` finds the mode and guard interval, tracks carrier and clock and reads the TMCC; `isdbt_demod.cpp` does the channel estimate, the interleavers, Viterbi and Reed-Solomon per layer; `isdbt.cpp` and `isdbt_tables_data.cpp` hold the carrier layout and TMCC coding, `isdbt_gen.cpp` is the transmitter used for the tests and `isdbtgen`), DAB (`dab_rx.cpp`), DVB-S/S2, DTMB, analog TV, DMR, DRM and ADS-B (one set of files per mode, `core/src/<mode>_*.cpp`, each with a test-signal generator; `modes.cpp` lists their tuning and test signals), FM radio (`fm_rx.cpp`: a channel filter brings the station to 500 kHz, a discriminator gives the multiplex, a pilot PLL drives the stereo decoder and the RDS decoder; `fm_gen.cpp` is the transmitter used for the tests). The error correction (LDPC, BCH, Viterbi, Reed-Solomon) lives in `ldpc.cpp`, `t2fec.cpp`, `dvbt_fec.cpp`, `dab_fec.cpp`. The LDPC decoder can run on the GPU (Metal in `gpu_ldpc.mm`, Direct3D 11 in `gpu_ldpc_d3d11.cpp`), with a self-test and the CPU as fallback.
4. **Transport stream** (`ts.cpp`, `bbunpack.cpp`): turns baseband frames into packets, parses the service list, guide and subtitles.
5. **Player** (`player.cpp`) decodes video and audio with FFmpeg, keeps the audio clock, and repairs short gaps (`conceal*.cpp`: Apple's model on macOS, a Direct3D 11 version on Windows, a CPU motion search elsewhere).
6. **Outputs** (`tsout.cpp`, `nettuner.cpp`): recording, UDP/RTP, and the built-in network tuner (HDHomeRun-style, M3U and XMLTV).

Threads: the analysis thread runs the receiver (it must never wait for the interface), the error-correction work runs on helper threads or the GPU, the player has its own reader thread and audio callback, and the interface runs on the main thread.

## Folders

| Folder | What is in it |
|---|---|
| `core/include/dect2/` | public headers of the receiver library |
| `core/src/` | the implementation, one file per topic (see above) |
| `app/` | the window: `main.cpp` (loop and layout), `app.h` (state and shared declarations), and one file per group of panels |
| `tools/` | `dect2cli` (receiver on the command line), `dect2scan` (channel scanner), `package/` (installer scripts), `dev/` (developer helpers) |
| `tests/` | unit tests, benchmarks and a fuzzer; run with `ctest` |
| `packaging/` | icons, Windows installer script and toolchain file, Linux resources |
| `third_party/` | Dear ImGui and miniaudio, with their licences |

## The app

`app/app.h` holds the `App` struct (all interface state) and the list of panels. `main.cpp` has the window loop and the layout. The panels are grouped: `toolbar.cpp` (top bar, status bar), `plots.cpp`, `plot.cpp` (our own plotting library on ImGui's draw lists: axes, lines, dots, bars, images, heat maps, panning and zooming), `analysis_tabs.cpp` (receiver analysis tabs), `tv.cpp` (player and guide), `scan_outputs.cpp`, `antenna.cpp`, `dab_ui.cpp`, `fm_ui.cpp`, `ui2.cpp` (the new interface shell: menu bar, top bar, the Panel / Sidebar / Scope / Tiles layouts and the palettes; the classic shell stays in `main.cpp` and is one click away under View), `wizard.cpp` (first-run tour), and `widgets.cpp` (small shared pieces). Drawing goes through `gfx.h` (Metal on macOS, OpenGL elsewhere), and `platform.h` hides file dialogs and settings storage per operating system.

## Platform notes

- **macOS:** Accelerate (vDSP) for the signal processing, Metal for LDPC, CoreAudio, Apple's frame interpolation (macOS 15.4+, left out when building with an older SDK). Apple silicon uses NEON code; Intel builds use AVX2 and refuse to start on a processor without it.
- **Windows:** built for AVX2 (no run-time dispatch), Direct3D 11 for LDPC and picture repair, D3D11VA for video, miniaudio (WASAPI) for sound.
- **Linux:** the vector kernels exist twice (AVX2 and baseline) and the loader picks one; the GPU decoder is a stub, so error correction runs on the CPU.

## Tests

`ctest --test-dir build` runs the unit tests. Most use synthetic signals produced by the generators in the library (`t2gen.cpp`, `dvbt_gen.cpp`, `atsc_gen.cpp`), so they need no radio. `test_gpu_ldpc` compares the GPU decoder with the CPU one, `test_native` loads fake radio libraries, `test_conceal` checks the picture repair against known pictures. `test_fm_rx` sends a stereo station with RDS and noise through the FM receiver and checks the sound, the stereo separation and the decoded station name and text.
