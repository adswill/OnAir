# Transmit: full control (3.8, branch feat38-txcontrol)

The owner asked for full control over transmitting: every parameter a user can set, under Advanced, with one-click defaults on top; a per-radio TX
calibration; a live meter and spectrum of what is sent. No new transmit modes were added (MeshCore and DMR transmit are other workers' branches).
The owner's TX rules hold throughout: OnAir warns and never blocks, the user's Start (Arm) is the decision, there is no band, licence, frequency
or power refusal. A value is clamped only to the scale the radio itself reports.

## The audit: what could be set before this branch

The transmit chain is `TxUi` (app/panels/tx_state.h) → `makeReq` → `TxRequest` / `TxTune` (core tx.h, source.h) → `TxSession` → the radio's `TxSink`.

### Radio parameters, per driver

| Parameter | HackRF (own USB driver) | PlutoSDR / AD936x (libiio) | USRP (UHD) | SoapySDR (LimeSDR, bladeRF, XTRX, USRP, ...) | Hamlib rig |
|---|---|---|---|---|---|
| Frequency | set | set | set | set | set (CAT) |
| Offset (Advanced) | set | set | set | set | set |
| Frequency correction (ppm) | **hidden**: the driver read `TxTune::radio["ppm"]`, the app never filled it | **hidden** (same) | **hidden** (same) | **hidden** (same) | n/a |
| Gain | TX VGA 0-47 dB | hardwaregain −89.75-0 dB | overall | overall only | n/a (PTT) |
| Gain stages | n/a (one VGA) | n/a | **fixed** (overall) | **hidden**: no per-stage gain | n/a |
| Amplifier | +14 dB switch | n/a | n/a | n/a | n/a |
| Output (TX1/TX2) | TX1 | TX1/TX2 | TX1..n | TX1..n | n/a |
| Sample rate | Auto / manual | Auto / manual (shared with RX in duplex) | Auto / manual | Auto / manual | audio rate |
| Analogue filter | **fixed** automatic: `TxTune::bandwidthHz` existed, the app never set it | **fixed** automatic | **fixed** automatic | **fixed** automatic | n/a |
| IQ balance, carrier leak | **none** | **none** | **none** | **none** | n/a |
| Power vs gain | **none** | **none** | **none** | **none** | n/a |
| Change while sending | frequency, gain, filter, amp | **refused** ("cannot change its transmit settings while sending") | **refused** | frequency, gain | frequency |

LimeSDR and bladeRF have no native transmit path in OnAir: they transmit through SoapySDR (`makeTxSink`: HackRF, Native = PlutoSDR and USRP only,
SoapySDR, Hamlib). That is unchanged.

### Signal parameters, per family of modes (127 rows in `txModes()`)

| Family (`TxPayloadKind`) | Settable before (payload panel + Advanced table, tx_params.cpp) | Fixed / hidden before |
|---|---|---|
| Analog voice: airband AM, QO-100 SSB, the analog fall-back of digital voice and scene modes (NFM) | audio source (mic, WAV/MP3, tone, silence), mic gain, test tones, compressor, high/low-pass, VOX + threshold + hang, PTT hold, AM type/depth/carrier, SSB sideband/carrier, FM deviation, pre-emphasis, CTCSS (50 tones), RF level | **DCS**: no code could be sent; CTCSS level fixed at 12 % |
| FM broadcast | deviation, pre-emphasis, stereo + pilot, RDS (PS, RT, PI, PTY, TP, TA, CT, M/S) + level, test tones, RF level | none found |
| FT8/FT4/WSPR/JT*, RTTY, CW, PSK, keyboard modes, JS8 | message fields, baud/shift/WPM/variant, audio offset, sideband | none found |
| LoRa APRS | presets or own SF/BW/CR/preamble/sync/CRC, repeat gap | none found |
| TV (DVB-T2/T, ATSC, ISDB-T, DVB-S, DTMB, DAB, analog TV) | programmes, files, cameras, names; channel width | the mode's modulation (FFT, guard, constellation, code rate ...) sat in a folded "Receiver-style options" node outside Advanced |
| Data modes on `kOpt` (AIS, ACARS, pagers, packet, VDL2, ...) | the mode's own fields (IDs, text, position) and its generator options | as above: generator options outside Advanced |
| KAAL/KAIT/KAVB/KAPP/KAFT/KAWS, Mesh | their own panels (profiles, passes, files, identity) | none found |
| Every mode | output trim, mirror spectrum, key-up fade, repeat until stopped / seconds, TX sample rate, carrier tune, dry run, IQ file, IQ over TCP | **timeout timer**: none; **schedule** (start at a time, repeat): none |

The live meter (peak, RMS, limiter count), the spectrum and waterfall of what is being sent (the session's tap of the IQ handed to the radio), the
constellation and the occupied bandwidth already existed (tx_step_send.cpp, tx_preview.cpp); they are kept, and the output power is added to them.

## What this branch adds

Core (`core/include/dect2/tx/tx_control.h`, `core/src/tx/tx_control.cpp`):

- **Per-driver plans.** `hackrfTxPlan`, `plutoTxPlan`, `usrpTxPlan`, `soapyTxPlan` work out what each radio is asked for (rate, filter, frequency
  after offset/ppm/crystal correction, gain stages in order, amplifier, output, connector) from a `TxTune` without touching hardware. The HackRF sink
  writes its plan through `txHwApply` and a `TxHwOps` adapter over its USB requests; the PlutoSDR, USRP and SoapySDR sinks take every value from their
  plan. The values are the same as before for the same input (the old inline code moved into the plans).
- **Live retune** on the PlutoSDR (LO, attenuation, filter; the rate stays, it is shared with the receiver in duplex) and the USRP (frequency, gain,
  filter). Before, both refused any change while sending.
- **SoapySDR gain stages**: `tune.radio["txgain.<stage>"]`, written after the overall gain, each clamped to the range the driver reports for it;
  a name the driver does not list is still sent, with a note in the log.
- **Calibration** (`TxCalibration`, saved per radio): ppm, a fixed Hz offset, DC on I and Q (carrier leak), I/Q gain and phase balance, and a measured
  power table (gain, amplifier, dBm). `txApplyCalibration` puts it into a request; values the request already sets win.
- **IQ correction** in the output stage (`out.iqGainDb`, `out.iqPhaseDeg`, `out.dcI`, `out.dcQ`, every mode and radio): I′ = aI, Q′ = b(Q cos p + I sin p)
  + dc. All zero leaves the samples bit-identical.
- **Timeout timer** (`tx.totSec`): ends any transmission by itself, with the key-down fade, even with "Repeat until stopped"; logged.
- **Schedule** (`TxSchedule`, `txScheduleNext`, `txParseUtcTime`): first start at a UTC time, then every N seconds, N times or until Stop.
- **DCS** on analog FM (`fm.dcsIdx` / `fm.dcsCode`, `fm.dcsInv`, `fm.dcsPct`): the 23-bit Golay word of the code (`actl::dcsWord`, the receiver's
  own) at 134.4 bit/s, NRZ, two poles at 300 Hz, in place of CTCSS. The code list is the receiver's 83 usual codes (`tones::dcsStandard`).

App (`app/panels/tx_control_ui.cpp` and the existing TX panel files):

- Advanced keeps "Reset to default" on top as the one-click default; it now also puts the filter and gain stages back to automatic and clears the
  schedule (the calibration is kept: it is measured, not a preference).
- Advanced gains: **Modulation of this mode** (the generator's own settings, moved in from Basic), **Radio** (filter Automatic / kHz; gain stages
  for SoapySDR radios), **Schedule** (start at, repeat every, times in all), **Calibration of this radio** (ppm with "Use the receiver's ppm" when the
  receiver uses the same radio, Hz offset, I/Q gain and phase, carrier leak I and Q, the power table with "Add the current level").
  New rows in the table: Timeout timer (every mode), DCS code / inverted / level and CTCSS level (analog FM).
- Next to the output level: the power a full-scale carrier would have at this level (from the table) and **Set the level for this power**; a gain
  outside the radio's scale is a note, the radio holds it at its own limit.
- The live meter adds **Output power**: average and peak envelope power predicted from the table and the signal's own RMS and peak.
- Start with a schedule arms it (the START button shows STOP while it waits); Stop, Space or Esc cancel it. A scheduled start that finds the radio
  in use cancels the schedule with a message.

## Tests (tests/tx/test_tx_control.cpp, registered as `tx_control`)

All in memory; nothing is transmitted. Run standalone against the prebuilt libraries: **all passed**.

- HackRF: 2,592 combinations of rate, gain (below, inside, above the scale, fractional), amplifier, ppm, offset and filter through `txHwApply` into a
  fake radio that records every call: call order, rate fraction, frequency, VGA rounding/clamp, amplifier, filter. Retune skips rate and filter; a
  refused call stops the sequence with the radio's words; TX2 and a frequency the request format cannot hold are refused in words before any call;
  a negative frequency becomes 0 Hz with a note; 1 Hz to 1 THz and band edges are planned without refusal.
- PlutoSDR (160 combinations), USRP, SoapySDR: filter clamps, crystal correction, gain to the reported range only, defaults, stages and their clamps,
  unknown stages, retune calls.
- Calibration: text round trip, power interpolation and extrapolation, inverse over −5..52 dB for both amplifier settings, request merge; the full
  path calibration → `TxSession` → the sink's `open()` → every driver's plan gets the corrected frequency.
- IQ correction: bit-identical at zero; three mixer models (gain error to 5 %, phase error to 6°, carrier leak): image −29.2 / −24.3 / −25.6 dBc
  corrected to −58.3 / −59.9 / below −150 dBc, carrier leak −32.6 / −28.0 / −30.4 dBc corrected to −61.8 / −64.6 / −66.9 dBc, by a grid search of
  the controls (0.05 dB, 0.1°, 0.001 steps: what a user does by eye).
- Timeout: 0.02 / 0.1 / 0.25 s on a repeating transmission end it by itself, with exactly that many samples (±1), the fade at the end and a log line.
- Schedule: start time, interval, count, overrun, past start, HH:MM parsing.
- DCS: all 83 codes generated, every fifth inverted, with ±1.5 kHz / −0.7 kHz frequency offsets, two noise levels and a CTCSS tone set as well, are
  **heard by the receiver's own DCS detector (83 of 83)**; a different code is not taken; no DCS when none is chosen.

Added to the existing fake radios (run in CI, not locally): the SoapySDR test driver records named TX gain stages (test_tx checks TOTAL clamped to
its range and an unlisted stage passed as typed); test_pluto_duplex checks the PlutoSDR TX retune on the fake libiio (LO and attenuation moved, the
shared rate untouched, the filter set). Existing tests test_tx_params and test_tx_polish pass with the changes.

**Validated on simulated signals only.** No radio was connected; nothing was measured on air. The UI was syntax-checked and passes the source
GUI rules (test_gui_rules); it was not run (no app builds during the overnight limits). New strings use tr() / N_(); they are not yet in the
translation catalogs.

## Not done, and why

- **Hamlib rig RF power**: a rig is keyed and tuned only; its own power setting (Hamlib RFPOWER) is not sent. Left for a rig-control worker.
- **USRP named gain stages** (UHD's "PGA" etc.) and the **SoapySDR TX antenna** choice: the plans support stages; only SoapySDR exposes them in the
  panel. Not reached.
- **SoapySDR filter change while sending**: frequency and gain retune live; the filter is set at start.
- **DCS turn-off code** (the 134 Hz tail some radios send before unkeying): not sent.
- **Per-mode modulation review**: the 100-odd `kOpt` modes expose what their generator's own settings UI offers (now under Advanced); each mode's
  generator was not reviewed for further hidden constants.
- No new transmit modes (by the brief).
