# Auto parameters in 3.8

Every mode with hand-set signal parameters opens on Auto; the manual controls sit under Advanced, and changing one turns Auto off for that mode. Auto re-runs when a decode stops making sense and shows what it found.

All numbers below are from **simulated signals only** (the project's own generators, with offset, drift, clock error, a noise sweep, a second signal in view and noise alone). No on-air test. Tests: `tests/autoparam/`.

## What is Auto now

| Build step | Detector | Modes | Default |
|---|---|---|---|
| 1 | RTTY auto-lock (`rttyAutoAnalyse`, on a worker thread) | RTTY: baud, shift, polarity | On in the receiver. A manual baud/shift/reverse turns it off |
| 2 | Trial decode of each modulation over the last seconds of the band | PSK31 family: BPSK31/63/125/250, QPSK31/63/125 | On (`pskAuto`) |
| 3 | Shared FSK analyser (`core/src/autoparam`: `findFskPairs`, `measureFskBaud`) | RTTY uses it to skip unlikely speeds | Internal |
| 4 | Multi-tone analyser (`analyseToneComb`) plus trial decode, ranked by occupied band, chosen by the cleanest text | Olivia, Contestia, MFSK, THOR, DominoEX; Hellschreiber by keying rate and width | On (`kbd<n>.autop`) |
| 5 | Known-set bank (a decoder per candidate, CRC decides) | JT65 A-C, JT4 A-G, Q65 A-E x 5 periods, FST4, FST4W, MSK144 | On for the one mode on screen; more CPU (Q65 is 25 decoders) |
| 6 | Width from the spectrum | DTMB 6/8 MHz (`setAutoWidth`), ISDB-T now defaults to Automatic | On |
| 7 | LoRa | Meshtastic presets search default on; LoRa APRS and MeshCom already try every preset | On (`meshPresets`) |
| 8 | One Auto button | Signal workbench Demodulate tab: modulation and symbol rate from the selection (Signal Lab `classify`, `estimateSymbolRate`) | Button |

Audit items that turned out already automatic: JS8 (decodes normal, fast and turbo at once), nRF24 (250k/1M/2M in parallel, address lengths tried). Zigbee has only a channel and a level filter. Wireless M-Bus has no signal setting in its UI. No change needed.

## Rules

- Auto runs when the decoder has no good text (none for about 8-14 s, or mostly framing errors, or text that stops reading as text). A working decode is left alone; PSK re-checks every 10 s and moves only after two agreeing answers.
- A manual change turns Auto off for that mode and stays off until the user ticks Auto again.
- The status line says what was found ("Found: 45.45 baud, 170 Hz shift, normal").

## Measured accuracy (simulated)

| Detector | Case | Result |
|---|---|---|
| RTTY | All 40 settings, +-60 Hz offset, 6 Hz/min drift, 12 dB in 3 kHz | 40/40 |
| RTTY | Noise sweep (two settings, 3 kHz): 20/10/5/2 dB, 0 dB, -3 dB | 10/10 each, 8/10, 7/10 |
| RTTY | Two signals in view, 8 dB apart | 6/6 strongest found |
| RTTY | Noise alone (40 trials, plus 30 s live) | 0 false locks |
| PSK31 family | All 7, 20 dB, 80 Hz offset, 3 Hz/min drift, 15 ppm clock | 6/7 (QPSK31 read as QPSK63) |
| PSK31 family | Noise sweep BPSK31/63: 20/12/8/5 dB | 7/8, 8/8, 8/8, 5/8 |
| PSK31 family | Noise alone | 0 false locks of 8 |
| Olivia | 22 sets, offset, 5 Hz/min drift, 15 and 6 dB | 19/22 both |
| Contestia | 22 sets, same | 20/22 both |
| MFSK | 11 variants | 9/11 at 15 dB, 10/11 at 6 dB |
| THOR | 10 variants | 10/10 both |
| DominoEX | 6 variants | 4/6 both (5 and 8 read as 11 and 16, same tone pitch, text still correct) |
| Hellschreiber | 7 variants | 7/7 at 15 dB, 4/7 at 6 dB |
| Keyboard modes | Noise alone, 10 trials per family | 0 false locks |
| WSJT bank | 19 cases across JT65, JT4, Q65, FST4, MSK144 at -2 to -10 dB (2500 Hz) | 19/19 setting found (MSK144 period may report 15 or 30 s, both decode) |
| WSJT bank | Noise alone (Q65, JT65) | 0 decodes |
| DTMB width | Both widths, 30/20/12/6 dB, three offsets | 24/24; locks from the wrong start width; noise alone measures nothing |

## What is not done

- Slow Olivia/Contestia sets (64 tones at 500/1000 Hz, and 16/125) need longer than the 45 s window; they stay on the last setting.
- MFSK11 and MFSK31 misread at 15 dB in this test.
- PSK with mixed modulations on one band: Auto picks the mode with the best text per symbol, the other signals stay undecoded.
- FT8/FT4/WSPR have no options. Whole-span decoding does not use Auto; it keeps its own bank.
- No on-air validation of any of this.
