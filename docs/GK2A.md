# GK-2A (GEO-KOMPSAT-2A) LRIT and HRIT

GK-2A is the Korean geostationary weather satellite at 128.2 E (launched 2018, operated by KMA, the Korea Meteorological Administration). It
sends its pictures to ground stations in two L-band broadcasts. OnAir lists them as **two modes**, because they are different signals:

| Mode | Frequency | Modulation | Symbol rate | Bandwidth (KMA) | Frames (before the convolutional code) |
|---|---|---|---|---|---|
| **GK-2A LRIT** | 1692.14 MHz | BPSK (NRZ-L) | 128 ksym/s | 1 MHz channel | 64 kbit/s |
| **GK-2A HRIT** | 1695.4 MHz | QPSK (NRZ-L) | 3 Msym/s | 5.2 MHz channel | 3 Mbit/s |

Both: root raised cosine, roll-off 0.5, linear polarisation (east-west), rate 1/2 K = 7 convolutional code (CCSDS, second output inverted),
Reed-Solomon (255,223) interleaved 4 deep, randomiser, sync word 0x1ACFFC1D, 1024-byte CADUs (KMA GK2A LRIT / HRIT Mission Specification
Documents, issue 1.0, tables 8.5 and 9.1). The receiver is **receive only**.

## What you need

* **GK-2A above your horizon.** At 128.2 E it is visible from East and South-East Asia, Australia, New Zealand and the western Pacific. It is
  below the horizon in Europe, Africa and the Americas (from Lithuania it is 104 degrees of longitude away: no).
* **A dish with an LNA and a filter at the feed.** The carrier is far below the noise of a plain antenna. Roughly: LRIT 0.9 m or more,
  HRIT 1.2 m or more (a 1.7 GHz grid antenna works for LRIT in good conditions). Aim at the satellite's elevation and azimuth.
* **A radio:** LRIT works from 0.3 Msps up to 20 Msps (RTL-SDR, Airspy, SDRplay, HackRF). HRIT needs 6.4 Msps or more (2.1 samples a symbol): HackRF
  (10 Msps is the default), Airspy R2, SDRplay RSPdx, Pluto, USRP. Any rate between is fine; the receiver adapts (no clamps).
* **KMA decryption keys for the pictures.** KMA encrypts the pictures (DES, one 64 bit key per key number; header record 7 says which). Keys come
  from KMA on application. OnAir **ships no keys**. Choose your key file with **Import keys** in the Products tab: the plain
  `EncryptionKeyMessage.bin` (a 2 byte count, then a 2 byte key number and 8 key bytes for each key; it is the file KMA issues for your station
  after the station-specific layer is removed) or a text file with lines `number:hexkey`. The keys are copied to `<data folder>/gk2a/keys.bin`
  and read at the next start. Without a key everything else still works: the carrier, the frames, the files, the text messages; the pictures show
  "encrypted (KMA key N), key not loaded" and are decoded the moment the key is imported (no need to receive them again while they are in the list).

## What it decodes

The whole chain, down to the file:

1. **Carrier and symbols.** Coarse carrier search from the squared (BPSK) or fourth power (QPSK) spectrum, a decision-directed Costas loop, a
   Gardner timing loop with a cubic interpolator, root raised cosine matched filter. Frequency offset, symbol clock, polarity and (QPSK) the quarter-turn
   ambiguity are found by the receiver.
2. **Frames.** The sync word is found in the soft symbols (52 code bits that do not depend on the encoder state, over three frames), which also gives
   the I/Q order, the polarity and the G2 inversion. Viterbi (K = 7, 8-bit soft decisions), sync word, randomiser, Reed-Solomon (255,223) x 4.
3. **VCDUs** (6 byte header, 886 byte M_PDU) and the CCSDS space packets in them, their CRC-16, the file counter.
4. **xRIT files:** primary header, image structure, navigation, annotation (the file name), time stamp, key header (7), the KMA image segment header (128:
   sequence number, total, first line); text and additional-data files.
