# FY-3 AHRPT and FY-3 MPT (Fengyun-3 polar weather satellites)

Two modes, each with its own row in the mode list (families 235 and 236, group "Weather and science"):

| mode | downlink | where | symbol rate | what comes out |
|---|---|---|---|---|
| **FY-3 AHRPT** | Advanced High Resolution Picture Transmission | FY-3C **1701.4 MHz**, FY-3A / FY-3B **1704.5 MHz** (L-band); FY-3D 7820, FY-3F 7790, FY-3E and FY-3H 7860, FY-3G 7790 MHz (X-band) | 2.6, 2.8 Msym/s (L-band); 6 (FY-3G), 30 (FY-3D), 38.4 (FY-3E / F / H) Msym/s in X-band | VIRR pictures (FY-3A / B / C), MERSI pictures (FY-3D onwards), the other instruments named and counted |
| **FY-3 MPT** | Medium rate Picture Transmission of FY-3A, FY-3B, FY-3C | **7750** MHz (FY-3A / B), **7780** MHz (FY-3C), X-band | 18.7 Msym/s | MERSI pictures (5 channels of 250 m, 15 of 1 km) |

Only the L-band AHRPT is within reach of a home station with a dish and an LNA. The X-band downlinks (all of MPT, and AHRPT of FY-3D and later) need an
X-band feed, a low-noise down-converter to L-band and a radio that samples at least as fast as the signal is wide (1.06 x the symbol rate at the very least,
1.5 x for the whole signal): 28 Msps for MPT, 40 Msps and up for FY-3D. The decoder does not care which frequency it is: tune the down-converter's output.
The MetOp AHRPT stays in the HRPT / AHRPT mode (its FY-3 entry there only counts frames, with the old single-code chain).

Frequencies in the first version of this request ("MPT near 1704.5 MHz") were mixed up: **1704.5 MHz is the AHRPT of FY-3A and FY-3B**. MPT is X-band.

## What the receiver does

1. DC spike taken off, blind IQ-imbalance correction, interpolation to twice the rate when the capture holds fewer than 2.8 samples a symbol (a 4 Msps capture
   of a 2.6 Msym/s signal), mixed to zero.
2. **The symbol rate is measured**: the spectrum of the signal's power has a line at the symbol rate; the rates that fit the sample rate are tried and the
   one with the clearest line wins (2.6 against 2.8 Msym/s is a 7% difference, nothing to confuse). Choose a satellite or type a rate (no limits) to skip it.
3. The carrier from the spectrum (a raised-cosine fitted to the averaged power spectrum, +-75 kHz at L-band, wider in X-band), root raised cosine matched filter,
   Gardner timing with cubic interpolation, a Costas loop with a **soft-decision** detector (tanh at the confidence the measured Es/N0 allows) whose bandwidth
   narrows as it settles (0.02, 0.004, 0.0012 of the symbol rate) and opens again when the lock is lost.
4. Soft symbols scaled by a **slow level estimate**: a fade lowers the confidence of the symbols instead of being normalised away.
5. **Two independent K = 7 Viterbi decoders**, one for each QPSK rail (the FY-3 coding). The pairing of the code symbols and their polarity are found from the
   path metrics (the right pairing scores far above the others), then the differential decoder, then the sync word under the eight ways the two rails can be
   arranged. All eight arrangements keep running, so a slip of the carrier loop by a quarter turn costs the frames of a few milliseconds, not a new search.
6. CADU sync word 0x1ACFFC1D, CCSDS randomiser, Reed-Solomon (255,223) interleave 4, virtual channels, the satellite from the spacecraft id (FY-3A 49 ... FY-3H 68).
7. Instruments: **VIRR** (FY-3A / B / C, virtual channel 5): frames of 26050 bytes on a 60-bit sync word, 10 channels of 2048 pixels of 10 bits, a time stamp, placed
   in the picture by that time so lost frames leave gaps. **MERSI** (virtual channel 3; MERSI-II on FY-3D, MERSI-LL on FY-3E, MERSI-3 on FY-3F / FY-3H, MERSI-RM on FY-3G): a
   bit stream of head frames and scan lines, 12 bits a pixel; the 250 m channels are averaged to the 1 km grid of the picture. MWRI, MWHS, MWTS, ERM, GAS, WAAI, X-EUVI,
   WindRad and PMR are named and counted.

