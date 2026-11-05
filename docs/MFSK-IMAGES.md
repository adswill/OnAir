# MFSK text and pictures (Shortwave Radiogram)

Shortwave Radiogram (Kim Andrew Elliott, WRMI and others, 30 minutes a week) sends text and pictures in MFSK. The programme starts in MFSK32,
switches between MFSK32 and MFSK64 and uses others now and then. OnAir's MFSK decoder (HF digital, mode "MFSK") follows fldigi's `mfsk.cxx`.
The code is `core/src/hfdig/hfdig_kbd_mfsk.cpp`; fldigi (W1HKJ and contributors, GPL-3.0) is credited in THIRD_PARTY.md.

## Modes

| Name | Tones | Tone spacing | Bandwidth | Interleaver | Pictures |
|---|---|---|---|---|---|
| MFSK4, MFSK8 | 32 | 3.9 / 7.8 Hz | | 5 | no |
| MFSK11, MFSK22 | 16 | 10.8 / 21.5 Hz (11.025 kHz clock) | | 10 | no |
| MFSK16 | 16 | 15.6 Hz | 234 Hz | 10 | yes |
| MFSK31 | 8 | 31.25 Hz | 219 Hz | 10 | no |
| MFSK32 | 16 | 31.25 Hz | 469 Hz | 10 | yes |
| MFSK64 | 16 | 62.5 Hz | 938 Hz | 10 | yes |
| MFSK128 | 16 | 125 Hz | 1875 Hz | 20 | yes |
| MFSK64L, MFSK128L | 16 | 62.5 / 125 Hz | | 400 / 800 | no |

MFSK128, MFSK64L and MFSK128L are new (parameters from fldigi). Set the audio centre to the middle of the signal (Radiogram: 1500 Hz) or leave
"Find signal" on; the receiver follows tuning errors of about 90 Hz (MFSK32) by itself and finds the signal elsewhere in the audio in 10 to 30 seconds.
The mode is not detected automatically: Radiogram announces its mode changes in the text ("now changes to MFSK64"), pick the mode in the Mode box.

## What was wrong with the text

The decoder itself is a faithful port: the K = 7 code (0x6d, 0x4f), the diagonal interleaver, the Gray code, the IZ8BLY varicode and the soft decision
Viterbi decoder agree with fldigi's `viterbi.cxx`, `interleave.cxx` and `mfsk.cxx` line by line (checked again in this round), and a loopback decodes down to
-6 dB in 2.5 kHz. What was missing was around it: no MFSK128 / 64L / 128L, no pictures at all (the picture pixels came out as a stream of garbage
characters and the receiver then needed seconds to find the text again), and a fixed receiver centre that is off by more than 90 Hz decoded nothing
unless "Find signal" caught it. See "Not proven" for the real recordings.

## Pictures

fldigi's picture mode, as `mfsk.cxx` and `mfsk-pic.cxx` do it:

* The text says `Pic:<width>x<height>[C][p<N>];` (the transmitter writes "Sending " in front). `C` is colour, `pN` is the number of samples per
  pixel (none: 8, `p4`: 4, `p2`: 2; the buttons X1, X2, X4 of fldigi).
* The transmitter sends the header text, flushes the encoder (a 1 and the preamble length of zero bits), 352 samples of the lowest tone, then the pixels
  and flushes again. Every pixel is `N` samples of one tone whose frequency is `centre + bandwidth * (value - 128) / 256`: black is the lowest tone, white
  the highest. Grey pictures are `W x H` samples, grey = (31 R + 61 G + 8 B) / 100. Colour pictures are three planes per row, red, green, blue.
* The receiver finds the header in the decoded text, waits for the pixels (a fixed delay after the end of the header: 352 samples, plus a symbol
  depending on which half of a tone the last bit came from; MFSK64 and MFSK128 differ because their preamble is longer), reads the mean frequency of each
  pixel's samples (analytic signal of the band-pass filtered audio, phase difference per sample), shows the picture while it grows, and goes back to text
  when the last pixel has arrived.
* The pixel delay was calibrated with the transmitter written from the same fldigi source: the pictures come out pixel exact (mean error 0.6 grey levels at
  40 dB signal to noise). The tuning error found by the text decoder (`offsetHz`) is applied to the pixel frequencies.
* The window shows the picture (scaled, aspect kept) with its size, colour or grey, X1/X2/X4 and the progress; "Save PNG" writes it to the pictures folder
  of the Settings (`<folder>/MFSK/`), "Save every picture" writes each finished picture to `<data folder>/mfsk/`, the thumbnails bring older pictures back
  (eight are kept), "Clear pictures" forgets them. Finished pictures also go to the Images window. The decoded text, including the picture header lines
  and the Radiogram text lines, is in the text pane below, with Copy and Clear.

Limits, as in fldigi: pictures are read at 8 kHz audio (MFSK16, 32, 64, 128 only); an image is up to 4095 x 4095 and 4 million pixels; a sample clock
error of 150 ppm moves the last pixel of a 320 x 240 picture by about 1.5 pixels (the tests accept it); no slant correction.

## Tests

`test_hfdig_mfsk` (ctest `hfdig_mfsk`): every MFSK variant clean; MFSK32 and MFSK64 with tuning error, clock error (+200 / -300 ppm), noise (0, -3 dB),
two fading rates, and "Find signal" with the signal 250 to 300 Hz away or the receiver 300 Hz off; pictures grey and colour at X1, X2, X4 on MFSK16 / 32 /
64 / 128; MFSK32 pictures with tuning error, clock error, noise and "Find signal"; the text goes on after a picture; noise alone gives no picture.
Tool: `DECT2_MFSK_WAV=<file.wav> test_hfdig_mfsk` decodes a recording (variant, centre, start, length, `DECT2_MFSK_PICDIR` for the pictures as PPM).

## Not proven: real recordings

John Rabold KS6M publishes Shortwave Radiogram programmes as 8 kHz WAV files (https://jrabold.net/ks6m/NBEMS/shortwave-radiogram/, programmes 464 to 473 when
this was written; about 28 MB each). Programmes 470, 472 and 473 were run through the decoder with MFSK16, 32, 64 and 128. Result: no readable text and no
picture. The recordings are real, weak and fading over-the-air recordings: the tone grid of MFSK32 is clearly there (tones 31.25 Hz apart from about 1260 Hz,
16 symbols of 256 samples), but the stream is not a code word of the fldigi MFSK code (a search over polarity, tone order, Gray, bit order, interleaver direction
and depth, polynomial order and pair phase found none), and the Viterbi metric is only high for short stretches. A recorded "CR EOT CR" end of transmission was
found, so the chain is right when the signal is good. Whether fldigi itself reads those recordings was not tested (no fldigi on the test Mac). The first
digital minutes of programme 473 start about 107 s into the file after analogue audio, centre near 1495 Hz. Until a clean recording is decoded, treat the
picture timing as proven against the transmitter only.
