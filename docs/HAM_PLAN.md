# Ham operation plan (goals section 7)

Goal: WSJT-X style operation for every amateur mode, for people with a rig. The switch at the top left is now RX / TX / Ham.

## What exists (inventory, 2026-10-20)

| Part | Where | State |
|---|---|---|
| Hamlib CAT | `core/include/dect2/source/hamlib_rig.h`, `rig_setup.h`, `core/src/source/hamlib_rig.cpp` | Rig list, serial/rigctld, dial + mode follow. `makeHamlibTxSink`: PTT and frequency only, **no RF audio** (the note says so) |
| TX session | `core/include/dect2/tx/tx.h`, `core/src/tx/tx.cpp` | One continuous or timed session per mode on a TX-capable SDR; FT8/FT4/WSPR/FT2/JT65/JT9/Q65/FST4/MSK144 payload kind `Ftx`; no per-slot scheduling |
| Safety today | `app/panels/tx_ui.cpp` (`txNoticePopup`, Start, STOP ALL) | One-time notice, explicit Start, STOP ALL. No band check, no arm. "OnAir does not limit the frequency" |
| Modulators | `core/src/hfdig/hfdig_ftx_gen.cpp` (`ftxAddTransmission[Ex]`), `hfdig_gen`, `hfdig_sstv_gen`, `hfdig_hamdrm_gen` | Audio for FT8/FT4/FT2/WSPR/JT/Q65/FST4/MSK144 messages at any audio Hz and start time |
| Decoders | `HfdigFtx` (`hfdig_ftx.h`), `FtxDecode` has call, grid, cq, snr, dt, hz, slotUtc | Done |
| Band table | `core/include/dect2/bandplan/bandplan.h` (service "ham") | Channel plans, not used as a TX gate |

New in phase 1: `core/include/dect2/ham/ham.h` (+ `core/src/ham/ham.cpp`, test `tests/ham/test_ham_core.cpp`): amateur band table and `hamTxAllowed` gate, callsign/grid validation, ADIF writer/parser, `WorkedLog`, slot timing, message builders/parser, `QsoEngine` (CQ, reply, report, R-report, RR73, 73, retries, Hold, watchdog, halt). `app/ham/ham_ui.cpp`: the Ham panel (FT8/FT4/FT2).

## Phase 2 (done): the slot transmitter

`HamSlotTx` (core/src/ham/ham_tx.cpp, tested with a fake rig in tests/ham/test_ham_tx.cpp): gate `hamTxAllowed` again, modulate with `ftxAddTransmissionEx` at 48 kHz, PTT on, play, wait for the audio to drain, PTT off on every exit. `abort()` (Halt TX, disarm, watchdog) flushes the audio and releases PTT at once. The Ham panel runs it on a worker thread and calls `QsoEngine::txSent` only after the burst was really played. The real rig is Hamlib PTT (or none: VOX) plus a chosen sound card output and a level slider. Not done: SDR TX path (needs SSB IQ from the audio, via TxSession), RTS/DTR PTT, split, ALC/power meter, per-mode duty-cycle limit, mode sub-band check, in-app help. Touch interface has a Ham view (sheet button).

## Next phases

1. SDR transmit path for FT8/FT4/FT2 (audio to USB IQ through the TX sink, same gate).
2. Rig control: RTS/DTR PTT, split, frequency/mode set from the band choice, ALC and power warnings, sub-band check per mode, duty-cycle limit.
3. One mode per agent, in this order: WSPR beacon, JT65/JT9/Q65/FST4/MSK144 (engine variants), JS8Call, PSK31/RTTY/CW keyboard panel (shared), SSTV, APRS/packet, M17, then the rest of the table below.
4. Uploads, opt-in, user credentials, off by default: PSK Reporter, QRZ, LoTW, Club Log, eQSL.
5. Docs page "Operating with a rig" and in-app help.

## Modes (one agent per row)

