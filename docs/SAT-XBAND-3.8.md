# X-band and L-band direct broadcast (3.8)

Mode-list entries, families 530 - 549 (poll slots 530 - 549, may move at merge; docs/MODE-NUMBERS.md). One receiver class (`XbandReceiver`, core/src/xband), one profile per entry, a shared CADU layer and instrument decoders. The screen is app/xband/xband_ui.cpp.

Most links need a tracking X-band dish, a down-converter and tens to hundreds of Msps, so the main path is a recording: **Decode recording** plays a .cadu file, WAV (8/16 bit, float), SigMF, or raw .cs8 / .cu8 / .cs16 / .cf32 (with a sample rate) through the receiver at CPU speed, off the analysis thread. Live reception works the same way when the radio keeps up.

| Mode | Family | Link | Instruments decoded | Validated on |
|---|---|---|---|---|
| JPSS HRD (NPP, NOAA-20) | 530 | QPSK 15 Msym/s, K=7, NRZ-M, 1024-byte CADU, 7812 MHz | VIIRS M1-16, I1-5, DNB (Rice, differential channels) | simulated only |
| JPSS HRD (NOAA-21) | 531 | OQPSK 25 Msym/s, K=7, NRZ-M, 1279-byte CADU | VIIRS | simulated only |
| EOS DB (Aqua, Aura) | 532 | OQPSK 7.5 Msym/s, NRZ-M per rail, 8160 MHz | MODIS 36 bands (250 m, 500 m, 1 km, day and night packets) | simulated only |
| EOS DB (Terra) | 533 | BPSK 13.125 Msym/s, K=7, 8212.5 MHz | MODIS | simulated only |
| MetOp X-band dump | 534 | QPSK 35 Msym/s, uncoded, 7800 MHz | AVHRR 1-5, on-board calibration | simulated only |
| MetOp-SG DDB | 535 | QPSK 93.3 Msym/s, K=7, 7825 MHz | METimage 20 channels | simulated only |
| Landsat 8/9 X-band | 536 | OQPSK 220.4 Msym/s, LDPC 7/8 | TIRS from .cadu input only | CADU layer only; **raw IQ not decoded (no LDPC)** |
| Landsat 8/9 S-band | 537 | OQPSK 1.067 Msym/s, K=7 | frames | simulated only |
| Proba S-band dump | 538 | BPSK 2 Msym/s, K=7, 2235 MHz (Proba-1/2 not randomised, Proba-V randomised; tried per frame) | Proba-V VEGETATION S-band lines | simulated only |
| Proba-V X-band dump | 539 | OQPSK 35 Msym/s, K=7, 8300 MHz | frames only (images use CCSDS 122 BPE, not decoded) | simulated only |
| GCOM-W1 | 540 | OQPSK 10 Msym/s, K=7, shortened RS(239), 8245 MHz | AMSR2 20 channels | simulated only |
| GCOM-C1 | 541 | OQPSK 69.4 Msym/s, uncoded | frames only | simulated only |
| STEREO high rate | 542 | BPSK 1.44 Msym/s, CCSDS turbo, 8443.52 MHz | frames from .cadu only | CADU layer only; **raw IQ not decoded (no turbo); ICER images not decoded** |
| Oceansat-2 DB | 543 | QPSK 21.2 Msym/s, differential, 92220-byte frames | OCM 8 channels | simulated only |
| Oceansat-3 L-band | 544 | QPSK 1 Msym/s, K=7, 1703 MHz | frames only | simulated only |
| Saral L-band | 545 | QPSK 400 ksym/s, K=7, 1698.4 MHz | frames only | simulated only |
| Oceansat-3 X-band dump | 546 | QPSK 80 Msym/s, RS(239) x8 | frames only | simulated only |
| Saral X-band dump | 547 | QPSK 16 Msym/s, K=7 | frames only | simulated only |
| Kanopus-V | 548 | QPSK 61.44 Msym/s, differential | PSS swaths, MSS bands | simulated only |
| DMSP RTD | 549 | BPSK 1.024 Msym/s, 2252.5 MHz | OLS visible and infrared | simulated only |

**FY-3 D/E/F** X-band AHRPT is the existing FY-3 AHRPT entry (family 235), not a new one: that receiver already takes the X-band rates and MERSI. tests/xband checks it at 30 and 38.4 Msym/s on the FY-3 test signal (simulated).

No public recordings of these links were found (`/private/tmp/feat38-xband-samples` stayed empty), so **nothing here has seen a real signal**: frame layouts follow SatDump and the receiver decodes its own generator. Details of real signals may differ.

## Products

Colour composites (natural colour, true colour), weather loops (Add to loop / Save loop: animated GIF of the product on screen), and per instrument:

- **MetOp AVHRR**: calibrated from the instrument's on-board targets (PRT, black body and space views, NOAA KLM method, coefficients for MetOp-B and -C): cloud-top brightness temperature, sea surface temperature (McClain 1985 split window, not tuned for MetOp, no land mask, clear sky only), fire (3b hot spots, night passes with channel 3b). The calibration test checks that the black body count returns the black body temperature.
- **VIIRS, MODIS**: airmass (MODIS), dust and fog as band differences of raw counts, marked "uncalibrated": relative contrast only. Their calibration (MODIS tables, VIIRS LUTs) is not implemented, so no cloud-top, sea or fire products from those instruments.

Channels and products are saved as PNG in `<data folder>/xband/<mode>/` and the products go to the gallery.

## Measured (simulated signals only)

K=7 links lock and decode down to 4 dB Es/N0 (Saral L-band, the slowest tested; carrier offset, 200 Hz/s drift, 15 ppm clock, all 8 phase/pairing ambiguities); the uncoded MetOp dump decodes cleanly at 12 dB and loses frames at 9 dB. Pixel-exact instrument tests: VIIRS (including the differential channels), MODIS, AVHRR, AMSR2, TIRS, VEGETATION, METimage, OCM, OLS. The chain (K=7 Viterbi in software) is well below real time at X-band rates; recordings are the practical path.

## Left out

LDPC 7/8 (Landsat X-band) and CCSDS turbo (STEREO) from raw IQ; ICER (STEREO images) and CCSDS 122 BPE (Proba-V X); Proba-1 CHRIS, Proba-2 SWAP and HRC; ATMS, CrIS, AIRS, MHS, IASI; MODIS calibration; geometric projection (bowtie correction, map overlays).

## Credits

Link parameters, frame formats and instrument layouts follow SatDump's pipelines and plugins (https://github.com/SatDump/SatDump, GPL-3.0, Copyright Aang23 and contributors); AVHRR calibration coefficients from its resources/calibration/AVHRR.json; code written for OnAir. Terra/Aqua/Aura spacecraft ids and the Landsat 8.2 GHz default frequency are from memory; check against the spec when a recording exists. THIRD_PARTY.md and in-app credits.
