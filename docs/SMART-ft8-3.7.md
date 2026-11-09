# Smart FT8 and RTTY (3.7, package ft8, proposals 1-5 of hamvoice.md)

Code: core/src/hfdig/hfdig_ftx.cpp, hfdig_rtty.cpp, hfdig_rtty_auto.cpp; UI: app/hfdig/hfdig_ftx_ui.cpp, hfdig_rtty_ui.cpp.
Tests: tests/hfdig/test_hfdig_ftxsmart.cpp, test_hfdig_rttyauto.cpp. Environment switches (for measuring): HFDIG_FTX_XSLOT=0, HFDIG_FTX_APQSO=0, HFDIG_FTX_EARLY=0.

## 1. Cross-slot soft combining (FT8, FT4, FT2) - done
Candidates that fail in the deep stage keep their three soft-bit sets (plain, 2- and 3-symbol coherent) for the last 6 slots. A later candidate within 3 Hz and 0.4 s DT
of a stored one (same sideband, other slot) is decoded again on the sum of the soft bits (each single earlier slot, then all together): BP, then OSD behind the existing
plausible-message guard. Shown as "(combined)". Measured (one station repeating a non-CQ message every slot, 3 noise seeds x 8 slots, decoded slots of 24):

| SNR (2500 Hz) | off | on |
|---|---|---|
| -20 dB | 15 | 18 |
| -21 dB | 8 | 16 |
| -22 dB | 1 | 10 |
| -23 dB | 0 | 7 |
| -24 dB | 0 | 0 |

About 2 dB on repeated messages; no false decodes.

## 2. QSO-aware a-priori - done (from the conversations on the band)
Messages decoded in the last 8 slots give hypotheses: "A B ..." heard -> both "A B ???" and "B A ???" (61 known bits: both calls and the type), and every "CQ X" caller ->
"X ???" (32 known bits). Candidates with sync score >= 2.0 are tried with the known bits forced (BP, OSD for the pairs), gated by plausible(). Shown as "(a-priori)".
Test: a pair heard strong, then a weak R-report of one of them (4 seeds): decoded 4/4 at -21 and -22 dB, 2/4 at -23 dB; with it off 0/4 at all three.
Noise-only slots after a conversation was heard (88 slots, see test): no false decodes. Not done: the operator's own call / "active QSO partner" from the log (no engine hook; the heard table covers it).

## 3. Early decode - done (FT8, FT4)
When 85 % of the transmission has been heard and the decoder is idle, a plain-stage decode runs on the audio so far; symbols not yet heard are erasures (zero soft bits, left out of the
noise estimate and SNR). Decodes appear about 4-5 s before the slot-end decode (FT8: ~11.3 s instead of ~16 s into the slot), marked "(early)", are not spotted or counted as slots,
and are replaced by the full decode of the same message. Checkbox "Early decode" (default on); off for whole-span sub-decoders. Costs one extra plain decode per slot.

## 4. Beacon frequency calibration - dropped as proposed
The receiver ppm calibration already exists (selfcal: ToneMeter, FM/ATSC/cell/GNSS references, per-radio Store, Tools > Calibrate frequency). FT8/WSPR audio frequencies carry no absolute
reference (stations choose their own), so a statistical correction from decodes is not possible, and a beacon table would need verified data I do not have. The only new thing would be
a time-station/IBP reference feeding selfcal; left for a separate item.

## 5. RTTY auto-lock - done
rttyAutoAnalyse (HfdigRtty::autoLock, button "Auto lock"): finds the spectral lines (parabolic peaks), pairs them at a table shift, and runs the decoder over the last <=20 s for every speed
and both polarities; the text score (good characters minus 2.5 per framing error) ranks them; the best is applied, runner-ups listed. 40 combinations (4 speeds x 5 shifts x 2 polarities,
tones 400-2200 Hz) at 15 dB: speed, polarity and tone place right in 40/40, exact shift 39/40 (170/200 and 425/450 Hz differ by 25-30 Hz and decode alike: one miss). 8/8 at 10, 5, 2 and 0 dB
on two test signals. Noise and a steady tone: no lock in 6 runs. PSK and Hell auto-lock were not done.

## Verification note
Local checks used the decoder objects directly; the full engine tests (test_hfdig_ft8, ftxmulti, rtty) run on GitHub CI.
