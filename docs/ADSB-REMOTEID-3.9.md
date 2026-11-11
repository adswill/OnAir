# ADS-B overlapping replies and Remote ID CPU (3.9)

Branch `feat39-adsbov`. Everything below was measured on simulated signals only (the generators in
`core/src/adsb/adsb_gen.cpp` and `core/src/rid/rid_gen.cpp`). The one real recording in the tree
(`tests/data/adsb/modes1_2msps_cu8.bin`, one aircraft, no overlaps) decodes the same 57 short and 126 long
messages before and after.

## 1. ADS-B: overlapping Mode S replies

### What changed (core/src/adsb/adsb_demod.cpp, step 6 in adsb_demod.h)

- **Subtraction of decoded frames.** A frame that passes the checks is rebuilt from its bits and subtracted
  from the complex samples. The rebuild uses the frame's carrier (lag-1 products, then an FFT over its pulses)
  and its complex gain (the mean pulse within +-20 us). The pulse shape is learnt from the frame itself, per
  pattern of three neighbouring half bits and per eighth of a half bit, so the radio's filter and the sampling
  phase need no model. The power, its running integral, the 0.25 us bins and the preamble scores under the
  frame are then worked out again. The subtraction is undone if it removes less than half of the energy.
- **Search inside the frame.** After a subtraction the scan goes on 2 us after the preamble, not after the end
  of the frame, so a second preamble buried in the first frame's data is found on the residual.
- **Retries.** A candidate that failed is remembered for one frame length. When a later frame over it is
  decoded and subtracted, the candidate is tried again on the residual.
- **Separation hypotheses.** These are tried when a frame fails while another preamble shows inside it, a
  decoded frame lies over it, or it is strong (10 dB and more) and still fails.
  - A sequence estimate (two-state Viterbi with the filter taps of the coherent slicer). Its cost lets the
    residual of every half bit hold a piece of the other reply at that reply's level and at any phase. Opposed
    phases, which cancel a pulse in the power and coherent slicers, do not fool it.
  - A multi-hypothesis frame: the coherent bits, with the bits on which the power, coherent, sequence and
    separation decisions disagree marked least certain. The tracker's CRC repair (syndromes over the least
    certain bits: 1 bit, or up to 3 with error correction 2, the app default) then works on those first. The
    tracker's rules are unchanged: a repaired DF11/17/18 needs a known address.
- **Guard.** What is left of a subtracted frame is never handed on as a new message. The check is the same DF
  and address within 3 us of that frame.
- Failures inside a subtracted frame and on retries are not counted as CRC failures.
- Test knob: `AdsbReceiver::setOverlap(false)` / `AdsbDemod::setOverlap(false)` turns it all off.

### Generator: overlap scenario

`adsbOverlapScenario(AdsbOverlapConfig)` (adsb_gen.h) generates pairs of replies with these settings:
- the second reply starts 2 to 118 us after the first
- the second reply is -12 to +12 dB relative to the first
- each reply has its own carrier offset (+-150 kHz) and phase
- 25 % of the second replies are DF11, the rest DF17
- the addresses come from a fleet of 24 aircraft
- one pair every 1 ms

The scenario is the same on every platform: it uses its own xorshift generator.

`adsbtool sim --overlap N [--no-overlap-rx]` runs it.

### Results (tests/adsb/test_adsb_overlap.cpp, 150 pairs per case, recovery off -> on)

| Case | Frames decoded | Both of a pair | Phantoms |
|---|---|---|---|
| 2 Msps, 30 dB, fix 1 | 31.7 % -> 61.3 % | 0 % -> 58.7 % | 0 -> 0 |
| 2 Msps, 30 dB, fix 2 | 32.3 % -> 64.7 % | 0 % -> 62.7 % | 0 -> 0 |
| 2 Msps, 20 dB, +40 kHz, fix 2 | 31.3 % -> 55.7 % | 0 % -> 46.7 % | 0 -> 0 |
| 2.4 Msps, 30 dB, -60 kHz, fix 1 | 42.0 % -> 83.7 % | 0 % -> 82.0 % | 0 -> 0 |
| 8 Msps, 30 dB, +25 kHz, fix 2 | 49.3 % -> 97.7 % | 0 % -> 96.0 % | 0 -> 0 |
| 2 Msps, 14 dB, fix 2 | 21.3 % -> 27.3 % | 0 % -> 11.3 % | 0 -> 0 |

