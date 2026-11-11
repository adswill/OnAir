# Single-radio trunking (3.9)

Discord request "Multi-Trunk with one radio": follow a trunked system's control channel and several voice calls at once from one radio, as SDRTrunk does, with no second radio. Simulated only: no real trunked system was recorded for this work.

## What already worked (before 3.9)

| Protocol | One radio, before | Where it stopped |
|---|---|---|
| P25 Phase 1 / 2 | the follower took calls inside the capture as slices (whole-span bank): 2 Phase 1 at once by default (up to 4), one Phase 2 as the second mode; outside it the radio left the control channel, or a second radio took the call | one sound and one recording at a time; "needs a wider capture" without saying how wide |
| Motorola Type II | trunk pool + one wide capture (`CaptureVoice`), 4 slices, analog FM and ASTRO | each slice mixed and filtered the whole capture itself; a call with every slice busy was reported as "outside the capture" |
| EDACS | one analog (or ProVoice) call inside the capture | one call at a time |
| MPT 1327 | control channel messages only | no call following at all |
| DMR Tier III / Capacity Plus / Connect Plus, TETRA, LTR | the trunk pool had the grant adapters (logic only) | no live grant feed or voice slice in the app |
| two radios | Multi-radio trunking window: one radio on the control channel, the others retuned to the calls | unchanged, still available |

## What is new

- **Channel slices** (`core/include/dect2/trunkpool/slice_ddc.h`): each voice slot mixes its channel to 0 Hz and decimates it to about 50 kHz with one or two polyphase FIR stages, so its decoder runs at the narrow rate. Rejection of a carrier 30 kHz away: above 50 dB at 0.24 to 20 Msps (test).
- **Voice slots for every protocol** (`CaptureVoice`): analog FM (Type II, EDACS, MPT 1327, LTR and other analog), P25 Phase 1, P25 Phase 2 (logical channel of the grant, descrambler seed from the site), DMR (time slot of the grant), NXDN. Default 4 slots, 1 to 16 in the core, 1 to 8 in the app. Each slot has its own sound (`setAudioSink`), its own WAV + JSON per call (`TrunkPool` recorder), its own row in the call list, and optionally its own voice monitor (`setMonitorPrefix`), so the existing conversation tracker groups its calls.
- **`OneRadio`** (`core/include/dect2/trunkpool/one_radio.h`): the glue each trunking mode uses: capture rate and centre, control channel, grants in, calls and status out, the learned channel list.
- **Coverage** (`core/include/dect2/trunkpool/coverage.h`): a call outside the capture is shown as `outside the capture (needs 3.4 MHz)` (the bandwidth a capture holding the control channel and that channel needs), with `; this radio reaches 2.4 MHz` when the radio is too slow. Typical radio limits are built in (RTL-SDR 2.4 Msps, HackRF 20, Airspy 10, SDRplay 10, PlutoSDR 4 on USB 2, ...); the driver's own figure wins. The plan (`planCoverage`) picks the centre and rate that hold the control channel and the most used voice channels (learned from the grants), with none of them on the DC spike.
- **DC spike**: automatic. The plan keeps every channel at least 20 kHz off the centre; a slice that still lands there switches a narrow DC notch on the capture for as long as it runs. The manual switch is the existing "Ignore DC spike".
- **Control channel off centre**: MPT 1327 cuts its control channel out with the FIR slice filter on captures of 1 Msps and more (before: a boxcar decimator, which let the other channels of a wide capture alias into it). P25 and Type II were already tested off centre; the test runs them next to a DC spur.
- **Busy versus outside**: with one radio, a call whose channel is inside the capture but finds every slot in use is now "busy: every voice slot is in use", not "outside the capture".
- **App**: a "One radio: control channel and calls together" block in the P25, Type II, EDACS and MPT 1327 tabs and in View > Multi-radio trunking: calls followed now against the slots, the capture and the radio's limit, the learned voice channels (inside, or outside with the bandwidth needed), the plan in one line, and one button "Retune to cover the control channel and the busiest channels" (moves the centre and the sample rate, keeps the decoder on the control channel with a decode shift). "Record each call" (off by default, recordings/trunk1/<mode>). Advanced: "Voice slots (calls at once)" (P25 keeps its own "simultaneous calls", now up to 8, default 4).
- **MPT 1327**: calls are followed. Enter the control channel number and the spacing in the channel table (the plan); every GTC's traffic channel inside the capture is decoded as FM in a voice slot; the tab lists the calls with their slot.
- **EDACS**: with following on, every analog call inside the capture is decoded at once in a slot of its own (sound, recording, call row); the listened channel stays the one played.
- **Type II**: `setVoiceSlots`; the outside message counts from the control channel.
- **P25**: the follower's default `maxSlices` is 4 (was 2); its messages say how much bandwidth a call outside the capture needs.

Settings: `trunk1.slots` (4), `trunk1.record` (off), `p25follow.slices` (4 for new installs). Web options (generated): MPT 1327 `setFollow`, `setVoiceSlots`, `setListenTg`; EDACS and Type II `setVoiceSlots`.

The two-radio setup is unchanged: the pool still takes another radio for a call outside the capture when one is open, and the Multi-radio trunking window works as before.

