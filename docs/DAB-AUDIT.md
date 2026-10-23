# DAB / DAB+ receiver audit: audio continuity (0.3.0)

Goal: smooth audio with barely any cutouts. The whole chain was measured on synthetic signals, the causes of dropouts were fixed, and the
measurement is now a regression test with ceilings per condition.

## Method

`tests/dab/test_dab_audit.cpp` (ctest `dab_audit`, label slow, about a minute on four cores):

- The generator (`dab_gen.cpp`, written from the standard, no code shared with the receiver) makes the clean ensemble: four services, DAB+
  48 kbit/s EEP-3A on sub-channel 1 (the one played), MP2 128 kbit/s on sub-channel 4.
- A channel model in the test (independent of the receiver) adds: white noise, static single frequency network echoes (inside and beyond the
  guard interval, a weaker transmitter heard first), three-path Rayleigh fading with Doppler (sum of sinusoids), flat slow Rayleigh fading,
  deep fades (signal gone), impulsive noise bursts, lost USB blocks (samples removed), an 8-bit RTL-SDR (quantisation, clipping) with tuning
  error and frequency drift, and sample clock error (the generator's resampler).
- The real `DabReceiver` decodes it; its audio is played by a model of the sound card: AudioOut's rules (playback starts at the start level,
  after an underrun again at 0.8 s), consuming in 10 ms blocks on its own clock.
- Measured where the listener is: a 10 ms block below -50 dBFS (or not there) is silent. **silence/min** = seconds of silent blocks per minute
  after the first sound, **cuts/min** = runs of silent blocks per minute. Next to them the per-stage counts: FIB CRC pass rate, access units
  lost (a superframe that did not decode counts all of its units), sync losses, superframes bad, Reed-Solomon bytes fixed and code words beyond
  repair, frames concealed, underruns.