| Mode | RX exists | TX exists (generator) | What Ham operation needs |
|---|---|---|---|
| FT8 | yes | yes (Ftx) | Phase 1 engine/panel done; phase 2 slot transmitter |
| FT4 | yes | yes | same, 7.5 s slots |
| FT2 | yes | yes | same, 3.75 s slots |
| WSPR | yes | yes | DONE (0.2.9): beacon in the Ham panel, `WsprBeacon` (core/src/ham/ham_wspr.cpp): 1 slot in N (N >= 2, default 5), power 0..60 dBm in WSPR steps, 1400..1600 Hz, stops after 30 transmissions without operator action; spots optional, not done |
| JT65 / JT9 / JT4 | yes | yes | DONE (0.2.9): the QSO engine runs them (60 s slots, 1 s start delay, RRR instead of RR73, JT65 A B C and JT4 A..G choice, mode bandwidth in the TX gate); test tests/ham/test_ham_modes.cpp (engine - HamSlotTx - decoder loopback). Known: the JT65 B / C decoder misses signals above about +5 dB |
| Q65 | yes | yes | engine variant, sub-mode + period choice |
| FST4 / FST4W | yes | FST4 yes | engine variant; FST4W beacon like WSPR |
| MSK144 | yes | yes | meteor-scatter 15 s repeating sequences, fast-sequence engine |
| JS8Call | yes | no | modulator + directed-message engine |
| PSK31 / PSK63 | yes | yes | DONE (0.2.9): `core/src/ham/ham_kbd.cpp` renders the text as one burst (48 kHz), `app/ham/ham_kbd_ui.cpp` is the panel: received text, type-ahead buffer, macros (CQ, Answer, RST, QTH, Name, 73) filled from the station settings, call-sign pick from the decoded text, Log QSO to ADIF; same Arm / licence / band gate (`HamSlotTx::sendAudio`), 600 characters and 180 s per burst, 50 % of 10 minutes on air; test tests/ham/test_ham_kbd.cpp. Verified with a differential detector and the varicode decoder, not with the full PskReceiver |
| RTTY | yes | yes | DONE: 45.45 Bd / 170 Hz, same panel; contest exchange not done |
| CW | yes | yes | DONE: text to Morse (prosigns as <AR>), 5 to 40 WPM, macros as memories; no paddle keyer input |
| Olivia / Contestia / MFSK / THOR / DominoEX | yes | yes | DONE: Olivia and Contestia 8/250, 16/500, 32/1000, MFSK4 .. MFSK128 (+ 64L / 128L in the HF digital decoder), THOR 16 / 22, DominoEX 11. MFSK text and fldigi's pictures (Shortwave Radiogram) decode continuously under tuning error, clock error, noise and fading in the tests; see docs/MFSK-IMAGES.md |
| Hellschreiber | yes | yes | DONE: Feld Hell; the received picture is in the Hellschreiber view |
| MT63 / FSQ / IFKP | yes | no | modulators, keyboard/selcall panel |
| SSTV | yes | yes | picture send/receive panel |
| HamDRM / FreeDV | yes | yes (generators) | digital voice: user-installed codec, PTT, mic path |
| ARDOP | yes | no | ARQ session (large; defer) |
| APRS (AFSK) / packet | yes | no | AX.25 modulator, beacon, message panel |
| LoRa APRS | yes | no | LoRa modulator (check legal use band) |
| D-STAR / DMR / Yaesu Fusion / M17 / NXDN | yes | DMR only | only where an open codec/stack exists; M17 first (open) |
| HF ALE | yes | no | defer |
| QO-100, satellites | yes | no | uplink path via SDR TX; separate task |

## Phase 3 (0.3.0, core done; the Ham view still has to use it): rig control, SDR transmit, safety layer

