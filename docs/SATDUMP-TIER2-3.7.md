# SatDump parity, tier 2 (ships in 3.8)

The file broadcasts on the DVB-S2 receiver, and Meteosat raw data. All links run in the wxsat receiver class (profile per mode-list entry), so each has its own mode-list row, numbers (docs/MODE-NUMBERS.md, families 269 - 271, poll slots 229 - 231; may need to move at merge) and screen. How batch 1 was done: docs/SATDUMP-TIER1-3.7.md.

## Modes

| Mode | Family | Link | Status | Validated on |
|---|---|---|---|---|
| MSG Raw Data | 269 | 1686.83 MHz, QPSK 3.75 Msym/s, K=7 r=1/2, ASM, RS(255,223) x5, same path as AWS; SEVIRI on APID 2046 | new | synthetic only, down to 6 dB Es/N0 with 40 kHz offset and 20 ppm: every compared pixel right. No public recording found. Clock constants and focal plane swap come from SatDump and are not checked against a real signal |
| HimawariCast | 270 | C-band 4148 MHz (1002 MHz after a 5150 MHz LNB), DVB-S2 QPSK 2/3, 2.586 Msym/s, MPE PID 1001, Fazzt payload 1411 | new | real files for the upper layers; synthetic for the radio path (below) |
| GeoNetCast | 271 | C-band 4080 MHz (1070 MHz after the LNB), DVB-S2 8PSK 5/6, 30 Msym/s, MPE PID 4201, Fazzt payload 1408 | new, files only | synthetic only, and through RF only at 3 Msym/s (the same chain at a rate a test can afford). Never run at 30 Msym/s. Needs 52 Msps, so for most radios recordings only |

### What was validated on real data

- bzip2 and HRIT, on 20 real segments from JMA's public samples (Himawari-9, IR1 and IR3, 2017-03-08 02:30): 20 of 20 decompressed with the CRCs right, two full discs assembled, about 20 % of the pixels space, as a full disc should be.
- Fazzt headers, on 176 real head packets from the himawari-rx repository: names, part counts and sizes parse, none rejected.
- Fazzt parts: the 474 real content packets there are the middle of a file (parts 344 - 817) with no head in the capture. Their layout was checked by hand; they were not reassembled into a file.
- NOT validated on real signals: MPE and IP around Fazzt on the air, the HimawariCast carrier itself, any GeoNetCast data, any MSG data. No public IQ or transport stream recording was found for any of them.

Synthetic thresholds seen: HimawariCast decodes at 10 dB with 60 kHz offset and 15 ppm; GeoNetCast at 16 dB at the reduced rate.

The app was checked with an isolated renamed copy (hidden, muted, test signal, own data folder): HimawariCast 463 files and 42 pictures, written to disk as HRIT, PNG and other files; MSG 6389 frames with none failed. Only the Overview tab was captured: the Picture tab that shows the discs and the file list has been compiled and exercised for its save path, but not looked at. As in batch 1, HOME did not move the macOS settings (they went to the renamed copy's own domain, deleted afterwards; dect2.plist unchanged).

## How it works

DVB-S/S2 receiver, transport stream, packets of the link's PID, the existing MPE extractor, IPv4 and UDP removed, Fazzt packets, whole files on a worker thread. HimawariCast files are bzip2 streams (decoder written for OnAir, no new dependency) holding HRIT segments: ten per channel make one full disc, saved as PNG. Everything else is saved as it comes. Files go under `<data folder>/wxsat/<mode>/` in COMPLETE, INCOMPLETE, HRIT, ADD, BADBZ2 and IMAGES.

One click: the symbol rate, standard and PID are set by the mode, and the carrier is found by itself. Advanced (Receiver panel): symbol rate and PID. MSG discs save themselves every 15 minutes as they end, one PNG per channel and a colour composite.

What comes off the air is untrusted: every length is checked, memory is bounded per file and in total, and file names are cleaned before they reach the disk. A file received over a recording blocks instead of losing frames.

MSG pictures are reduced to fit memory: the eleven standard channels 2:1, HRV 3:1.

## Left out / next

- EUMETCast: not added. SatDump has only demodulator settings for it (a work-in-progress pipeline), and no file layer. Its transport is not public and most of the content needs EUMETSAT's key unit. No decryption was written; the existing DVB-S2 mode shows the carrier and its IP flows.
- MTG: not added. The Ka-band data link is 188 Msym/s, beyond any radio the app supports. The S-band link is telemetry frames only, needs a new PM subcarrier demodulator, and has no public recording; left for the owner to decide.
- HRIT from MSG via EUMETCast (DecompWT wavelet) and MTG FCI (fcidecomp): not used, separate licences, no file layer reaches them.
- Satellite-specific calibration and projection of any of these; SEVIRI at full resolution; MSG-1 (no clock known, retired from this service); Rapid Scan mode (the 5-minute discs).
- The Symbols panel stays empty on the two DVB-S2 modes: the DVB receiver's telemetry has no constellation.
- Real recordings of all three links.

Sources: formats and parameters from SatDump (GPL-3.0, Copyright Aang23 and contributors): plugins himawari_support, geonetcast_support, meteosat_support, src-core/common/mpeg_ts/fazzt_processor and resources/pipelines/{Himawari,GeoNetCast,MSG}.json. Code written for OnAir; notices in each file, THIRD_PARTY.md and in-app credits. DecompWT, fcidecomp and libtomcrypt were not used. Sample files from JMA and from sam210723/himawari-rx (MIT) were used for testing only and are not in the repository.
