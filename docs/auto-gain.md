# Auto gain (AGC)

> The app now uses the unified auto gain of [AUTOGAIN.md](AUTOGAIN.md) (Off / Auto / Optimise once / Safe start). SmartAgc below stays in
> core and is still tested; the amp permission switch (`agcamp.<radio>`) is shared.

The AGC keeps a radio's front end at the best operating point for the active mode. It lives in `core/include/dect2/source/agc.h` and
`core/src/source/agc.cpp`; the app glue (gear button, per-radio amp switch, applying the decisions) is `app/panels/agc_ui.cpp`.
The old `AutoGain` class in `gain.h` is no longer used by the app (the tests keep it).

## What it measures

Every tick, from the ADC statistics and the spectrum frame (`measureFront`):

| Measurement | Used for |
|---|---|
| ADC peak, clipping fraction, rms | overload (fast attack), hot / low level |
| noise floor over the ADC's quantisation noise (`adcMarginDb`, the station-health model in `core/src/sweep/station_health.cpp`) | "gain buys sensitivity" (margin too small) or "gain wasted" (margin large) |
| power outside the channel and its skirts, and the strongest bin out there | strong neighbours (blockers) |
| DC spike over its surroundings | reported; it is not allowed to hide a real overload |
| the decoder's own SNR / MER / Es/N0 while locked (`rxSnrDb`) | freeze while locked; undo an upward step that made the decoder worse |

## What each mode wants (`agcProfileFor`)

| Class | Modes | Aim |
|---|---|---|
| wideband | channels 1 MHz or wider (DVB-T/T2, ATSC, ISDB-T, DAB, DTMB, analog TV, Wi-Fi ...) | ADC headroom: rms about -16 dBFS, dead band +-4 dB, peaks under 0.8 |
| narrow | everything narrower (weak-signal and digital voice/data modes) | lowest noise: floor 14 dB over the ADC's own, rms ceiling about -22 dBFS, strong neighbours kept under -22 dBFS |
| pulsed | ADS-B, Mode S | peaks decide (under 0.7), margin 12 dB |
| strong | FM broadcast, HD Radio, airband | front end protected: LNA limited to 24 dB, neighbours under -12 dBFS |

A converter deeper than 8 bits lowers the aim by 1.5 dB per extra bit (more room for strong signals), at most 8 dB.

## Gain stages (`GainModel`, per device)

| Radio | Model |
|---|---|
| HackRF One / Pro | LNA 0-40 dB (8 dB steps), VGA 0-62 dB (2 dB steps), amp +14 dB (switch). The overall gain is split **LNA first** (best noise figure), the VGA takes the rest and keeps a little to trim with. A strong neighbour lowers the LNA limit (the VGA, after the baseband filter, carries the gain instead). |
| RTL-SDR, rtl_tcp | the R820T tuner gain table (29 steps, 0 to 50 dB); only table values are chosen. |
| AirspyHF+ | 0-48 dB in 6 dB steps; the driver's "auto" preamp (+6 dB) switches on in the top 6 dB, so without permission the AGC stays at 42 dB. |
| everything else (Airspy, SDRplay, Pluto, Lime, bladeRF, USRP, ...) | one overall gain over the range the driver reports; the driver spreads it over its own stages. |

## Control loop (`SmartAgc`)

* **Overload**: clipping above 0.2 % or a peak at full scale cuts the gain at once (6 to 20 dB by how bad it is), ignoring the settle time, the lock
  freeze and the rate limits. A soft ceiling then keeps the gain from climbing straight back (10 s; 90 s when it overloads three times in 30 s).
* **Recovery**: after a burst the gain climbs back toward where it was (up to 4 dB per 1.2 s); a quiet signal is not left low.
* **Release is slow, attack is fast**: up steps are at most 6 dB every 2.5 s, down steps up to 10 dB every 0.8 s.
* **Dead band and hysteresis**: nothing happens while the level is inside target +- window; a reversal within 8 s needs a clearly larger error;
  a step that would overshoot the opposite edge (a 6 dB-step radio) is not taken. Stationary input gives no changes.
* **Settling**: 0.8 s after every change before judging again; the level is measured afresh.
* **Frozen while a decoder is locked** on good data (SNR above the mode's limit, or no SNR figure); only an overload or a level more than the
  window above the dead band still acts. A lock with a poor SNR does not freeze it. An upward step that makes a locked decoder 1.5 dB worse is undone.
* **Retune / mode / radio change** starts a fresh acquisition (8 s of faster steps, up to 24 dB at once) with the new mode's profile.
* **Blockers**: the power outside the channel is kept under the mode's ceiling; when it comes within 8 dB the HackRF's LNA limit steps down by 8 dB
  per 6 dB of excess (and back up with 3 dB of hysteresis).

## The amp

The amp is **off by default for everyone** (new installs, the scanner tool, new mode defaults) and the AGC **never touches it** unless the user
allows it. Next to the AGC checkbox a small gear opens a popup with the switch **Let AGC turn the amp on/off by itself** (default off, saved per
radio as `agcamp.<radio>` in the settings). Without it:

* the AGC, "Optimise gain" and "Auto-tune" move only LNA and VGA; an amp the user switched on stays on (its 14 dB are part of every total);
* the AirspyHF+ preamp is kept out of the AGC's reach (42 dB top).

With it, the amp can come on when 70 dB or more is wanted and nothing strong is near, goes off again below 56 dB or when a neighbour
appears, and is the first thing to go on an overload (then banned for 2 minutes). Turning it on or off never changes the total gain.

When the user switches the amp on by hand, a note in amber appears next to the switch: *The amp can fry itself on strong signals while on*.

## Tests

`tests/source/test_agc.cpp` with the simulated front end `tests/source/agc_fake.h` (a gain model, noise whose figure falls with LNA gain, blockers
that desensitise the LNA, a DC spike, an ADC that clips and quantises, a decoder that locks above 8 dB SNR). It checks: the per-device tables and
splits; the mode profiles; the measurements; convergence from four start gains inside a few steps and 12 s; no hunting over 300 s of
noisy stationary input on HackRF, RTL-SDR and 6 dB-step radios; recovery after strong bursts; the amp never switched on without permission, the
user's amp left alone, the allowed amp coming on and going off on an overload; blocker protection (LNA limited, neighbour under the ceiling);
the weak-signal margin goal; the lock freeze; reacquisition after a retune.

**Not verified on hardware.** The thresholds are rules of thumb tuned on the simulation; the tuner table of radios other than the R820T
(E4000 etc.) is the R820T's, and a driver that snaps differently will still work (it takes the closest value).

## Gain stages by hand (#233)

The gain slider stays the simple default and moves every stage together. Radios that advertise their stages also list them under "Radio settings",
one setting each, "-1" or untouched meaning "follow the slider"; a stage set by hand is applied after the slider's gain, so the slider still moves
the others:

| Radio | Settings |
|---|---|
| any SoapySDR radio with 2+ gain elements | `gain.<element>` (LNA, VGA, TIA, PGA, ...) |
| HackRF | LNA, VGA and amp sliders (always there) |
| Airspy R2 / Mini | `gain.lna`, `gain.mixer`, `gain.vga` (0..15; libairspy's own stage calls) |
| SDRplay RSP | `lnastate` (RF/LNA gain reduction state, band dependent) and `ifgr` (IF gain reduction 20..59 dB) |
| Fobos SDR | LNA step (`lna`) |
| RTL-SDR | RTL AGC switch (the library has no per-stage call for the common tuners) |