5. **Pictures.** Each image file is one segment. Its data field is (DES-decrypted, then) a lossless **JPEG 2000** codestream, decoded by OnAir's own
   decoder (reversible 5/3, any precision up to 16 bit, tiles, layers, all progression orders; also 9/7), on a worker thread so the receiver never
   waits. The segments of one picture (same name, segment `_NN`) are put together into the full picture of the channel. The 16 channels are named as in the KMA
   specification: VI004 VI005 VI006 VI008 NR013 NR016 SW038 WV063 WV069 WV073 IR087 IR096 IR105 IR112 IR123 IR133 (APID 0 to 15 on virtual channel 0).
   The picture is shown with an optional percentile stretch (the channel counts use only part of the range) and saved as a PNG; the full-range data
   stay in the `.lrit` files.
6. **Raw files.** Every whole file is saved as it was sent, as `<name>.lrit`, into `<data folder>/gk2a/lrit/` (switch: Save .lrit files), so other xRIT tools
   (xrit-rx, SatDump's tools, KMA's own) can use them too.
7. **Text.** Observation schedules and additional data (virtual channels 4 and 5) are shown as text and saved.

The CADU frames can also be recorded and decoded from a `.cadu` file (the same as GOES HRIT).

## Why nothing decodes: the hint line

The Products tab and the Receiver tab show one line saying what to do next: sample rate too low; input near full scale; no carrier (aim the dish, check
the frequency, GK-2A is not visible from Europe); carrier found but Es/N0 too low (with the Es/N0 needed); carrier locked, waiting for sync; frames but
Reed-Solomon fails; pictures encrypted and no key; a key is missing for some key number.

## Sensitivity

Measured with the built-in test signal (`tools/sens`, `sens_gk2a_gap`): Es/N0 is the symbol energy over the noise density, the noise bandwidth is the
symbol rate. "Ideal" is the same code chain with perfect timing, carrier and frame alignment and the same 8-bit soft Viterbi and Reed-Solomon decoders;
the gap is what the rest of the receiver (matched filter, timing, carrier, sync search) costs. Theory for this code: a BPSK / QPSK symbol carries 1 / 2 code
bits; Reed-Solomon (255,223) on a K = 7 r = 1/2 code works from Eb/N0 of about 1.9 dB for half the frames and 2.8 dB for nearly all (Es/N0 of a code symbol
= Eb/N0 - 3.6 dB; for QPSK add 3.0 dB for the two bits a symbol).

| Link | frames pass at 50 % | at 90 % | ideal chain at 50 % | gap |
|---|---|---|---|---|
| LRIT (BPSK, 128 ksym/s, 2.4 Msps, 20 kHz offset) | -1.58 dB Es/N0 | -1.32 dB | -1.71 dB | 0.13 dB |
| HRIT (QPSK, 3 Msym/s, 10 Msps, 120 kHz offset) | 1.47 dB | 1.73 dB | 1.30 dB | 0.17 dB |

(600 frames a point, 0.25 dB steps, `sens_gk2a_gap --link 0|1`, 2026-11-01. The 90 % columns for the ideal chain are -1.45 dB and 1.57 dB: the receiver stays 0.13 to 0.16 dB behind it.)

Real-world impairments are swept by `tools/sens/run.sh --mode 'gk2a'`: a carrier error of a 2 and a 20 ppm radio, a channel off the centre of the capture,
a 50 ppm sample clock error, a static echo, Rayleigh and Rician fading, phase noise, and all of them together.

## Status and what is unverified

**Unverified on air:** no GK-2A signal was available (nobody with a dish in view of 128.2 E has shared an IQ recording that could be found), so everything above is
built from the KMA specifications and tested against OnAir's own test signal (goes_gen / gk2a_gen, which sends the real layers). Things that could differ on air:

* the HRIT sync / I-Q mapping (G1 on I and G2 on Q is the usual for CCSDS QPSK; the receiver finds the swap and polarity itself, so a different mapping that is
  one of those eight cases works);
* the exact SCID, and whether the key header number is 2 or 4 bytes wide (the low 16 bits are used, as xrit-rx does);
* the header record 128 line number (counted from 0 here; if KMA counts from 1 the receiver uses `(sequence - 1) x lines`, which does not depend on it);
* the choice of JPEG 2000 options in KMA's encoder (the decoder handles every option except the selective arithmetic coding bypass, packed packet headers and
  ROI shifts, and says so);
* the encryption of the text files (the key header is honoured for them too).

## Credits

The KMA specifications; sam210723's xrit-rx for the key file layout and how the files are encrypted (read, nothing copied); the JPEG 2000 and DES code is OnAir's
own, checked against FFmpeg's encoder. See THIRD_PARTY.md.