## Using it

* Pick the satellite (it sets the frequency, the symbol rate and the roll-off) or leave **Automatic** and tune 1701.4 or 1704.5 MHz.
* The Picture tab shows the pass growing row by row; the instrument and the view (colour composite or one channel) are two combo boxes; Save PNG writes the picture
  as shown to `<data folder>/fy3/`.
* **Why it does not decode yet** is a sentence on the Picture tab and the Receiver tab (no carrier / code not found / no frame sync / Reed-Solomon failing / sample rate too low).
* **Margin**: the Receiver tab shows the measured Es/N0 against the 2.2 dB the decoder needs.
* **Next pass**: with the weather satellites' elements downloaded in the Satellites tab (and the position set) the tuner shows the next pass of the selected satellite.
* The decoder takes Doppler of +-40 kHz and a rate of several hundred Hz/s, a sample-clock error of 300 ppm, an inverted spectrum or exchanged I and Q.

## Sensitivity

Measured with `tools/fy3tool` (Es/N0 = energy of a QPSK symbol over the noise density, noise bandwidth = symbol rate; share of frames that pass Reed-Solomon):

| Case | Needed Es/N0 (50% of frames) | Theory |
|---|---|---|
| Ideal chain (perfect carrier, timing, sync; soft Viterbi per rail, differential decoder, RS) | 1.78 dB | - |
| FY-3 AHRPT, 2.6 Msym/s, 4-20 Msps | 1.95 dB | gap 0.17-0.21 dB |
| FY-3 MPT, 18.7 Msym/s, 28 Msps | 1.72 dB (90%: 1.96, 99%: 2.37) | at theory (1141 of 1141 frames at 2.5 dB and up) |
| FY-3 MPT, 18.7 Msym/s, 20 Msps | about 3 dB, erratic (65% at 3.5 dB, 17% at 4 dB), clean from 8 dB | 1.4 samples a symbol: too few |

AHRPT also decodes through a Doppler ramp, clock error, off-centre carriers and a rate sweep (impairment table: `fy3tool imp`). MPT at 28 Msps decodes 100% at 5 dB
through offsets of +-60 kHz, ramps of 500 Hz/s, +-300 ppm, IQ imbalance of 2 dB / 10 deg, a DC spike of 50%, phase noise of 30 Hz, fades, an inverted spectrum and exchanged
I and Q. MPT needs at least 20 Msps (4 to 16 Msps cannot hold an 18.7 Msym/s signal); 28 Msps or more is advised.
Reed-Solomon erasure decoding is not used (the sync loss costs frames instead).

## Not verified

The HRPT / AHRPT mode (family 64) counts FY-3 frames only; the pictures are in these two modes. No off-air recording of an FY-3 downlink was available to this build. The coding and the instrument layouts follow the open decoders; the receiver was checked against its
own test signal built with the same layers. Wrong guesses about a detail of the real signal (the exact bit order after the differential decoder, a channel order in MERSI) would show as
frames that do not sync or pictures with the wrong channels; the receiver tries every arrangement of the rails and both polarities of the code on its own, so most such mistakes are absorbed.

## Credits

The channel coding, the virtual channel numbers and the VIRR / MERSI frame layouts follow the FengYun-3 plug-in of **SatDump** (Aang23 and contributors, GPL-3.0,
https://github.com/SatDump/SatDump, plugins/fengyun3_support). The code is written for OnAir; THIRD_PARTY.md and the Credits window name it.
