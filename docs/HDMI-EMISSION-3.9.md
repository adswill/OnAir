# Local display / HDMI emission (3.9, issue #475)

A monitor, a TV or its HDMI / DVI cable leaks the TMDS pixel clock and the picture data it carries. The radio then sees:

- a clean line at the pixel clock (25.175, 74.25, 148.5, 297, 594 MHz ...) and at its harmonics;
- around those lines, a comb of lines spaced by the line rate, the "parallel lines" on a waterfall. The line rate is the pixel clock divided by the total pixels per line, 27 - 135 kHz.

OnAir now recognises this and labels it **"Local display / HDMI emission, not your SDR"**, so it is not mistaken for the radio's LO or clock leak.

## Where it shows

- **Spectrum explorer.**
  - The live spectrum is checked once a second, and each finished sweep checks its panorama.
  - The lines that belong to the emission get magenta ticks at the top of the spectrum, labelled with the likely timing.
  - A *Display emission* tab shows the summary, the evidence, the alternatives with their confidence and a *Copy* button.
  - The check is skipped through an inverting converter.
- **Signal identifier.** When the selected signal is the clock line or one of its comb lines, the candidate list gets an "Interference" entry with this label. It carries the timing as "why", the evidence and up to three alternatives. When it ranks first above 60 %, the verdict is "likely".

## How the timing is estimated (core/src/rfi/display_emission.cpp)

1. **Narrow lines.** Local peaks at least 6 dB over the local median, falling by 3 dB within 6 bins on both sides. Fewer averaged spectra need a higher threshold, up to 12 dB.
2. **Harmonics.** For each table clock, every line seeds a clock estimate (line / nearest harmonic, within 400 ppm). The seed that explains the most harmonics inside the range wins, and the clock is refitted by least squares over all of them.
3. **Line-rate comb.** When the resolution allows (bin at most a quarter of the line rate), lines are counted at the timing's own line rate on both sides of each harmonic. Positions on the grid with no line count against it, and so do lines inside its reach that are off the grid. A comb at twice the true spacing therefore loses.
4. **Score.** The score is built from: the harmonics found, a penalty for missing ones, the comb, a penalty for the clock offset and a small preference for the lower harmonic. The display confidence is 1 - exp(-score / 30), and "found" needs 0.45. A lone line at 148.5 MHz gives about 0.33, so it is not called a display. Each timing's share comes from exp(score / 8) over all the matches.
5. **Unknown clock.** Four or more harmonics of a clock between 20 and 700 MHz that is not in the table are reported as "display-like clock, not a standard timing", with about N pixels per line from the comb. The confidence is capped at 0.6.

**Table.** CEA-861 TV formats (VIC 1, 2, 4, 5, 16, 17, 19, 20, 31 - 34, 63, 64, 93 - 97, and the 1/1.001 rates) and VESA DMT / CVT / CVT-RB monitor timings, 49 rows. The values are from memory of the standards. The test checks every row's refresh rate against clock / (htotal x vtotal) to within 0.3 %.

**What cannot be told apart.**
- Timings with identical totals and clock, such as 1280x960 60 Hz and 1600x900 60 Hz RB (both 1800 x 1000 at 108 MHz). Both are listed.
- At one harmonic in a narrow capture, 3840x2160 30 Hz and 1920x1080 60 Hz (297 MHz with a 67.5 kHz comb). The lower harmonic is preferred, and the other is listed as the alternative.
- In a wide sweep with 100 kHz bins the comb cannot be resolved, so all timings sharing the clock are listed. Zooming in on one harmonic separates them.
- Displays with spread-spectrum clocking (DisplayPort, some laptops) smear the lines. They are not modelled.

## Generator and tests (simulated only)

- `dispem::generateIq()` builds the IQ samples. It places the clock harmonics inside the band, odd ones stronger, and around each one the comb of a static picture repeating every line (random phases and levels falling away from the clock). It adds weak frame-rate lines, a clock offset and noise.
- `dispem::simulateSpectrum()` makes a wide spectrum.

Test `display_emission` (tests/rfi/test_display_emission.cpp):
- **Table consistency.**
- **Wide spectra.** 10 timings x clock offsets of -180 / 0 / +250 ppm, 20 MHz - 2 GHz, 100 kHz bins. Clock within 30 ppm; the timing is among the matches; 59.94 Hz is told from 60 Hz.
- **Narrow 2 Msps captures at one harmonic.** 8 timings, h1 - h3, tuned 230 kHz off the line, -120 / 0 / +90 ppm, at 45 and 36 dB. The exact timing comes out best, with at least 6 comb lines. `ONAIR_TEST_LONG=1` runs every offset at the weaker level.
- **Negatives.** A lone 148.5 MHz carrier, a 150 MHz line with a 25 kHz comb, a 100 kHz switching-supply comb from 1 to 30 MHz, and noise.
- **Identifier.** The candidate ranks first for a 1080p60 capture.

No real recording of a display's emission was available, so all of this has been validated on simulated signals only.
