# General / Auto: tune around, OnAir spots the signal

In the General receiver (family 55) OnAir looks at the spectrum in the background and labels every signal it recognises.
Press **Decode** on a label and the radio is tuned and set up for that mode, but you stay in General: tune away from the
signal and OnAir goes back to the General receiver (a plain spectrum, no decoder). Tune back and the label is offered again.

## What you see

- A label on the spectrum and the waterfall, over the signal: `DVB-T / T2  7.56 MHz  96%` (name, measured width, how sure),
  a tint over the signal's width, a **Decode** button and an **x** (ignore this label). Hover a label for the details.
- The corner button **Signals (n)** opens a compact list: frequency, name, width, confidence, Decode, Ignore. Signals seen
  earlier outside the current view are listed greyed out; Decode on one tunes there.
- While a decoder runs a banner shows `Decoding DVB-T / T2 (stays on within +-1.9 MHz)` and **Back to General**.

## Rules

- A label shows after the signal was seen in two scans in a row, or at once when the identifier is at least 90 % sure,
  says "likely" and its own detector verified it. It goes after three scans without the signal (six for bursty signals).
  A different name replaces a label only when seen twice in a row, so a label does not flicker between two guesses.
- Scans: one every 5 s on the identifier's own thread (`sigid::Capture::startAll`, 0.6 to 2 s of samples, at most 12
  signals), only in General, only when the Identify window is not open; a retune triggers a scan after 0.8 s.
- **Keep-decoding distance:** the decoder runs while the tuned frequency is within the larger of 25 kHz and a quarter of the
  signal's width of the signal (Advanced: set a number of kHz instead). Further away: back to General, same settings
  (demodulator, rate) as before the Decode, on the new frequency. A radio that is still moving after Decode is not
  taken for "tuned away" (2 s settle time).
- **Auto-decode** (off by default, checkbox in the list): when the tuned frequency is inside that distance of a label that is at
  least 80 % sure and was not rejected by its detector, Decode starts by itself. After "Back to General" it does not restart
  until you have left that signal.
- **Ignore** hides a label (and that name at that place) for the session; "Show ignored signals again" is under Advanced.
- Choosing another mode by hand ends the session without any action.

## Where it is

- State machine: `core/src/engine/general_auto.cpp` (pure, no GUI; `tests/genauto/test_genauto.cpp` drives it with synthetic scans
  and one end-to-end run of real generator IQ through the identifier thread).
- App: `app/specx/general_auto_ui.cpp` (frame loop, labels, list); preferences `genAutoOn`, `genAutoDecode`, `genAutoReleaseKhz`.
- Identifier additions: SSB voice row (speech-envelope check, USB above 10 MHz, LSB below, dial at the missing carrier);
  DVB-T / T2 at the native 9.14 MS/s of an 8 MHz channel checked in `tests/genauto/test_idgaps.cpp`.

## Limits

Identification is the classical-DSP identifier of docs/IDENTIFIER.md: calibrated on OnAir's own test signals, not on every
real signal. DVB-T and DVB-T2 share one label (both open the same DVB mode, which finds out which it is).
SSB sideband is a convention for the band, not measured.