- 20 s of signal per condition (`DAB_AUDIT_SECONDS` changes it), fixed seeds.
- `tests/dab/test_dab_playout.cpp` (fast) checks the playout alone: no click at any splice (largest second difference equal to the clean
  tone's), a short loss bridged at full level, a long one faded to silence, and the output buffer level against a sound card at -1000 to
  +1000 ppm over ten minutes.
- Service switching (in the audit): two switches while locked, the new service's first audio within 1 s, no sync loss.

## Results (20 s each; before = this branch's start, 1ca0cc98b + fix-dab-030)

| condition | before: AU lost / silence / cuts per min | after: AU lost / silence / cuts per min | ceiling (cuts, s) |
|---|---|---|---|
| clean, 20 dB | 0 % / 0 s / 0 | 0 % / 0 s / 0 | 0, 0 |
| AWGN 6, 5, 4.5, 4 dB | 0 % / 0 s / 0 | 0 % / 0 s / 0 | 0-3.2, 0-0.5 |
| **AWGN 4.5 dB, sample clock +100 ppm** | **no audio at all** (650 RS code words failed) | 0 % / 0 s / 0 | 3.2, 0.5 |
| **AWGN 4.5 dB, sample clock -100 ppm** | **no audio at all** | 0 % / 0 s / 0 | 3.2, 0.5 |
| AWGN 20 dB, +100 ppm | 0 / 0 / 0 | 0 / 0 / 0 | 0, 0 |
| **SFN echo 0 dB at 195 us (inside the guard)** | 12 superframes bad, 1319 RS fixes / 5.2 s / 6.2 | 0 / 0 s / 0 (0 RS fixes) | 0, 0 |
| SFN pre-echo -3 dB, main path 146 us later, 12 dB | 0 / 0 / 0 (30 RS fixes) | 0 / 0 / 0 (0 RS fixes) | 0, 0 |
| SFN echo -3 dB at 342 us (beyond the guard) | 0 / 0 / 0 | 0 / 0 / 0 | 0, 0 |
| mobile 3 paths, Doppler 20 Hz, 10 dB | 0 / 0 / 0 (85 RS fixes) | 0 / 0 / 0 (15 RS fixes) | 3.2, 0.5 |
| mobile 3 paths, Doppler 60 Hz, 18 dB | 0 / 0 / 0 (89) | 0 / 0 / 0 (6) | 3.2, 0.5 |
| mobile 3 paths, Doppler 100 Hz, 25 dB | 30 SF bad / **24.5 s** / 15.6 | 33.8 % / **15.1 s** / 22.5 | 30, 20 |
| flat Rayleigh 2 Hz, 12 dB | 12 SF bad / 5.2 s / 3.1 | 4.0 % / 1.6 s / 6.3 | 10, 3 |
| deep fade 250 ms every 4 s | 13 SF bad / 6.3 s / 6.2 | 6.3 % / 2.2 s / 12.6 | 16, 3.5 |
| deep fade 100 ms every 2 s | 3 SF bad / 3.0 s / 3.1 | 0 / 0 / 0 | 3.2, 0.5 |
| signal gone 1.5 s once (tunnel) | 7.6 s / 3.1 | 4.7 s / 3.1 (1.57 s heard silent for 1.5 s gone) | 3.2, 6 |
| USB block of 8192 lost every 2 s | 0 / 0 / 0 | 0 / 0 / 0 | 0, 0 |
| **USB block of 131072 lost every 3 s** | 3 sync losses / 8.8 s / 9.4 | 0 sync losses / 0.5 s / 6.3 | 10, 1.5 |
| **impulsive noise 100 us +15 dB, 100 Hz** | FIB 73.6 %, 22 SF bad / 8.8 s / 9.4 | FIB 100 % / 0 / 0 | 0, 0 |
| RTL-SDR 8 bit, +25 kHz, 80 Hz/s drift, 60 ppm, 12 dB | 0 / 0 / 0 | 0 / 0 / 0 | 0, 0 |
| MP2, Doppler 20 Hz, 10 dB | 0 / 0 / 0 | 0 / 0 / 0 | 3.2, 0.5 |
| MP2, deep fade 250 ms every 4 s | 5.5 s / 6.2 | 0.7 s / 6.2 | 10, 1.5 |

The "before" build fails 11 of the 24 ceilings. Cuts per minute rose in three conditions where the loss itself cannot be avoided (the 250 ms
fade wipes out about 13 logical frames through the 16 CIF time interleaving, i.e. four superframes): one long silence became shorter ones with
concealed audio in between; the silence per minute fell by two thirds or more in every one of them.

Playout (`test_dab_playout`): one or two lost access units bridged at full level, eight (320 ms) faded to silence and back, largest step 0.0086
(the clean 1 kHz tone's own); sound card at -1000 / -100 / 0 / +100 / +1000 ppm for ten minutes: the buffer stays at 17 000 - 26 000 frames
(target 19 200), no underrun (without the rate control +-1000 ppm drifts 600 ms in that time and runs dry or overflows).

## What was wrong and what changed

Receiver (`core/src/dab/dab_rx.cpp`):

1. **Sample clock error was not followed within a frame.** The windows were placed at U + l * 2552 and the fraction of a sample drifting
   between two symbols (0.26 samples at 100 ppm) turned the differential phase by up to 34 degrees at the band edge: 1.5-2 dB lost, at
   4.5 dB the whole MSC failed while the FIC (first symbols) still passed. Now the clock error is measured from how far each frame moves
   against the prediction (`sro`, shown in the app as "Clock ... ppm"), the windows follow the symbols and the left-over fraction is turned
   out per carrier; the next frame is predicted kFrame * (1 + sro) later.
2. **The FFT window followed the strongest path.** With two paths of nearly equal strength (a single frequency network) it jumped between
   them, and with a weaker transmitter heard first the earlier path's next symbol ran into the window. Now it goes on the earliest path
   within 10 dB of the strongest when all fit in the guard interval, and where the most path energy fits when they do not (`placeWindow`).
3. **Soft bits were normalised per frame**, so a faded frame (or one of pure noise) gave soft bits as confident as a good one and poisoned
   the 16 CIFs of time interleaving. Now they are scaled by a slow average over good frames: a fade gives weak soft bits, nearly erasures.
4. **A fade was taken for a lost sync.** Four frames without the phase reference ended the lock; the carrier offset was re-estimated from the
   cyclic prefixes of noise meanwhile. Now a frame whose phase reference symbol is far weaker than the good frames is a fade: timing and
   carrier offset are held and the frame is decoded with its weak soft bits, for up to 12 frames (1.15 s).
5. **Lost samples (a USB drop) cost the sync for 4 frames, then a full search, and the de-interleaver was out of step.** Now a frame of
   normal strength without the phase reference triggers a quick re-acquisition at once (only +-2 carriers around the known offset, half a
   frame of samples kept for it, decoding state kept); the CIFs missed go into the de-interleaver as erasures, counted from the CIF counter
   of FIG 0/0 (or from the samples when the FIC failed), so the logical frames around the gap still decode and the DAB+ superframe phase is
   kept. Over 20 CIFs lost, the decoding starts over.
6. **Impulsive noise** (ignition, switching) went straight into the FFT. A blanker zeroes samples more than 12 dB over the running mean power
   (OFDM itself goes there about once in 10^7 samples).

Audio (`core/src/dab/dab_audio.cpp`, new `dab_playout.{h,cpp}`):

7. **A Reed-Solomon code word beyond repair dropped the whole superframe** (120 ms, 2-6 access units). Its bytes are spread over the
   superframe (one in every N), so the access units whose CRC still passes are now used.
8. **Lost audio was just missing**: nothing was put out for a bad access unit or superframe, the buffer drained, and AudioOut re-buffered for
   0.8 s after every underrun. Now every lost unit is concealed in time: the last 40 ms repeated (crossfaded at every splice, the samples
   held back by 2 ms for it) at full level for 40 ms, then faded to silence over 120 ms; audio coming back is crossfaded in (faded in over
   10 ms after silence). After a loss that went silent one good unit alone is not played (no blips between silences).
9. **No gap filling**: during a sync loss the output ran dry. When less than 100 ms is queued and nothing arrives, the gap is filled, and the
   level is restored before the audio comes back, so the output never re-buffers.
10. **No rate control**: the sound card's clock and the transmitter's differ by up to 100 ppm (360 ms an hour). The playout resamples by up to
    0.2 % (cubic) to keep 400 ms queued.
11. **Start level 250 ms** was barely above the spacing of the audio bursts (120 ms superframes arriving in 96 ms frames, plus USB block
    jitter). Now 400 ms; after a service switch AudioOut's threshold is set back to it (it waited 0.8 s after every flush).
12. **The superframe phase was searched again after 3 bad superframes**: a fade of a few hundred ms then cost another second. Now after 8,
    and the receiver tells the audio when the phase really changes (reconfiguration, a long gap).
13. MP2 frames lost or erased are concealed the same way (24 ms each).

Status in the app (DAB panel): "Audio x % concealed" with a tooltip (Reed-Solomon fixed / beyond repair, access units ok / concealed, seconds
silent, sync losses and quick re-acquisitions, CIFs erased, impulse samples blanked, output queued and rate correction) and "Clock" (the
measured sample clock error in ppm). The lost-sample counter was already shown ("dropped").

## Checked and left as they were

- Transmission modes II-IV: not implemented. They were removed from EN 300 401 in V2.1.1 (2017) and no ensemble uses them; DAB has only
  DQPSK (no QPSK mode to audit).
- FIC / MSC coding: depuncturing of every EEP and all 64 UEP profiles, energy dispersal and the Viterbi decoder round-trip against the
  generator's own encoder (`test_dab_realworld`, `test_dab_gen`).
- Reed-Solomon erasure decoding: there are no erasure marks before RS in DAB+ (the CRCs are per access unit, after RS), and a lost logical
  frame puts 24 bytes into every code word, more than the 10 erasures RS(120,110) can fill. Not worth it.
- Coarse/fine frequency: +-75 kHz acquisition and an 80 Hz/s drift pass; the offset is now held through fades.
- Doppler 100 Hz (about 480 km/h in Band III): beyond mode I (carrier spacing 1 kHz, symbol 1.246 ms); the loss there is physical.
- Decoder threading and RTL-SDR USB buffers: the DAB receiver runs on the engine's decode thread; the source's lost-sample counter is shown
  in the panel. Lost blocks are now survived (above), the buffer sizes were not changed (they are shared by every mode).
- PPM: the measured clock error is shown; it is not applied to the device automatically (the correction is per device and shared by every
  mode).
- Band III scan with several ensembles: `test_dab_search` passes unchanged; the scan resets the receiver per channel, so the quick
  re-acquisition (which keeps state) never carries over to another channel.

Not verified on a real radio.

# Decoding sensitivity and lock time (dec-dab-030)

Goal: lock faster and decode at a lower SNR. Measured with `tests/dab/test_dab_sens.cpp` (ctest `dab_sens`, label slow, about 2 minutes on four
cores): the generator's ensemble through a channel model written in the test (white noise, TU6 / RA4 delay profiles with Rayleigh fading and
Doppler, a 0 dB echo), into the real receiver. **Threshold** = the lowest SNR (over 2.048 MHz; C/N in the 1.536 MHz of the signal is 1.25 dB
higher) at which at least 90 % of the FIBs pass their CRC (FIC) and at least 90 % of the DAB+ access units of the 48 kbit/s EEP-3A service decode,
6 s per point after 1.5 s of lock, 0.5 dB steps (AWGN rows: 0.125 dB steps, 12 s per point, `DAB_SENS_STEP=0.125 DAB_SENS_SECONDS=12`).
**Time to lock** = from a cold start at a random point of the transmission, 15 starts per SNR. `DECT2_DAB_BASELINE=1` runs the old decoding
in the same binary; that is how the "before" column was measured.

| condition | FIC before -> after | DAB+ before -> after |
|---|---|---|
| AWGN | 2.4 -> **1.6 dB** | 3.8 -> **2.9 dB** |
| AWGN, carrier +-5 kHz | 2.4 -> 1.6 dB | 3.8 -> 2.9 dB |
| AWGN, sample clock +-50 ppm | 2.4 -> 1.6 dB | 3.8 -> 2.9 / 3.0 dB |
| TU6, Doppler 10 Hz | 7.0 -> **5.5 dB** | 7.5 -> 7.5 dB |
| TU6, Doppler 60 Hz | 10.5 -> 10.5 dB | 8.5 -> 8.5 dB |
| RA4, Doppler 30 Hz | 11.5 -> 11.0 dB | 8.0 -> 8.0 dB |
| SFN, two paths of 0 dB, 146 us | 3.5 -> **2.5 dB** | 5.5 -> **5.0 dB** |

| time to lock, AWGN (median / p90) | sync | first FIB, service list | first DAB+ audio before -> after |
|---|---|---|---|
| 4 dB | 0.16 / 0.19 s | 0.16 / 0.19 s | 0.62 / 0.66 -> **0.52 / 0.56 s** |
| 10 dB | 0.16 / 0.18 s | 0.16 / 0.18 s | 0.61 / 0.66 -> **0.47 / 0.54 s** |
| 20 dB | 0.14 / 0.18 s | 0.14 / 0.18 s | 0.59 / 0.66 -> **0.46 / 0.51 s** |

Band III scan: a channel without DAB is left after 0.7 s of input with no null symbol in it (it was 2.2 s each), so a scan with a few ensembles
takes about 40 s instead of about 80 s. DAB at 0 dB SNR never reads as "nothing there" (`test_dab_search`).

What changed (`core/src/dab/dab_rx.cpp`):

1. **Soft bits from a channel estimate instead of plain differential detection.** The product of two noisy symbols costs about 3 dB. The channel
   is estimated at the phase reference symbol, then from every symbol with the decided symbols taken out, and cleaned in the delay domain: only
   the impulse response taps that stand out of the noise in the phase reference (12 times the median tap power, widened by two samples) are kept,
   which removes the noise and most decision errors from the estimate. The soft bits of each DQPSK symbol come from both received symbols,
   log-MAP over the unknown absolute phase of the first one (M(d) = log sum_s exp(2/sigma^2 Re(conj(s) (conj(H_l-1) y_l-1 + conj(H_l) y_l
   conj(d)))), the noise measured per symbol): coherent detection when the estimate is good, differential when it is poor, and blind to a 90
   degree slip of the estimate. Decisions use a straight-line prediction of the channel. A frame whose estimate changes fast (Doppler above about
   40 Hz) goes back to plain differential detection; the panel says which ("Detection: coherent / differential"). Log-MAP against max-log: under
   0.1 dB.
2. **DAB+ audio starts from 10 of the 16 CIFs** of time interleaving (the missing ones erased); a logical frame that does not decode is caught by
   Reed-Solomon and the CRCs. MP2 waits for all 16 (no check on its audio bits).
3. The scan's "no DAB here" (`DabTelemetry::quietMs`).

Tried and left out: **Reed-Solomon erasures for DAB+** from Viterbi reliabilities (a full errors-and-erasures decoder; the bytes ranked by the
survivor's decision margin, then by re-encoding disagreement). It repaired about half of the code words that failed at the threshold, but a
superframe needs all 6 of its code words and the threshold moved by less than the 0.125 dB step. Not kept.

Checked and left as they were: the Viterbi decoder already keeps the whole path (no truncated traceback) with 8-bit soft inputs; sync and the
service list come with the first frame after the null symbol (0.15 s: about one frame plus the search window, little left to win); the FIC has
no time interleaving, so its thresholds in fading (RA4, TU6 60 Hz) are set by the fades themselves. Caching the ensemble across retunes would not
start the audio sooner (the sub-channel table arrives in the first FIC). `dab_audit` passes unchanged (Doppler 100 Hz: the same as before, those
frames go differential).

Limits: synthetic signals only, not verified on a real radio. No competitor (welle.io, Qt-DAB, AbracaDABra, DABlin) publishes a threshold or a lock
time to compare with; these are the first such figures we know of. Packet mode FEC (FIG 0/14 FEC scheme 1, EN 300 401 5.3.5) is shown in the
analyser but not applied: it was written without the standard at hand and its layout could not be checked.

# DAB (MP2) services: garbled audio report from 0.2.9 (dec-dab-030)

Report: a user recording of a DAB (not DAB+) service on 11B, "only with UEP services": no dropouts but about 70 hard sample steps per second,
a DC offset of -0.06, nothing above about 10 kHz. UEP is only used by MP2 (DAB) services.

New test `tests/dab/test_dab_uep.cpp` (ctest `dab_uep`, label slow, 20 s): MP2 services through the generator and the whole receiver for every
one of the 64 UEP profiles (32 to 384 kbit/s, levels 1 to 5, mono where Layer II allows only mono), EEP A and B levels, mono and stereo, 48 kHz
and 24 kHz; every logical frame decoded must be the transmitter's to the bit, and the audio the tone (no DC, no step larger than the tone's own,
not more audio than the frames carry). All 64 UEP profiles and the EEP cases pass: depuncturing, time de-interleaving, Viterbi and energy
dispersal of UEP are right (the profile table was also compared again, row by row, with the standard's table 8 as we know it; the generator
shares those tables, so the round trip alone could not have caught a wrong row).

What was wrong: **MP2 at 24 kHz** (MPEG-2 half sampling rate, allowed by EN 300 401 for DAB audio and used for low-rate speech and community
services, typically the low-bit-rate UEP ones). Such a frame is 48 ms long and spans two logical frames; every 24 ms logical frame went to the
decoder as if it were a whole frame. The first half decoded with the rest of the frame missing (garbage in the upper part of every frame), the
second half failed and was concealed by repeating the last 40 ms: 1.5 times as much audio as sent, garbled, with clicks at every splice and a
band limit of 12 kHz, which matches the report. Now a logical frame whose header announces a frame of two logical frames is kept and decoded whole
with the next one; everything else goes to the decoder exactly as before, so 48 kHz does not change (a first version that cut frames out of the
byte stream by their headers cost one cutout per fade in dab_audit's MP2 deep fade case). The generator makes 24 kHz MP2 and mono MP2 for the test.

Also: **scrambled services** (conditional access, FIG 0/2 CAId and the component's CA flag) were played as noise. The flags are now read
(`DabService::caId`, `DabComponent::ca`); a scrambled station is not played and the panel says "Audio: scrambled (conditional access): cannot be
played" (`DabTelemetry::scrambled`). Samples that are not numbers (NaN, infinity) are now zeroed on input (the channel estimate would spread
them; `dab_realworld` failed on x86-64 Linux for that).

Not verified with the user's signal: the recording is the decoded audio, not IQ, so whether that service is 24 kHz MP2 is inferred from the
symptoms (nothing above 12 kHz, garbling without dropouts). An IQ recording of the ensemble would confirm it.

## Shared harness (tools/sens, docs/SENSITIVITY.md)

`tools/sens/run.sh --full --mode dab` (its own channel models, SNR over the 1.536 MHz of the signal, 1 dB steps, FIB CRC as the metric, 3 s
trials; before = `DECT2_DAB_BASELINE=1`, 12-40 trials a point). The committed baseline (`tests/sens/baseline/sens-baseline.json`) holds the "after".

| scenario | before | after |
|---|---|---|
| AWGN, carrier 2 / 20 ppm | 3 dB (31 % FIBs bad at 3 dB) | 3 dB (7 % bad at 3 dB) |
| off centre by 154 kHz | **no lock in 3 s** at any SNR | 5 dB, lock 0.64 s |
| sample clock 50 ppm | 4 dB | 3 dB |
| echo -6 dB at 30 us | 4 dB | 3 dB |
| TU6, 20 Hz | 9 dB | **5 dB** |
| Rician K = 10 dB | 4 dB | 3 dB |
| phase noise 50 Hz | 6 dB | 5 dB |
| combined (2 ppm, 20 ppm clock, echo, phase noise) | 7 dB | 6 dB |
| lock, median / p90 | 0.19 / 0.19 s | 0.19 / 0.19 s |

With 12 trials a point the phase noise and combined rows vary by +-2 dB from run to run; the values above are from 40 trials. The two
measurements agree: dab_sens's FIC threshold of 1.6 dB over 2.048 MHz is 2.85 dB over 1.536 MHz, inside the harness's 3 dB step.

Two more changes came from the harness: an ensemble beyond the +-80 kHz of the search is now moved to the middle after 0.5 s without lock
(was 1.5 s, too late for a 3 s trial and a slow start for the user), and the common phase error of each symbol (oscillator phase noise) is
measured from the fourth power of the differential cells and taken out of them where there is a signal. Taking it out of the channel
estimate too cost 0.3 dB at the FIC threshold and was left out.
