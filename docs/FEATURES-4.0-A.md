# 4.0 mode features, group A: broadcast and media

Mode-specific features, each with a pure function (header in core/include) and a unit test (tests/<dir>). All tests use simulated signals only; nothing here was checked on air.

| Mode | Feature | Why | Code | Test |
|---|---|---|---|---|
| DVB-T, DVB-T2 (ISDB-T, DTMB share the profile) | Echo table: delay, level, extra path in km, and a verdict per path: pre-echo (locked on a weaker, later transmitter), beyond the guard interval (ISI, caps the SNR), inside it (harmless), with what to do | The impulse-response plot existed, nothing read it for you | `dect2/t2/tv_diag.h` `findEchoes`, Analysis > Impulse | `tests/tvdiag` |
| DVB-T, DVB-T2 | Narrowband interferer finder on the per-carrier SNR: frequency, depth, width; told apart from a broad multipath notch or tilt | Shows LTE / link / ingress that a mean SNR hides | `tv_diag.h` `findCarrierDips`, Analysis > SNR | `tests/tvdiag` |
| FM | Co-channel detector: two PIs that alternate on one frequency (>= 4 reads and >= 15 % each, so bit errors do not count), with each PI's SNR | Tells a shared channel (tropo, Es) from one station | `dect2/fm/fm_dxtools.h` `PiWatch`, census panel | `tests/fmdx40` |
| FM | Adjacent-channel risk: a station 100 / 200 kHz from one 20 dB stronger is flagged | Explains missing RDS / splatter | `fm_dxtools.h` `adjacentRisk`, census panel | `tests/fmdx40` |
| FM | Antenna direction helper: type headings while turning the antenna, get the best heading, gain over the average and lobe width | A DXer finds the best bearing without a notebook | `fm_dxtools.h` `bestHeading`, census panel | `tests/fmdx40` |
| DAB | Ensemble change watch: new / gone ensemble, services added or removed, bit rate or DAB / DAB+ change, ensemble ID change; ignores half-received FIC and single-update glitches | Notice a multiplex reconfiguration without comparing lists | `dect2/dab/dab_watch.h`, "Ensemble changes" in the DAB census | `tests/dab40` |
| Weather satellites | Pass quality 1 - 5 (elevation, length, night for visible products, horizon) with the reason in a tooltip, in the Peak column | Pick the passes worth recording | `dect2/wxsat/wxsat_pass.h`, passes window | `tests/wxsat40` |
| General receiver | "What is this signal?": occupied bandwidth, carrier line, symmetry, flatness, humps -> ranked guess (AM, NFM, WFM, SSB, CW, FSK, OFDM) with a Use button that sets the demodulator and tunes onto the signal | An unknown signal gets a mode and a bandwidth in one click | `dect2/general/sig_guess.h`, General controls | `tests/general40` (real modulated IQ, frequency offsets, noise-only false-guess check) |

Already in 3.x and therefore not rebuilt: FM census and DX alerts (fm_dx.h), first-adjacent cancellation, DAB census / SFN / TII, PSIP, DRM finder, AM census, per-mode presets, Auto setup / Reset with undo, results export.

## Limits
- The SSB sideband is a convention (band above the tuned frequency = USB); the spectrum alone cannot tell.
- Pass quality uses sun elevation at the highest point as the daylight proxy.
- GUI parts were syntax-checked only (the app was not built, per the load rules); the logic is what the tests cover.