## Protocols covered

| Protocol | Control channel decoded live | Calls followed from one capture | Test |
|---|---|---|---|
| P25 Phase 1 | yes | yes, N at once | control channel at -481 kHz + 4 calls, one on the DC spike, 2.4 Msps |
| P25 Phase 2 | yes (grants) | slice decoder built (TDMA slot, seed) | not tested in this work |
| Motorola Type II (analog, ASTRO) | yes | yes | control channel 12.5 kHz from a DC spur + 5 calls, 4 and 5 slots |
| EDACS (analog) | yes | yes, every call in its own slot | 3 overlapping calls |
| MPT 1327 | yes | yes (new) | control channel at -300 kHz + 3 calls, one on the DC spike |
| DMR Tier III / Cap+ / Con+ | no live CSBK grant feed (the receiver does not hand CSBKs to the trunk model) | slice decoder built and tested | 3 base-station carriers, grants given to the pool |
| NXDN | no trunking grant decoder exists | slice decoder built and tested | 3 carriers, grants given to the pool |
| TETRA | the TETRA receiver already searches and decodes every carrier of the capture itself | unchanged | - |
| LTR | LTR has no control channel (each repeater carries its own data) | not built | - |

## Measured (tests/trunkpool/test_trunkpool_onerig.cpp, generated signals only)

| Case | Result |
|---|---|
| Channel filter, neighbour 62.5 kHz away (where it would alias) | 76 dB down at 0.24 Msps, 80 to 81 dB at 1.2, 2.4, 10 and 20 Msps (output 48 to 50 kHz) |
| P25, 2.4 Msps, 4 slots: control channel at -481 kHz (+350 Hz, +20 ppm), 4 calls at -394 / +6 (on a DC spur) / +156 / +444 kHz (offsets -450 to +600 Hz, clock -45 to +45 ppm), tuner +1.5 ppm, 24 dB noise | 176 TSBKs in 6.5 s; 4 calls at once; 279 to 297 voice frames per call (of about 300); the DC notch switched itself on for the call on the spike |
| P25, 2 slots for the same 4 calls | 2 at once, the others "busy: every voice slot is in use" |
| P25 grant at 853.506 MHz (2 MHz above the centre) | "outside the capture (needs 3.0 MHz) (853.5063 MHz, the capture is 2.0 MHz wide): add a second radio or widen the capture" |
| DMR, 1.2 Msps, 3 base-station carriers at -312.5 / +12.5 / +262.5 kHz (+-300 Hz, +-25 ppm, 22 dB, DC spur) | 3 at once, 114 to 138 voice bursts each in 6 s |
| NXDN, the same layout | 3 at once, 88 to 96 voice frames each in 6 s |
| Type II, 1.92 Msps, control channel 12.5 kHz from a DC spur (+400 Hz, +60 ppm), 5 analog calls at once | 4 slots: 4 followed, the fifth busy, 4 tones recovered; 5 slots: 5 followed, 5 tones; 210 OSWs; a grant at 857.4 MHz: "outside the capture (needs 7.1 MHz)" |
| MPT 1327, 1.2 Msps, control channel at -300 kHz (+40 ppm), 3 calls at -175 / +12.5 (DC) / +200 kHz | 43 messages, 3 at once, 3 tones; channel 220: "outside the capture (needs 1.8 MHz)" |
| EDACS, 0.96 Msps, 3 overlapping calls | 3 in voice slots at once, 3 tones |
| Coverage plan, RTL-SDR (2.4 Msps), control channel + 7 learned channels | centre 851.479 MHz at 1.3 Msps: 5 of 7 channels (95 % of the calls), none within 20 kHz of the centre; "all of them need 8.4 MHz, this radio reaches 2.4 MHz" |
| Coverage plan, HackRF (20 Msps) | all 7 at 8.5 Msps |

CPU (GitHub runner, Linux x86-64): about 2.6 % of one core per extra voice slot at 2.4 Msps (channel filter plus the P25 decoder at 50 kHz); the control channel through the same filter about 2.9 %. The channel filter's first stage costs about 5.5 multiply-adds per input sample, so a slot at 20 Msps costs roughly eight times as much as at 2.4 Msps (not measured).

Regression tests run with it (quick.yml): trunkpool_pool, trunkpool_dedup, trunkpool_health, smartnet_rx, edacs_follow, edacs_rx, edacs_provoice, mpt1327, mpt1327_std, mpt1327_chan, p25_trunk, p25_scenario_slice / hop / cc, p25_wide_follow, dmr_rx, dmr_engine, nxdn_rx.

## Not done

- DMR Tier III / Capacity Plus / Connect Plus: a live grant feed from the DMR receiver's CSBKs into the trunk model, and a Tier III control channel generator to test it with.
- NXDN trunking (Type C / D) grants: no decoder of the assignment messages exists.
- P25 in the app still follows with its whole-span bank (one sound, one recording at a time); the new per-call slots are used by Type II, EDACS and MPT 1327.
- The P25 Phase 2 slice has no test of its own here.
- Not run against a real system; the app panel was not run (no local build: battery rule), only syntax-checked.
