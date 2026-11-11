# COSPAS-SARSAT: extra modes in the log, CPU, missing frames (3.9)

Report (Discord, "EG", RTL-SDR and RSPduo, 3.8.0): switching to Cospas-Sarsat prints the start-up lines of a dozen other modes, CPU goes from 10 % (0.2.9) to 70 %, no frames.

## What was found

**Other modes in the log: noise, not extra decoders.** Only the active mode's receiver is fed (`Engine::feedReceiver` dispatches on the active standard). But `Engine::start()` calls `configure(rate)` on every receiver, and each one says its rate line ("Balise: the sample rate is too low", "APRS / Packet: listening ...", "Sensors: listening ..."). The log callbacks are installed on the first start and stay installed, so every start after the first (switching mode stops and restarts the radio) prints all of them. The first start prints none, which is why it looks new. Fix: during the receiver set-up in `start()` the receivers' own messages are dropped (`Engine::rxLog`, `rxLogQuiet_`); messages while running are unchanged. Side effect: no mode's configure-time line is shown on restart any more (the first start never showed them either); the engine's own "sample rate too low" lines are untouched.

**CPU.** The SARSAT receiver's per-sample path and burst search are identical to 0.2.9 (git diff v0.2.9: only the L-band wide search, stricter false-accept checks, full-frame export; the wide search is off at 406 MHz). Through the engine with the built-in source at 0.5 Msps and the tuner 20 kHz off the dial (the mode's own offset) on a 4-vCPU GitHub runner: engine 0.07 to 0.09 CPU-s per s, bare receiver 0.07 to 0.10 (the second-generation detector's FFT and the mixers; run to run noise on a shared runner). That is about 7 to 10 % of one core there, in line with the 10 % EG saw on 0.2.9. **A 70 % load was not reproduced.** It is not in the receiver or in the engine path for this mode on a synthetic radio; what is left is outside this harness: the real SDRplay driver path, the GUI (spectrum size and frame rate, transcripts) or autogain. A steady-tone storm (a tone burst every 0.9 s) did not make the burst search expensive either (0.10 s/s, same as idle), so no false-burst problem was found. A cap is in anyway: bursts that do not look like a beacon may spend 1 s of search, repaid at 0.15 s per second of signal (`setFalseBurstBudget`); bursts that look like a beacon are always searched. That is a safety net, not a measured fix.

**Frames.** Generated beacons (five types, 25 s) decode at 0.5 Msps with the 20 kHz tuner offset: 6 of 6 messages, exact IDs. That covers the sample rate and the centre handling (hardware 406.048 / radio 406.028 is the mode's designed offset). The only change since 0.2.9 that can drop real frames is the false-accept rule (#404: a frame needing 3 or more repairs must have a perfect sync word, an unknown country code is rejected, a long message needing 2 or more repairs must have an intact second check). On a weak real beacon that is a deliberate trade. Not reproduced with real signals: validated on simulated signals only.

## Before and after (CI runner, 0.5 Msps)

| | before | after |
|---|---|---|
| log lines of other modes on a restart | reasoned from the code (callbacks kept, `configure()` on all), not run | none (16 modes checked, 3 starts each) |
| engine CPU, SARSAT | 0.07 s/s (first run) | 0.09 s/s (second run; same code path, runner noise) |
| generated beacons decoded | 6 of 6 | 6 of 6 |

## Test

`tests/sarsat/test_sarsat_engine.cpp` (ctest `sarsat_engine`): beacons decode at 0.5 Msps with the offset and CPU under 0.30 s/s; a steady-tone storm stays under 0.25 s/s; after three starts of 16 modes (sarsat, tones, packet, sensors, hfbeacon, iono, roshf, ssdv, ebicab, nrf24, specx, astro, dmr, adsb, cw, psk) no log line starts with another mode's name. Linux only, via quick.yml.

## What to ask EG

His log after the fix, CPU with the spectrum window closed, the autogain mode, and whether "Span search" is ticked in the SARSAT settings (it runs five receivers at 0.5 Msps).

## Reply for EG

```
hey, the extra lines in the log were only start-up messages from the other modes, they werent running. thats cleaned up in the next version (3.9)
the 70% cpu i cant reproduce yet, in my tests sarsat sits around 10% at 0.5 Msps. can you try with the spectrum closed and tell me if auto gain is on?
test beacons decode fine here at 0.5 Msps with the 20 kHz offset, so send me a short recording if it still shows nothing
```
