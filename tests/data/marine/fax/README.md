# Weather fax layer (`core/src/marine_fax.cpp`)

Text for the fax section of `docs/modes/marine.md`. Nothing here was checked against a real radio signal or a recording.

## Supported
- Input: real USB audio, any rate from 8 kHz (12 kHz recommended); 4 kHz and up work but are not tested below 8 kHz.
- Black 1500 Hz, white 2300 Hz (FM, carrier 1900 Hz, deviation 400 Hz). Mistuning of up to +-50 Hz is measured (start tone, then the
  phasing black level) and removed; `FaxStatus::blackHz` shows it.
- IOC 576 (start tone 300 Hz, width 1809) and IOC 288 (675 Hz, width 904); width = IOC x pi cut to an integer, as fldigi does.
- Line rates 60, 90, 100, 120, 180, 240 per minute, detected from the phasing lines or fixed with `setLpm`.
- Start tone (0.7 s of a steady 300 or 675 Hz tone), stop tone 450 Hz (0.4 s). Tones must be steady within 8 Hz; a sweep or speech does not trigger.
- Phasing: white pulse of 5 % centred on the line boundary (2.5 % at each end of the line), black between. Six equally spaced pulses lock
  the line start; lines are tracked until three in a row are no longer phasing lines; the picture starts at the first of them.
  Joining without the start tone works if the phasing is heard (IOC then defaults to 576, or `setIoc`).
- Slant: the line period is a least-squares fit through the phasing pulses (crossing times interpolated to a fraction of a sample), so a
  clock error of tens of ppm is measured to about 0.1 ppm. `setSlantPpm` adds a manual trim (re-shears the rows already received),
  `setAutoSlant(false)` uses only the manual value.
- The picture: row 0 is the white line the sender puts after the phasing, then the picture rows. Height limit 1500 rows (`setMaxLines`).
  Rows that are really the stop or start tone are removed.
- `FaxAudioSource`: start tone, phasing, one white line, the synthetic chart (grid, coast, isobars, H and L, title block, 8-step grey
  wedge), stop tone, black signal, repeat. `faxTestChart` gives the same chart for comparing.

## Not supported
- Inverted phasing (black pulse on white), colour fax, other line rates, IOC other than 576/288, start tones other than 300/675 Hz.
- A picture with three black rows in a row directly after the phasing and white margins looks like phasing: those rows are taken as
  phasing. Charts normally begin with a white margin.
- No stop tone: the picture stays open until the next start tone (or `reset()`).
- Hard 1500 row limit.

## Sources
- fldigi `src/wefax/wefax.cxx` (ioc_to_width, APT frequencies 300/675/450 Hz, 5 s stop tone, 10 s black, phasing line 2.5 % / 95 % / 2.5 %,
  ENDPHASING white line), which follows HAMFAX, ITU-T T.3 and WMO No. 386. Fetched once; the facts used are quoted in the comments of
  `marine_fax.cpp` and `test_marine_fax_known.cpp`.

## How it was verified (all with the test source and an independent transmitter written in the test; no radio, no recording)
- `marine_fax_known`: width, tone cycle counts (1500 / 3375 / 2250 in 5 s), phasing line shape, an independent transmitter
  (grey levels exact, edges within 0.2 px).
- `marine_fax_tones`: +-50 Hz mistuning, 0 dB audio SNR detection (floor about -3 dB), dropouts, no false alarm in 10 minutes of noise,
  speech-like audio and a sweep.
- `marine_fax_decode`: 7 IOC/line-rate combinations, audio rates 8 / 11.025 / 16 / 22.05 / 44.1 / 48 kHz, chunk sizes 1 to 65536 give the
  same image, overrides, line cap, back-to-back transmissions, joining late.
- `marine_fax_slant`: -50 .. +50 ppm over 800 lines: drift under 0.2 px, estimate within 0.2 ppm; also at 8 kHz, at 15 dB and 8 dB SNR.
- `marine_fax_noise`: SNR sweep, mistuning, DC offset, 8-bit audio, -40 dB level, fading, 20 ms dropouts, `reset()` mid-picture.
- `marine_fax_thread`: reader and setter threads against `push()`; real-time factor.
- ASan/UBSan clean on all of them; ThreadSanitizer clean on the thread test.

## Limits (measured)
- Line lock and slant estimate hold down to 2 dB audio SNR (noise over 0 .. 6 kHz against the 0.5 amplitude carrier); the picture is
  readable (row correlation above 0.7) down to about 10 dB. The decoder's own level SNR estimate is in `FaxStatus::snrDb`.
- About 450 times real time on one core at 12 kHz, 60 times at 48 kHz.
