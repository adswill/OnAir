# Transmit in 4.0: parameters, the flow, the tests

Branch `feat40-tx`. Owner's request: the transmit UI, the way you select and transmit, and the per-mode options were poor, and some options did
nothing. This document has the audit of 3.9, the parameter table that replaced the hand-written panels, the new transmit flow and the tests.

Everything below was checked on generated signals in memory (the TX generators into the RX decoders, or the generated IQ compared). Nothing was
transmitted on the air.

## 1. Audit of the 3.9 transmit panel

Every option key and request field the 3.9 panel showed was traced into the generator (`core/src/tx/tx_payload.cpp`, `tx_voice.cpp`, `tx.cpp`
and the modes' own `*_txgen.cpp`). Rows marked **no** did nothing; **wrong** reached the generator with a value meant for another mode.

| Mode(s) | Parameter shown in 3.9 | Used? | What was wrong | 4.0 |
|---|---|---|---|---|
| Olivia, Contestia, MFSK, THOR, DominoEX, Hell | Sideband (USB / LSB) | no | the generator always sent USB (#225 work-around) | works: own key `kbd.lsb` |
| Olivia, Contestia | tones, bandwidth (named in the mode list) | no | not in the panel at all | added: `kbd.tones`, `kbd.bw` |
| MFSK, THOR, DominoEX, Hell | variant (MFSK16 / THOR 22 ...) | no | not in the panel; the family default only | added: `kbd.variant` |
| Flipper Sub-GHz chat, MeshCore | Centre frequency, Sideband | no | keyboard rows shown for generators that ignore them | removed; MeshCore got its real LoRa rows (`meshcore.*`, read by the generator but never shown) |
| RTTY | mark tone | no | fixed at 2125 Hz | added: `rtty.markHz` |
| PSK31 | PSK variant | yes | a bare number box | a list of the nine modes |
| Zigbee | PAN / short address | no | not packed by the generator | removed |
| FLARM | Device ID | no | not packed (the panel said so) | removed |
| VDES | MMSI | no | not packed | removed |
| Mode S uplink | ICAO address | no | not packed | removed |
| Meteor / GOES / HRPT | Image (PNG / PPM) | **wrong** | not read; the same buffer became the analog voice's audio file | removed |
| HD Radio, CDR | ensemble, label, text, services | no | the generators take no names | removed (a note says so) |
| DRM | ensemble name, services | no | DRM reads only the label and the text | removed for DRM |
| DAB | Text (DRM) | no | DAB has no text here | removed for DAB |
| ATSC 3.0 | MPEG-TS file, multiplex names | no | the ROUTE / fMP4 demo is always sent | removed (a note says so) |
| DVB-T2 | FFT size, guard, pilot pattern ... (named in the mode list) | no | only the channel width was in the panel | added: FFT, guard, pilot pattern, constellation, code rate, rotation, short frames, extended carriers, time interleaver, L1 modulation, cell / network / system ID, with a table-59 check |
| DVB-T | mode, guard, constellation, code rate | no | not in the panel | added |
| DVB-S/S2 | code rate as a bare number, no frame size / pilots / inversion | partly | | full list of code rates (S and S2), frame size, pilots, spectral inversion |
| P25, NXDN, dPMR | Callsign / my ID | no | these take the radio ID only | removed |
| P25, NXDN, dPMR, TETRA | Talkgroup, Colour code | no | only DMR reads them | removed (the destination field is the talkgroup / called ID) |
| NXDN, dPMR, D-STAR, M17 | short text | hidden | read by the generator, never shown | added |
| TETRA | callsign, destination, radio ID, talkgroup, colour code | no | TETRA sends its downlink test pattern | removed (a note says so) |
| JS8Call | To | hidden | read by the generator, never shown | added |
| JS8Call | audio frequency | **wrong** | not shown, came from the last mode's tone field | added |
| CubeSat | destination | hidden | read, never shown | added |
| VOR / ILS | navaid kind, radial / DDM (receiver's test-signal widget) | only without an ident | typing an ident sent a conventional VOR at radial 0 | works with an ident too |
| LoRa (raw) | SF, bandwidth, coding rate, sync word, preamble, CRC, gap | no | the first frame of the test schedule was always used | added (`lora.*`, same keys as LoRa APRS) |
| every mode with the receiver's test-signal widget | SNR, CFO, drift, echo, fading, mistuning | no / **wrong** | SNR, CFO, drift and echo were overwritten (did nothing); mode-specific fading / Doppler / mistuning (marine, orbcomm, sigfox ...) were sent on the air | the widget is shown only for test-scene generators, labelled as such; a transmitter never starts from the receiver's impaired settings |
| all modes | callsign, text, address, position, tone typed for another mode | **wrong** | one shared buffer: a mode that does not show the field still read it (e.g. NXDN sent the FT8 text) | `txParamsSanitize`: fields no row of the mode writes are cleared |
| analog voice | CTCSS / DCS own tone or code (`fm.ctcssHz`, `fm.dcsCode`) | hidden | read by the generator, not in the panel | added (when the list says Off) |
| analog voice | Pre-emphasis | only with mic / file | the test tone is sent flat | kept, the help says so |
| FM broadcast | Send the clock time | yes | the CT group goes out at the start of each minute | kept (marked in the table) |

## 2. The parameter table (`core/include/dect2/tx/tx_params.h`)

Each transmit mode declares its parameters once, in `core/src/tx/tx_params.cpp`. A row has: key, label, one-line help, unit, type (number,
integer, hex, on/off, choice, text, file), range, default, **Simple** or **Advanced** (and its group: What to send, Modulation, Audio, Filters,
Framing, Timing, Output), an optional sub-mode condition (`when`: `ftx.kind=1`, `dmr.call.kind=1|6|7`, `dvbs.std!=1` ...), and where the value
goes: an `opt` key, a request field (callsign, text, address, ...), or a generator setting (`SynthConfig`). Rows a mode draws with its own widget
(the audio input, the TV multiplex, a mesh node, the KA* panels) are `custom`; rows a generator reads that are set by another screen or a test are
`hidden`. Rows that cannot change the generated samples say why (`meta`).

The panel is drawn from the table: the Simple rows under "What to send", the Advanced rows in their groups. Presets, "Reset to default", the
values remembered per mode and the hand-over to the generator need no per-mode code.

### Rows per mode (defaults in brackets; S = Simple, A = Advanced)

| Mode(s) | Rows |
|---|---|
| every mode | A Output: trim (0 dB, -40..6), mirror the spectrum (off); A Timing: soft start (2 ms, 0..10 s), timeout timer (off, 0..3600 s) |
| DVB-T2 | S channel width (8 / 7 / 6 / 5 MHz), TS file, multiplex; A FFT (8K), guard (1/128), pilot pattern (PP7), constellation (64-QAM), code rate (2/3), rotated (on), short FEC frames (off), extended carriers (off), time-interleaver blocks (3), L1 modulation (QPSK), cell ID (0x1001), network ID (0x3085), T2 system ID (0x8001) |
| DVB-T | S channel width, TS file, multiplex; A FFT mode (8K), guard (1/8), constellation (64-QAM), code rate (2/3) |
| ISDB-T | S channel width (6 MHz), TS file, multiplex; A FFT mode (3), guard (1/8), layer B constellation (64-QAM), layer B code rate (3/4) |
| DVB-S/S2/S2X | S symbol rate (5 Msym/s), TS file, multiplex; A standard (S2), modulation (QPSK), code rate (S2 list or S list), roll-off (0.35), frame size (normal), pilots (off), spectral inversion (off) |
| ATSC, DTMB, DVB-C | S TS file, multiplex; A the generator's own settings (test-scene widget) |
| ATSC 3.0, HD Radio, CDR, TETRA, NFC | nothing to type (note); output rows |
| Analog TV | S picture source, ident (ONAIR TEST), sound carrier; A system (B/G), colour (usual), test picture (card) |
| DAB | S ensemble name, service names |
| DRM | S station label, text message |
| FT8, FT4, FT2, JT65, JT9, Q65, FST4, MSK144 | S callsign, grid, message (CQ / report / free text), to (report), text (free text, 13 characters); A audio frequency (1500 Hz), sideband (USB) |
| WSPR | S callsign, grid, power (37 dBm); A audio frequency, sideband |
| RTTY | S text; A speed (45.45 Bd), shift (170 Hz), mark tone (2125 Hz), reverse (off), sideband (USB) |
| Olivia, Contestia | S text; A tones (32), bandwidth (1000 Hz), centre (1500 Hz), sideband (USB) |
| MFSK, THOR, DominoEX, Hell | S text; A variant (family default), centre (1500 Hz), sideband (USB) |
| CW | S text, speed (20 wpm); A pitch (700 Hz) |
| PSK31 family | S text, mode (BPSK31); A audio frequency (1000 Hz) |
| JS8Call | S callsign, grid, to, text; A audio frequency (1000 Hz) |
| SSTV | S callsign, picture (PPM), SSTV mode (Robot 36) |
| HamDRM | S callsign, file |
| FreeDV | S audio input; A test tone (needs the user-installed libcodec2) |
| CP16 | S text; A seconds a character (2 s), carrier spacing (15 Hz), lowest carrier (1000 Hz), lower sideband (off) |
| Flipper Sub-GHz chat | S message; A offset from the centre (0 Hz) |
| MeshCore | S message; A SF (8), bandwidth (62.5 kHz), coding rate (4/8), preamble (32), pause between frames (0.05 s), offset |
| Pagers | S capcode, text, speed (POCSAG 1200, all seven POCSAG / FLEX speeds); A function bits (3, POCSAG only) |
| APRS / packet | S callsign, destination, path, comment, latitude, longitude; A speed (1200 Bd) |
| LoRa APRS | S callsign, destination, path, comment, latitude, longitude, LoRa settings (EU); A (own settings) SF, bandwidth, coding rate, preamble, sync word, CRC; pause between packets (10 s) |
| LoRa (raw) | S payload; A SF (7), bandwidth (125 kHz), coding rate (4/5), preamble (8), sync word (0x12), CRC (on), pause (2 s) |
| AIS | S MMSI, callsign, vessel name, latitude, longitude (dummy-load warning) |
| ADS-B | S flight ID, ICAO address (dummy-load warning) |
| FM broadcast | S station name, radio text, RDS PI, live RDS control, audio input; A deviation (75 kHz), pre-emphasis (50 µs), stereo (on), pilot (9 %), RDS (on), RDS level (2 kHz), PTY (10), TP, TA, clock time, music; test tones left / right and levels; RF level (0.3) |
| Airband AM | S audio input; A type (AM / DSB), depth (85 %), carrier level (0.55), the voice audio chain |
| QO-100 | S path (SSB / DATV); SSB: audio input; A sideband (USB), carrier injection (0 %), peak level (0.4), voice audio chain; DATV: the DVB-S rows |
| DMR | S callsign, DMR ID, what to send (8 kinds), talkgroup, radio ID called (private / Tier III), message (SMS), latitude / longitude (LRRP), slot sync; A colour code (1), timeslot (1) |
| P25 | S radio ID, talkgroup / called ID |
| NXDN, dPMR | S radio ID, talkgroup / called ID, short data text |
| D-STAR | S my callsign, your callsign (CQCQCQ), slow-data text |
| M17 | S callsign, destination, packet text |
| digital voice without an encoder, scene generators, mesh | S send analog voice instead; when on: audio input; A deviation (5 kHz), pre-emphasis (50 µs), CTCSS tone (off) or own tone, CTCSS level (12 %), DCS code (off) or own code, inverted, DCS level (12 %), RF level (0.4); the voice audio chain |
| voice audio chain | A test tone (1000 Hz), level (50 %), second tone (off), compressor (off; threshold, ratio, make-up, release), high-pass, low-pass, VOX (off; threshold, hang), ignore the DTMF keypad |
| Marine | S service (NAVTEX / DSC MF/HF / weather fax / DSC VHF), message (NAVTEX), MMSI (DSC) |
| ACARS, VDL2, HFDL | S registration, text |
| Radiosonde | S serial, latitude, longitude |
| SAME | S header |
| HF ALE | S callsign, called station, AMD text |
| VOR / ILS | S ident, navaid, radial (VOR) or DDM (ILS) |
| DECT | S RFPI |
| CubeSat | S callsign, destination, payload text; A speed (1200 AFSK / 9600 G3RUH) |
| KAAL, KAIT, KAVB, KAPP, KAFT, KAWS | the mode's own panel (its keys are declared as hidden rows) |
| Winlink | the Winlink screen fills the frames |
| other test-scene generators | A test scene settings (the receiver's widget, labelled); S send analog voice instead |

## 3. The transmit flow (`app/panels/tx_panel.cpp`, `tx_step_settings.cpp`)

- **Pick**: the signal list (searchable, grouped like the mode browser) on the left.
- **What to send**: the Simple rows of the chosen mode, first in the middle pane; notes and warnings that never block (a pilot pattern the FFT
  size does not allow, a free text longer than 13 characters, a station name longer than 8, the AIS / ADS-B dummy-load warning).
- **Send to**: one choice of three: Radio, Dummy load (test, no RF), IQ file. Then the radio, the level with the predicted output power, the
  radio range and band-plan / harmonic warnings (`tx_plan.h`).
- **ARM, then TRANSMIT**: two separate big buttons. ARM sends nothing; it makes TRANSMIT live and disarms itself after two minutes, on STOP,
  Space, Esc, and on a change of signal, radio or destination. A test or an IQ file starts with one button ("RUN THE TEST", "WRITE THE IQ FILE").
  The top bar's run button follows the same two steps.
- **STOP** is always on screen beside the main button. Space and Esc stop (and disarm); Space never starts anything.
- **States** in colour on a band across the panel: Idle (grey), ARMED (amber, with the time to auto-disarm), Preparing (amber), ON AIR (red,
  pulsing), Test (blue), Done (green, with the length and how it ended). While on air: elapsed time and what is left ("ends in 00:07" for one
  pass or the timeout timer, "until the message is sent" for finite modes).
- **Preview** (right pane): spectrum and waterfall of what will be sent, occupied bandwidth, peak / RMS level, the session log and the new
  "Sent" table (UTC, signal, MHz, length, payload, how it ended) of every transmission since the program started.
- **Advanced**: presets, reset, the table's groups, the session (repeat, one pass, sample rate, frequency offset), the radio (filter, gain
  stages), the schedule, IQ over TCP, calibration. Soft start now goes up to 10 s (a gentle ramp for an amplifier; the end fade stays at most 0.5 s).
- **Carrier tune (hold)** needs ARM on a radio, like TRANSMIT.

`app/panels/tx_api.h` gives other screens (the Ham station, feat40-ham) the same pieces: choose a mode, set payload texts and table values,
arm / transmit / stop, and draw the flow, What to send, Advanced and the preview. They act on the one transmitter.

- **External PTT** (Advanced > Radio): "Key a rig's PTT while transmitting" keys a Hamlib rig of the radio list (an amplifier, transverter or
  sequencer behind the SDR) by its own PTT method (CAT, RTS, DTR or VOX, from the rig's profile) just before the RF starts and releases it right
  after it stops, also when a one-pass transmission ends by itself. A rig that does not answer is a note; the transmission goes on (never a
  block). Not saved: it is chosen per session. Not tested against real hardware (the RigKeeper is the ham transmitter's, which has its own tests).
- The remote API's start (`/tx/start`) is the caller's decision and needs no ARM.

## 4. Tests

| Test | What it checks |
|---|---|
| `tests/tx/test_tx_param_model.cpp` (`tx_param_model`) | every mode of `txModes()` (and its analog-voice and DATV variants, the weather-fax sub-mode): the table is sane (unique keys, defaults in range, conditions on rows of the same table); every drawn, non-meta row changes the generated IQ when its value changes; no generator reads an `opt` key that is not a row of its mode |

| `tests/tx/test_tx_loopback.cpp` (`tx_loopback_tv`, `tx_loopback_rest`) | TX generator -> white noise at a set SNR in the occupied bandwidth and a carrier offset -> OnAir's receiver (Engine, file playback) for the table's defaults and a few parameter combinations per mode (DVB-T2 2K / PP2 / 16-QAM, DVB-T 2K 16-QAM 1/2, DVB-S 3/4, APRS 9600, POCSAG 2400, FLEX, CW 30 wpm, RTTY 50 Bd / 425 Hz, BPSK63, Olivia 8 / 500, LoRa SF9 ...); text modes must give the sent text back. Cases that decoded in 3.9 are "must", new combinations are reported |

### Results (GitHub quick.yml, Linux, run 43160509036)

- `tx_param_model`: 197 modes and sub-modes, 2057 rows changed the generated IQ, 476 rows marked as not changing the samples (custom widgets,
  files, rows that wait for a UTC slot or later lines). The first run found 66 rows that did nothing; each was fixed in the generator or the
  table (see the commits "rows the model test found doing nothing"), or marked with the reason. 13 signals depend on the clock (DAB, Meshtastic,
  the FT8 family, WSPR, FLARM, time signals): their rows are not compared sample by sample. No generator read an undeclared key.
- `tx_loopback_tv`: 10 of 10 decoded (DVB-T2 defaults and 2K / 1/8 / PP2 / 16-QAM, DVB-T defaults and 2K 16-QAM 1/2, ISDB-T, DVB-S2 and
  DVB-S 3/4 at 2 Msym/s, DTMB, ATSC, DAB) at 12 to 25 dB in the occupied bandwidth.
- `tx_loopback_rest`: 27 of 28 decoded (FM, DMR, AIS, APRS 1200 and 9600, POCSAG 1200 and 2400, FLEX, CW 20 and 30 wpm, RTTY 45 / 170 and
  50 / 425, BPSK31, BPSK63, Olivia 32 / 1000, Contestia, DominoEX, FT8, ALE, CubeSat, ADS-B, DRM, LoRa SF7 and SF9 4/7, VOR, SAME, ACARS) with
  10 to 200 Hz carrier offsets. Not decoded (info): Olivia 8 tones / 500 Hz; probably the receiver's default of 32 tones / 1000 Hz (not checked further).
- The existing `tx` round trips (sessions, fake Pluto and Soapy radios) still pass.

Simulated only. A row "changes the signal" means the generated IQ differs; whether a receiver decodes the change is the loop-back tests' job.
