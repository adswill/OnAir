# RX888 MkII sample rates (4.1)

Before 4.1 the RX888 MkII driver offered output rates up to 64 Msps. 4.1 raises that to the ADC's limit. This page explains what bandwidth
you actually get, because "128 Msps" does not mean "128 MHz of spectrum".

**None of this has been tested on a real RX888.** The rate plan, clock requests, transfer sizes and conversion were checked against
a fake FX3 in `tests/source/test_rx888.cpp`. Whether a given board, firmware and computer keep up at 130 Msps still needs a real radio.

## The facts

- **ADC:** an LTC2208, 16 bits, rated for 130 Msps. The SDDC firmware sets the clock (Si5351) to whatever STARTADC asks for. ExtIO_sddc
  accepts 50 to 140 MHz (`MIN_ADC_FREQ` / `MAX_ADC_FREQ` in Core/config.cpp) and defaults to 64 MHz. OnAir sets up to 140 MHz. Anything
  above 130 MHz is overclocking: OnAir shows a note and does not block it.
- **Real samples:** the ADC samples the HF input directly and delivers *real* samples. A real signal sampled at fs carries **fs/2** of
  unique spectrum (0 to fs/2). The other half of a complex view is its mirror image.
- **USB 3:** 16-bit samples at fs need 2 x fs bytes/s: 128 MB/s at 64 Msps, 256 MB/s at 128 Msps and 260 MB/s at 130 Msps. That fits a USB 3
  (5 Gbit/s) link, but needs a good port, a good cable and no hub. USB 2 carries about 40 MB/s (about 20 Msps). OnAir now opens the radio on
  USB 2 with a warning instead of refusing it. Most samples will be lost there.
- **VHF:** above half the clock, the R828D tuner puts the channel on a 4.57 MHz IF. Its IF filter passes **about 8 MHz** whatever the
  sample rate. OnAir notes this when a VHF rate above 32 Msps is chosen.

## What each rate gives

The output is complex at clock / D. The conversion filter is flat to about +-35 % of the output rate and is 75 dB down from 65 %. "HF span"
is what you see when you tune to the middle of the band you want. To see the whole of 0 to fs/2, tune to fs/4.

| Rate asked (Auto) | ADC clock | D | Output | Unique HF span | Flat part | VHF (R828D) |
|---|---|---|---|---|---|---|
| up to 32 Msps (default 8) | 64 MHz | >= 2 | 64/D Msps | = output rate | ~70 % of it | <= ~8 MHz |
| 32 to 64 Msps | 128 MHz | >= 2 | 128/D Msps | = output rate | ~70 % of it | ~8 MHz |
| 64 to 65 Msps | 2 x rate (up to 130) | 2 | the rate | 0 to 65 MHz at 65 Msps, centred at 32.5 MHz | ~45 MHz | ~8 MHz |
| 65 to 128 Msps | 128 MHz | 1 | 128 Msps | **0 to 64 MHz** (the other 64 MHz is the mirror) | all (no filter) | ~8 MHz |
| 128 to 130 Msps | the rate | 1 | the rate | 0 to rate/2 (up to 65 MHz) | all | ~8 MHz |
| 130 to 140 Msps | the rate (overclocked) | 1 | the rate | 0 to rate/2 (up to 70 MHz, if the board runs at that clock) | all | ~8 MHz |
| above 140 Msps | 140 MHz | 1 | 140 Msps | 0 to 70 MHz, with a "this radio only offers 140 Msps" note | all | ~8 MHz |

So "120 MHz" on an RX888 MkII really means **a little over 60 MHz of HF spectrum (0 to ~64 MHz) in one view**. The most you can see at once
on HF is half the clock: 64 MHz at the usual 128 MHz clock and 65 MHz at the rated 130. At D = 1 (no decimation) the HF spectrum is not
filtered at all. The board's own anti-alias filter is the only thing limiting what folds in from above fs/2. At D = 2 the conversion filter
also rolls off the outer ~15 % at each edge.

The ADC clock setting (Advanced):
- **64 / 128:** that clock always. Rates above the clock give D = 1 at the clock, not a lower rate than asked.
- **Match the rate:** the clock is the rate times a whole number (up to 140 MHz), so 100 Msps gives a 100 MHz clock with D = 1.

## Trade-offs

- **CPU:** the real-to-complex conversion runs on one thread of its own. At D >= 2 it is a 16 x D tap complex FIR per output sample. At
  D = 1 it is a single complex multiply per sample. `test_rx888` prints what each case costs. On the GitHub runners (one thread, per second of signal) it was 0.19 s at 64 MHz with D = 32, 0.63 s at 128 MHz with D = 2, 0.64 s at 130 MHz with D = 2, and 0.33 s at 128 or 130 MHz with D = 1. Older or slower machines may not keep up at D = 2 and the full clock.
  The decoders and the spectrum behind it have to keep up with 128 Msps complex too, which most modes do not need.
- **USB pipeline:** 128 KiB bulk transfers (as SDDC). The number in flight now follows the clock, so about 32 ms is always queued: 32 at
  64 MHz (as before), 63 at 128 MHz and 64 at 130 MHz. The queue to the conversion thread holds about 250 ms: 256 blocks at 64 MHz (as
  before), 489 at 128 MHz and 496 at 130 MHz.
- **Dropped samples:** when the conversion falls behind, the oldest blocks are dropped. The samples are counted and logged every 5 s, and
  the status line shows them ("RX888: N samples dropped"). The SDDC stream has no sample counter, so samples lost inside USB itself cannot be
  counted.

## Defaults

The defaults are unchanged: with no rate given, the radio runs at 8 Msps from a 64 MHz clock (D = 8). Every rate up to 64 Msps gets the same
clock and decimation as in 4.0. Only rates that were capped before (above 64 Msps) behave differently.