The busy airspace from the generator (AdsbAirspace, 2 Msps, 25 dB, twice the normal message rates, 1 s,
frames above 8 dB) gave these results:

| Aircraft | Overlapped frames | Overlapped decoded | Alone decoded | CPU (Mac) |
|---|---|---|---|---|
| 150 | 29.6 % | 26.1 % -> 42.4 % | 87.1 % -> 87.3 % | 0.145 -> 0.222 s/s |
| 300 | 51.1 % | 21.5 % -> 35.7 % | 82.7 % -> 82.9 % | 0.275 -> 0.467 s/s |

There were no phantom messages in any case, and none on noise alone. The receiver's CPU guard (setEffort)
still lowers the work on a slow machine. Below effort 1 the recovery is off.

### Limits

- At 2 Msps the stronger reply of a pair 6 dB apart often fails on its own: 4 to 14 wrong bits, where the
  other reply's pulses meet its own in opposed phase. The separation estimate fixes a few of these. When the
  stronger reply does not decode, nothing can be subtracted, and the weaker one is lost too. At 8 Msps nearly
  every pair is recovered.
- The separation estimate added about 1 % on its own. Most of the gain comes from the subtraction and the
  retries.

## 2. Remote ID CPU

feat39-sensC measured 2.44 s of CPU per second of signal on one CI core and 0.75 on the Mac. Remote ID runs the BLE
advertising demodulator and the Wi-Fi demodulator (802.11b chain and OFDM receiver) side by side on a 20 Msps
capture.

### What changed

- **Burst gate in front of the Wi-Fi demodulator** (core/src/rid/rid_rx.cpp).
  - The detector runs 128-point FFTs over half of every 25.6 us window. It measures the energy in the Wi-Fi
    channel, without the BLE channel and the DC spike, against its own noise floor: the lower quartile of every
    16th window over 26 ms, so a long 802.11b beacon cannot raise it.
  - Wi-Fi gets the windows from 102 us before a window with energy (0.8 dB over the floor) to 205 us after it.
  - It gets everything during the first 6.5 ms and while the channel is busy more than 60 % of the time.
  - The level shown is still that of the whole input. `RidTelemetry::wifiFed` is the share handed on.
  - Test knob: `RidReceiver::setGate(false)`.
- **BLE is not gated.** Gating it lost about 1.4 % of the packets at 12 dB: the joins between bursts disturb
  its slicer near sensitivity. It got faster instead.
  - Block mixer: the phase at the block start times a table of the step's powers. There is no recurrence, so
    it vectorises.
  - At whole multiples of 4 Msps the filter runs in two stages: 2 MHz over 1.5 us at the input rate, then the
    same 650 kHz filter at 4 Msps. That is 58 instead of 131 taps per output sample, with the same delay.
  - Block demodulator with a polynomial atan2 (about 1e-6 rad).
- **802.11b chain** (core/src/wifi/wifi_rx.cpp).
  - The same block mixer.
  - At 20 Msps, the Barker correlations of a block are computed as one short FIR along it. They used to be
    computed one sample at a time.
  - **Fix:** a finished frame is now decoded inside feed(). Before, it was decoded only at the end of feed(),
    and a pending frame blocked the search, so every frame after the first in a large block was lost. This was
    found while measuring. It also affects the Wi-Fi mode, whose results now no longer depend on the block size.
- **OFDM receiver** (core/src/wifi/wifi_ofdm_rx.cpp).
  - Narrowband triggers are dropped. A BLE packet repeats after 16 samples like the short training does, but
    it also repeats after 8, which the short training does not. On BLE alone this cut the long-training searches
    from 6575 to 359 a second.
  - The lag-16 autocorrelation products are computed per block. The running sums are kept in locals.
