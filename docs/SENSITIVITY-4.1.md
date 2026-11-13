# Sensitivity 4.1 (feat41-sensData): the gaps 4.0 left open

All numbers are from `sens41` (tests/sens/test_sens41.cpp) on the GitHub Linux runner (quick.yml). Every row runs the 4.0 path and the 4.1 path
on the **same signal and the same seeds**: each change has its own switch, so the 4.0 behaviour is measured in the same binary. Figures are the
share of the 4.0 path's clean-run units decoded (4 trials a point), SNR as in the sens harness (docs/SENSITIVITY.md). Simulated signals only:
no recording of any of these was used. Noise-only runs are next to every gain. `ONAIR_SENS41=<sections>` picks sections
(sonde, iridium, zigbee, aero, globalstar, fm), `ONAIR_SENS41_TRIALS` sets the trials.

| item | change | before -> after | false decodes | default |
|---|---|---|---|---|
| iMet-54 | soft symbols, soft Hamming(8,4) with CRC-checked second choices, bytes realigned on their UART framing bits | no change (11 dB 77 -> 77 %, 9 dB 11 -> 14 %) | 0 in noise | on (neutral) |
| MRZ | both halves of every Manchester bit (4.0 used the first half only) | 8 dB 77 -> 92 %, 6 dB 2 -> 15 %; +8 kHz: 8 dB 85 -> 98 %; about 1 to 1.5 dB | 0 in noise | on |
| Iridium | bit reliabilities from the demodulator, Chase repair of one failing BCH block | frame level (sigma 0.5): MSG 160 -> 197, IBC 248 -> 278, IRA 265 -> 281 of 300; receiver curve: no change (10 dB 26 -> 27 %) | random bursts: 6 -> 8 frames of 20000; 0 in noise | on |
| Zigbee | second choice of the 4 least sure symbols when the FCS fails | 1.5 dB 89 -> 95 %, 0 dB 33 -> 47 %; 50 ppm: 0 dB 18 -> 29 %; about 0.5 dB | 0 in noise | on |
| FT8 / FT4 | the a-priori CQ stage keeps only words shaped like a real CQ | wrong texts 1 -> 0 in 30 slots a point (white noise and dial +40 Hz / +100 ppm); decode shares identical at every SNR (-20 dB 97 %, -21 dB 69 / 66 %, -22 dB 7 / 14 %) | 0 in noise slots | on |
| Aero | band search every 0.25 s for the first 4 searches, carrier search after every averaged spectrum | 3 s trials: 0 -> 100 % from 3 dB up, 0 dB 0 -> 75 %; 6 s: 0 dB 68 -> 100 %, -2 dB 7 -> 16 % (50 ppm: 0 dB 48 -> 99 %) | 0 in noise | on |
| Globalstar | first floor capped over the neighbours, not the quietest channel | 0 events at odd rates **not reproduced** (8, 10, 12, 13.7, 20 Msps, 0 / 5 / 50 ppm Lagrange: same events both ways) | 0 in noise (10, 13.7 Msps with the resampler) | on (neutral) |
| NFM PLL | 2nd-order PLL demodulator, 4 kHz natural frequency; auto = PLL under 8 dB C/N | SINAD worse at every SNR: 16 dB 23.1 -> 21.9, 8 dB 14.2 -> 13.2, 4 dB 5.2 -> 4.7 dB | - | off (`setFmPll`) |
| APT FMFB | FM feedback demodulator, gain 2, two poles at 14 kHz | PSNR 15 dB 32.8 -> 16.5 (distortion), 7 dB 16.1 -> 16.8, 2 dB 12.8 -> 13.1, 0 dB 11.0 -> 10.4 | - | off (`setAptFmFeedback`) |

## 1. iMet-54

4.0 left it about 7 dB behind RS41 with the hard SECDED decoder (any word with two wrong bits dropped the whole frame of 216 words) as the main
suspect. In this harness RS41 is at 7 dB (98 %, 0 % at 5 dB) and iMet-54 at 13 dB (98 %), 11 dB 77 %, 9 dB 11 %: about 4 to 5 dB apart at the
90 % point. Three changes were measured one after the other, with the same seeds:

1. soft symbols from the symbol chain (`SymbolChain::step` with a soft output: the slicer value over half the smoothed extremes' span) and
   maximum-likelihood decoding of each Hamming(8,4) word by correlation with the 16 codewords, then a Chase search over the 6 least sure words
   checked by the 20 check bits of the frame CRC: 11 dB 77 -> 75 %, 9 dB 11 -> 11 %.
2. each UART byte read where its start and stop bits fit (a symbol the clock gains or loses in a click shifts every later byte): 11 dB 77 -> 77 %,
   9 dB 11 -> 14 %.
3. a soft header search (up to 7 wrong header symbols when the soft correlation is 60 % of a clean one): it locks before the true header
   (clean frames 11 -> 1), removed.

So the frames are not lost to scattered bit errors (soft FEC would have recovered them) nor to slips. What is left is ahead of the frame layer:
the cliff (98 % to 11 % in 4 dB) has the shape of the FM threshold of the discriminator in the narrow channel, which RS41 avoids because its
symbols come from tone energies (part 4.0 B). A4 measured tone energies on the iMet-54 midpoint chain as worse (its UART line code is
unbalanced, the tone centre comes from the drifting extremes). **The gap is explained as the discriminator's FM threshold, not the code;
not closed.** Next step: tone energies with a centre from the preamble (00 AA bytes, balanced) held through the frame. Changes 1 and 2 are kept
(no loss, 0 noise-only units); `SondeReceiver::setSoftDecisions(false)` gives the 4.0 path.

## 2. Soft decisions

* **MRZ** (2400 symbols/s Manchester): 4.0 decided each bit from the first symbol of its pair. With soft symbols the bit is
  soft(first) - soft(second): about 1 to 1.5 dB (table). Guard `sens41`: 8 dB 90 % (4.0: 77 %).
* **Iridium**: `DemodResult::rel` / `IridiumBurstBits::rel`, from the phase step y[k] y*[k-1] turned by -45 degrees (0 where it disagrees with
  the coherent decision). `decodeIridiumBurst`: when a ring alert, broadcast or pager frame has exactly one 32-bit BCH block that does not
  decode and at least 3 that do, a Chase search tries every combination of the block's 5 least reliable bits through the algebraic decoder and
  keeps the candidate with the smallest reliability sum, only if all the bits it changes are among the 7 least reliable; the repaired frame
  must pass the same plausibility checks as a "guessed" frame. Frame level (bits +-1 plus Gaussian noise): a large gain (table). Receiver
  level: none, so the harness bursts are lost before the BCH (burst detection, unique word, or several blocks at once).
  False frames from 20000 random bursts (half with a pager header): 6 hard, 8 with the repair. A first version that repaired two blocks and
  allowed changes among the 10 least reliable bits gained more (MSG 160 -> 227) but doubled the false frames (6 -> 12): not kept. `IridiumReceiver::setSoftDecisions(false)`.
* **Zigbee**: `symbol()` also returns the second best of the 16 sequences; when the FCS fails, the 4 PSDU symbols with the smallest lead
  (under 30 % of the score) are tried with their second choice (15 extra checks of a 16-bit CRC: about one wrong frame in 4400 failed frames).
  The sub-GHz BPSK path is unchanged. `ZigbeeReceiver::setSoftDecisions(false)`. A coherent O-QPSK correlator (about 5 dB, A3) is still the
  larger step.

## 3. FT8: wrong texts at the CQ stage

The a-priori CQ stage clamps the 29 CQ bits and the 3 type bits; the 45 bits it fills in are random for a noise candidate. A real CQ never
has: the R of a reply before the grid, the /R of the second call, a report / RRR / RR73 / 73 where the grid goes (g15 >= 32400), or a call
starting with 0, 1 or Q (no ITU prefix). The 4.0 wrong text was `CQ 1Y3GWX/R R DI01` (three of the four). A noise word passes about one
time in five; a real CQ always. `HFDIG_FTX_APCQSHAPE=0` gives the 4.0 stage. Rovers (/R) calling CQ below the plain decoder's threshold are
lost to the a-priori stage (they still decode at the plain stage).

Measured with `hfdig_sens40b` (`ONAIR_SENS40B_MODES=FT8 ONAIR_SENS40B_SLOTS=30`, 3 stations a slot) on the macOS runner, this branch and
integ-41 built and run on the same runner (quick.yml `compare`, run 43373132809): integ-41 printed `CQ 1Y3GWX/R R DI01` at -21 dB in both
the plain and the dial/clock-error sweep; this branch none, with the same share of decodes at every SNR from -12 to -24 dB. FT4 / FT2 use
the same stage and were not swept.

## 4. Aero acquisition on short trials

A new carrier had to show in two band searches one second apart (2 s), then the channel's squaring carrier search looked after 4, 8, 16 and 32
averaged spectra of 0.4 s. Now the first 4 band searches run every 0.25 s (same 16 spectra each), and the carrier search looks after every
spectrum from the 4th on. With 3 s trials the 4.0 receiver decoded nothing at any SNR (clean run 1 unit against 78). `AeroReceiver::setFastAcquisition(false)`.

## 5. Globalstar clock error at odd rates

QA-4.0 saw 0 events at 13.7 Msps with any clock error through its 4-point Lagrange resampler. sens41 runs the generator at 8, 10, 12, 13.7 and
20 Msps through the same resampler (0, 5, 50 ppm) and 6 dB noise: events and channel floors are the same with the 4.0 and 4.1 floor
(13.7 Msps: 3 events both ways, floors within 0.1 dB of the noise). **Not reproduced** here; the QA path also writes 16-bit files (gain,
clipping) and uses the generator's own noise, which this test does not. The neighbour floor cap is kept (a cap from the quietest channel of
the band is wrong whenever the radio's filter rolls the noise off at the edges); `GlobalstarReceiver::setLocalFloorCap(false)` gives 4.0.

## 6. FM threshold extension

* NFM (general receiver, 1 kHz tone at 2.5 kHz deviation, 240 kS/s, SINAD of 2 s): the PLL is about 1 dB worse at every SNR. A loop that tracks
  2.5 kHz deviation at audio rates needs a natural frequency of about 4 kHz, so its noise bandwidth (about 13 kHz) is no narrower than the
  12.5 kHz channel: no threshold gain is possible. `setFmPll(0/1/2)`, default 0.
* NOAA APT (wxsat_apt40 signal, 960 kS/s, PSNR): FM feedback gives +0.4 to +0.8 dB at 2 to 7 dB and loses 0.6 dB at 0 dB and 16 dB at 15 dB
  (the 14 kHz poles distort the 17 kHz deviation when the loop is not compressing enough). Not clearly better: off (`setAptFmFeedback`).

## CI runs

quick.yml (Linux): 43363995944 (sonde only), 43369540756 (all sections; sens40a3, sens40a4, sonde, iridium, zigbee, aero, globalstar,
general_rx, wxsat_apt40, hfdig_ftxsmart, hfdig_ftxsens pass; sens41 failed only its own Iridium false-frame check and a wrong iMet-54
guard, both changed), 43373243946 (soft header: broke iMet-54, removed), 43376898910 (sonde and iridium sections, sonde_extra, iridium_*: pass); macOS 43373132809 (FT8 against integ-41).
