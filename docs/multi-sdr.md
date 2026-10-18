# Several radios at once (multi-SDR)

Warning shown when a receiver is added: performance may limit what is possible. Every receiver is a full decoder
with its own sample thread; a high-end computer is recommended, and lower sample rates help. The status bar and the
tab strip show the process CPU load.

## Design: one App / Engine per receiver

`RX 1` is the `App` main() makes. `RX 2..4` are further `App`s made on demand (app/state/receivers.cpp), each with its own
`Engine`, radio, mode, frequency, gains, rate, scanner, results. The UI code is unchanged: it still gets `App& a`.

- Tabs: a strip at the bottom of the window, `RX 1 | RX 2 | +`, in both interfaces (the classic interface is one
  full-window layout, so it gets the same strip instead of an extra OS window). Only the selected receiver is drawn
  (so ImGui window names never clash); all receivers tick every frame (`drawUI(a, disp, draw=false)`).
- Radio claims (core/src/engine/device_claims.cpp): `Engine::start` claims the radio for `receiverId`; a second claim fails
  ("this radio is in use by RX n"). The source list shows such radios as "in use by RX n" and disables them. Network
  radios (rtl_tcp, SoapyRemote, SpyServer) are keyed by address, so they open once too. Test signal and files are free.
- Radios with several receivers of their own: a Hermes Lite 2 or Red Pitaya lists one entry per receiver (slice), "RX1" keyed
  by its address as before, "RX2" and up by `host:port/rxN`, so each one is claimed on its own and several tabs can use one
  radio. The entries share one stream (source_hermeslite2.cpp, `Link`): the sample rate is the first one's, the receiver count
  follows the highest one open.
- Closing a tab (x) stops the engine, saves its results and releases the radio.
- Audio: one receiver plays at a time (the speaker button of a tab); the others are muted (`rxForcedMute`).

## Shared (singletons) and per receiver

| Shared | Per receiver |
|---|---|
| preferences file, data folder, language, theme | engine, scanner, results (`results_rx2/` ...; RX 1 keeps `results/`) |
| data feeds out (core/src/net/feeds.cpp: all receivers send into it, settings are RX 1's) | prefs `rx2_freqMhz`, `rx2_family`, `rx2_subMode`, `rx2_lna/vga/gain/amp`, `rx2_g*`, `rx2_fFreq*`, `rx2_devName`, `rx2_bw`, `rx2_standard` |
| PSK Reporter, Discord status (RX 1), tile cache, map, update check, i18n, codecs, diagnostics | |

`plat::prefs()` is a facade (app/state/rx_prefs.cpp): with `plat::gPrefRx = n` the per-receiver keys get the prefix `rxn_`.
RX 1 uses today's names, so nothing is lost. Open receivers are remembered (`rxOpen`), not auto-started.

## Per-mode screen state

Many `app/*_ui.cpp` keep a `static State S` per mode. Two receivers in the same mode would share it, so starting a mode
another receiver already runs is refused: "RX n already uses this mode: per-receiver state for this mode is not
separated yet". Separated modes: none yet (every mode is blocked from running twice). Different modes run side by side.
The app-level results lists the screens keep themselves (app/panels/results_ui.cpp) belong to RX 1 only; engine results
(the decoded tables) are per receiver.

## Core audit

Core statics are mutex-guarded caches (radio ranges, device lists) or atomics (statistics counters, FreeDV mode choice,
SSTV ids). Decoders hold their state in the `Engine`. test_multi_rx runs ADS-B and ACARS engines concurrently.

## Tests

`test_multi_rx`: two engines (ADS-B on RX 1, ACARS on RX 2) decode at the same time; a radio claimed by RX 1 cannot be
opened by RX 2; results are saved per receiver; stopping RX 2 leaves RX 1 running.