- Safety (`core/include/dect2/ham/ham_safety.h`): `HamTxCheck::country` adds the country's own amateur bands from `data/bandplans/builtin.json` (each cut to the ITU union, so a plan only narrows; unknown country or a plan without ham bands refuses). Per-mode duty limits inside `HamSlotTx` (slot modes 60 % of 10 min, WSPR 60 % of 20 min, keyboard 50 % of 10 min; one burst at most a slot / 200 s). TX audit log `txAuditAdd` (memory + optional file, one line per attempt: time, path, mode, result, frequency, seconds, call, on-air text; never credentials). Hard stop: an output error (`write() < 0`), a stalled output (3 s) or a drain that never ends stops the burst and releases PTT. `HamRigIo::prepare()` gets the exact check that passed and must tune the radio to that frequency; `waitForSlot()` starts a burst early by the radio's latency.
- SDR (`core/include/dect2/ham/ham_sdr.h`): `SdrTxChain` 300..2900 Hz complex band-pass (USB, LSB option), drive, ALC under a power limit (% of full power), cubic B-spline interpolation to the radio rate (42 x 48 kHz on a HackRF). Measured: opposite sideband -86 dBc, images below -120 dBc, two-tone IMD3 -68 dBc at 12 dB ALC, 2 to 4 % of one core at 2 MHz. `SdrHamIo` keys a TxSink only after `prepare()`, at the checked frequency; refusals without a confirmed band filter, outside the radio's TX range, audio outside 300..2900 Hz, gain out of range; warnings per radio (no PA, HF harmonics, amp, duplex note). `sdrSinkFactory(d)` claims the radio. **UI to do (ham-ops):** radio choice CAT / SDR, band filter checkbox, power limit, drive, gain, warnings list, the TX monitor (`setMonitor`).
- Rig (`core/include/dect2/source/rig_control.h`): `RigKeeper` (watched link: read every 2 s, re-open after 1, 2, 4 .. 30 s, never while keyed; read every 2 s during a burst and stop it when the rig is gone), `tuneForTx` (set, read back, refuse when off by more than 20 Hz; split through the TX VFO; PKTUSB falls back to USB), PTT by CAT / RTS / DTR (Hamlib `ptt_type`, separate PTT port) or VOX, a failed PTT release closes the port and tells the operator. `CatRigIo` is the HamRigIo for it (replaces the app's RealIo). Finding the rig: port-name hints (IC-7300 .. CP2105 Yaesu, CH340 Xiegu, QDX / QMX as TS-480) then a CAT read probe; `pairRigAudio` picks the rig's USB sound card; `networkRigPresets` (rigctld, FlRig); `hamlibErrorHint`. The rig profile (`HamlibRig`) now carries PTT type / port, data mode, TX audio device, split. The TX-view Hamlib sink no longer keys a rig outside the amateur bands.
- Tests: `tests/ham/test_ham_safety.cpp` (licence x Arm x band matrix on both HamSlotTx paths, country plans, duty, audit, stall, slot wait), `tests/ham/test_ham_sdr.cpp` (spectrum, ALC, power limit, the matrix on the SDR path, FT8 decoded back from the IQ, Halt and radio failure), `tests/source/test_rig_control.cpp` (keeper, split, PTT, CatRigIo matrix, rig lost mid-burst, detection, pairing, profile). Recording sinks and fake CAT only.

## Safety rules used

Licence statement stored in prefs (`ham.licence`), callsign and grid validated, explicit Arm button (never saved; cleared by watchdog), frequency + bandwidth wholly inside an amateur band (and inside the country's plan when set), per-mode duty-cycle limit, the radio tuned to the checked frequency and read back, hard stop on any output or rig error, TX audit log, Halt TX, watchdog (360 s without operator action), retry limit, no uploads.

## Operating workflow (0.3.0, ham-ops-030)

`app/ham/ham_ui.cpp` is the operating screen; `ham_log_ui.cpp` (logbook, awards, contest), `ham_map_ui.cpp` (map, spots), `ham_setup_ui.cpp` (setup wizard, guide, shortcuts) around it; shared state in `ham_ops_ui.h`. Logic in `core/include/dect2/ham/ham_log.h` (Logbook with undo, dupes, search, full ADIF round trip, awards, stats, WorkedIndex), `ham_dxcc.h` (prefix database: built-in cty.dat-style table of about 200 entities written from the public ITU allocations, or the user's own cty.dat; never downloaded), `ham_ops.h` (sun / grayline, great-circle paths, band dials, IARU region edges, contest + Cabrillo, spots, macros). The log file stays `onair_ham.adi`, now rewritten whole on each change (tmp + rename). Safety unchanged: a message the operator picks (Tx1..Tx6, free text, macros) goes through the same slot scheduler and `hamTxAllowed`; Arm has no shortcut; Esc halts. The region check is a warning only (the gate keeps the union of the regions). Not done: contest exchange inside the FT8 messages (the engine sends the standard messages; the exchange is logged), rig frequency from the band bar beyond the receiver tune (rig agent), ALC / power meters from the rig.

## Warnings, not locks (3.2, owner rule 2026-10-26)
OnAir does not decide whether a transmission is legal: "its the users decision at the end of the day". The only gates left are the explicit **Arm** (never saved, cleared by Halt and the watchdog; nothing transmits by accident) and numbers that cannot be keyed. Everything the gate used to refuse is now a plain-words **warning** shown on screen, kept in the TX audit log with the burst, and the burst is sent:
- no licence set: "No amateur licence is set. You are responsible for being allowed to transmit."
- outside the amateur bands: "27.265 MHz is not an amateur band (11 m is CB). You are responsible for being allowed to transmit here." The same for a country plan miss (`HamTxCheck::country`), the SDR path's centre-frequency / lower-sideband check, and a CAT rig keyed outside the bands (stderr line).
- a callsign or locator that does not look valid, and an SDR without a confirmed band filter (harmonics).
`hamTxAllowed(c, why)` returns false only for "TX is not armed" and a frequency that is not a number; on true, `why` holds the warnings. `hamTxWarnings(c)` lists them. Kept on purpose: Arm / PTT itself (and a radio that cannot transmit at all). **Removed in 3.2 (owner, 2026-10-26: "remove all tx blocks")**: the HackRF-only amp switch and gain range, the radio's TX range, the SDR chain's 300-2900 Hz audio window, the burst-length and duty-cycle refusals (`HamSlotTx`, `aprsSend`, `js8Send`, `kbdSend`, `sstvSend`, the WSPR "one slot in two" floor), the audio-stall and drain-timeout stops, the remote-API refusal of transmit paths, and the compile-time web transmit ban (now the host setting `web_allow_tx`, see ONAIR-WEB.md). `sdrTxLimitWarnings()` and the burst/duty text in the audit log are warnings only: overdriving or a long duty cycle can damage the radio, and the user is responsible for the law and for the hardware. **The one new rule is the map guard**: no upload to the OnAir map server while transmitting and for 10 minutes after, and never an identity transmitted in the last 24 hours (TRANSMIT.md, `core/include/dect2/net/map_guard.h`). The audit log still records every burst.

## 11 m (CB)
The Ham panel band bar and the FT8 band buttons have an 11m entry (CB, 26.965-27.405 MHz, dial 27.265 MHz USB, the frequency CB FT8 groups use by custom; it is not a standard). 11 m is not an amateur band, so it is not in `hamBands()`: the gate WARNS ("27.265 MHz is not an amateur band (11 m is CB). You are responsible for being allowed to transmit here.") and, once Arm is pressed, transmits. `rxBandNameFor()` names the band for the buttons. Test: tests/ham/test_ham_core.cpp, test_ham_ops.cpp.
