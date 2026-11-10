# Hellschreiber fixes for 3.8 (issue #225)

Branch `feat38-hell`, based on `integ-38`.

## What was reported (BI4PYM, on 3.7.0)

1. Hell is on/off keyed, so the transmitted spectrum should peak at the chosen audio frequency (for example 1500 Hz above the SSB carrier). OnAir's transmitted signal looked asymmetric.
2. To receive, the user had to click 1500 Hz away from the signal. fldigi keeps the dial fixed and lets you click the signal itself.
3. In dual-line view the second copy came later. It should be drawn at the same time as the first.

The screenshots attached to the issue were not opened, because downloading them needed approval. The analysis below comes from the code.

## Root causes

- **Receive: click to tune.** Since #219, a click on the spectrum makes the clicked frequency the dial. For SSB audio modes the signal sits at dial + audio (USB) or dial - audio (LSB). So a click on a Hell signal put it at 0 Hz audio, outside the 200 .. 3800 Hz passband. Only a click 1500 Hz below the signal worked.
- **Receive: Find signal moved off a locked Hell signal.** The search stops only when the decoder is locked *and* its text looks like text. Hell has no text, so the search kept running and could jump to a stronger signal.
- **Transmit: sideband from another payload's field.** The keyboard TX payload passed `r.variant == 1` as "LSB" to the sideband modulator. The Keyboard payload has no sideband control, and `variant` belongs to other payloads (the RTTY sideband, the PSK variant, the SSTV mode). Whenever that field was 1, Hell went out on the lower sideband, below the dial, instead of 1500 Hz above it.
- **Transmit: keying shape.** The falling edge of each inked run was a 32-sample ramp starting at the beginning of the last pixel. A Feld pixel is 32.65 samples long at 8 kHz, so the envelope fell to 0 and then jumped back to full level for one sample. That gave a key click on every falling edge, which shows as broadband splatter. A single inked pixel had no falling ramp and was cut off hard.
- The on/off modulator itself is symmetric by construction: a real envelope on one tone. The new tests measure this, before and after the sideband modulator.

## Changes

- `core/include/dect2/engine/click_tune.h`: `sidebandClick()`. A click inside the sideband (300 .. 3500 Hz from the dial, on the side of the demodulator in use) keeps the dial and returns the audio frequency. A click outside moves the dial so the signal lands on the audio frequency the decoder uses now.
- `app/hfdig/hfdig_kbd_ui.cpp` and `app/gfx/plots.cpp`: when one of the keyboard-mode entries is shown (Olivia, Contestia, MFSK, THOR, DominoEX or Hell) with USB or LSB, a click on the spectrum or waterfall goes through `sidebandClick()`:
  - Inside the sideband, the click sets the decoder's audio frequency to the unsnapped click. Find signal turns off, because the click chose the signal.
  - Outside, the dial moves so the signal lands on the current audio frequency.
  - Wheel steps and typed frequencies are unchanged. NFM, AM and every other mode are unchanged.
- `app/hfdig/hfdig_kbd_ui.cpp`: dual line draws the same columns twice, at the same time and x position, one copy straight above the other with no gap, as the Hell printers did. Any 14 rows in a row are then a whole column, whatever its vertical phase.
- `core/src/hfdig/hfdig_kbd.cpp`: for Hell, Find signal no longer searches while the decoder is locked.
- `core/src/hfdig/hfdig_kbd_hell.cpp`: each inked run starts and ends with a raised cosine that fits inside its first and last pixel. No spike. A single pixel is ramped on both sides. HellX5 / X9 stay square, and the FSK variants are unchanged.
- `core/src/tx/tx_payload.cpp`: keyboard payloads always go out on the upper sideband.

## Tests (`tests/hfdig/test_hfdig_kbd.cpp`)

- **Audio spectrum, all 7 variants.**
  - Feld and Slow Hell at 1000, 1500 and 2000 Hz; HellX5 / X9 at 2000 Hz. The peak bin must be the requested frequency, and the power below and above must agree within 0.5 dB (1.5 dB for X5 / X9, whose 2.5 / 4.4 kHz width folds at 0 Hz and 4 kHz).
  - FSK Hell 245 and 105, and Hell 80: the energy centroid must lie within half the shift of the centre. FSK is not symmetric by nature, because the inked pixels use the lower tone.
- **Key clicks (Feld Hell).** Less than 1e-3 of the power may lie more than 600 Hz from the tone.
- **IQ through the TX sideband modulator.** Feld Hell, USB and LSB, at 48 kHz:
  - the tone is 1500 Hz above or below the dial
  - the sidebands agree within 0.5 dB
  - the other sideband is more than 40 dB down
- **`sidebandClick()` cases:** USB and LSB, inside and outside the passband, and a click just above the dial.
- **End to end through `HfdigReceiver` with noise (15 .. 25 dB SNR):**
  - Feld Hell on USB and LSB, with the transmitter's dial 0, +337 and -420 Hz off, at audio 1500 and 1700 Hz.
  - One click outside the sideband (+7.5 kHz), which moves the dial.
  - Slow Hell, HellX5 and FSK Hell 245 with offsets.
  - Each case: the audio frequency from the click, then the received picture must match the font above 0.9.
- The existing Hell tests are unchanged and run in the same binary: the picture of every variant, accented Latin, Cyrillic and Greek text, the user-font loader (Inter), and noise alone.

Validation:
- The laptop was on battery, so nothing was compiled or run locally.
- The changed files were syntax-checked with `clang++ -fsyntax-only`.
- The tests run in GitHub CI (ci.yml on this branch).
- Everything is on simulated signals; there is no Hell recording.
- The GUI click and dual-line drawing were not checked on screen.

## Not done / open

- FSK Hell received on LSB is mirrored, so ink and space swap. This was not changed. fldigi has the same convention.
- The TX payload still cannot choose the Hell variant: it is always Feld Hell. The ham keyboard panel has every variant.

## Reply draft for #225 (not posted)

> thanks for testing it! found the problems: clicking the spectrum put the dial on the signal, so with hell (and the other keyboard modes) you had to click 1500 Hz below it. now it works like fldigi, the dial stays and clicking the signal sets the audio frequency, and if you click outside the passband the dial moves so the signal lands on your audio frequency. on tx the keyboard payload could go out on LSB if another mode had LSB/variant 1 picked, thats fixed too, and the keying had a click on every falling edge which is gone now. dual line draws both copies at the same time now, one above the other. will be in 3.8, let me know if it looks right on your side