- Viterbi and the FFT were not the cost (under 1 % each in the profile), so they were not changed.
- Skipping non-beacon frames early was not needed: the frame decode was not a measurable cost.

### Results

Mac, under load from other workers, generator signal, 2 s, both carriers, receiver fed in blocks of 262144:

| SNR | CPU before -> after | Messages before -> after | BLE packets before -> after | Wi-Fi RID frames before -> after |
|---|---|---|---|---|
| 30 dB | 0.966 -> 0.355 s/s | 1174 -> 2069 | 698 -> 697 | 119 -> 343 |
| 20 dB | 1.117 -> 0.326 | 1174 -> 2070 | 698 -> 698 | 119 -> 343 |
| 15 dB | 0.835 -> 0.311 | 1162 -> 2024 | 690 -> 692 | 118 -> 333 |
| 12 dB | 0.808 -> 0.304 | 730 -> 862 | 682 -> 682 | 12 -> 45 |
| 9 dB | 0.781 -> 0.298 | 308 -> 309 | 308 -> 309 | 0 -> 0 |
| 7 dB | 0.966 -> 0.291 | 7 -> 7 | 7 -> 7 | 0 -> 0 |

- The extra Wi-Fi frames come from the feed() fix. Fed in small blocks, the old code also found them.
- BLE is within one packet of before at every SNR.
- Alone: BLE 0.33 -> 0.12 s/s, Wi-Fi 0.59 -> 0.28 s/s.
- On noise alone the whole receiver takes 0.16 s/s.
- The generator's band is busy: Wi-Fi is handed 50 to 60 % of it. On a quieter band the gate saves more.

CI: quick.yml run 42937005020 (GitHub Linux) passed all 14 targeted tests (adsb_overlap, rid_cpu, rid_rx, rid_engine, ble_rx, ble_engine,
wifi_rx, wifi_ofdm, wifi_engine, wifi_56, adsb_rx, adsb_impair, adsb_deeprepair, real_adsb). ctest does not print the CPU figures of
rid_cpu, so the CI-core figure after the change was not captured. Scaled by the 2.5x Mac-to-CI ratio of the sensC measurement, the
estimate is about 0.9 s/s: real time, with little margin on one CI core.

### Found, not changed

The BLE demodulator decodes very differently depending on where the 4 Msps samples fall relative to the bits.
The generator puts all of its packets at one phase. With a filter delay off by 0.4 of a sample, a third of the
packets were lost at 9 dB. Real packets arrive at any phase, so real-world sensitivity is somewhere between
those figures. Fractional timing in the BLE decoder would fix it; that belongs to the BLE mode.

## Tests

- `tests/adsb/test_adsb_overlap.cpp` (`adsb_overlap`) covers:
  - the overlap scenario at 2, 2.4 and 8 Msps, with carrier offsets, 14 to 30 dB and error correction 1 and 2
  - the airspace at 150 and 300 aircraft
  - noise only
  - that the scenario repeats exactly
  - `ONAIR_TEST_LONG=1` adds more pairs, rates, SNRs and densities
- `tests/rid/test_rid_cpu.cpp` (`rid_cpu`) covers:
  - gate on against off at 30, 15 and 10 dB with +-40 kHz: the same BLE and Wi-Fi Remote ID messages and the
    same level
  - noise only (Wi-Fi fed under 20 %, no messages)
  - 802.11b frames independent of the block size
  - few OFDM triggers on BLE alone
  - CPU, printed only
  - `ONAIR_TEST_LONG=1` adds 8 to 20 dB at +-80 kHz
- Existing tests run locally against the changed code, all passing:
  - adsb_rx, adsb_impair, adsb_deeprepair, adsb_loud, adsb_gen, adsb_autogain, adsb_jam, adsb_ext, real_adsb
  - rid_rx, rid_proto, rid_alerts
  - ble_rx, ble_sensors, ble_proto
  - wifi_rx, wifi_ofdm, wifi_56, wifi_viterbi_nan, wifi_clients
- The engine tests (rid_engine, ble_engine, wifi_engine) do not start in the local standalone link, with or
  without these changes, so they were run on GitHub.
