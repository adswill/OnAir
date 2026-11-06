# SSDV pictures

Mode list: **WEATHER & SCIENCE > SSDV pictures**. Receive only.

SSDV (Slow Scan Digital Video) is the picture format of high-altitude balloons and some amateur satellites: a baseline JPEG cut into 256-byte
packets (sync 0x55, type 0x66 with Reed-Solomon (255,223) or 0x67 without, callsign, image id, packet number, picture size, the entropy-coded
data, CRC-32 and 32 parity bytes). OnAir receives the packets from an FM / FSK channel, builds the picture while it arrives, and saves it.

## What it does

- One FM channel (the packet front end: offset, AFC, channel filter, discriminator) feeds a bank of bit slicers that run at the same time,
  each with its own clock loop and both frequency polarities:
  - **plain bit streams** at 1200, 2400, 4800, 9600 and 19200 baud (2-FSK / GFSK / MSK, NRZ);
  - **RTTY-framed bytes** (start bit, 8 data bits, one or two stop bits) at 300, 600 and 1200 baud, as sent by some UKHAS balloon trackers.
- The sync word 0x55 0x66 / 0x55 0x67 (one bit error allowed) starts a packet. The CRC-32 and the Reed-Solomon code decide: up to 16 wrong bytes per
  packet are corrected; anything else is dropped.
- Good packets are kept per picture (callsign and image id). The JPEG is rebuilt as packets come in, in any order, repeats ignored; **missing
  packets show as grey** until they arrive on a later pass.
- A finished picture (all packets up to the last one) is saved as a **JPEG** in `<Save pictures to>/SSDV`; a picture that stays incomplete for 20 s is
  saved as `...-partial.jpg` and replaced if more packets arrive. "Save PNG" writes a PNG copy to the picture folder.
- Advanced (the Tuner section): a fixed speed instead of Automatic.

## Wenet (Project Horus high-speed pictures)

Wenet is the image downlink of Project Horus balloons ("The Swift One", github.com/projecthorus/wenet). Receive only, in the same mode: it is the same SSDV
packets inside a different radio link, so it is a speed of the SSDV mode and not a mode of its own. **Needs a user test with a real recording**: it was
checked only with signals built from the Wenet transmitter's own framing, never with a real Wenet transmitter.

- **Radio:** 2-FSK, 115.2 kbaud (the Wenet transmitters actually run at 115177 .. 115386 baud: the clock loop follows that), deviation about +-72 kHz,
  about 200 kHz wide, usually on 70 cm (the Wenet nominal frequency is 443.5 MHz; it is not allowed in the 434 MHz ISM band). A second link, the sound-card
  ("I2S") transmitter at 96 kbaud, is received too. Because of the width the capture needs a **sample rate of 2 MHz or more** (700 kHz is the limit);
  Wenet has its own wide front end (the narrow SSDV front end of the other speeds cannot carry it).
- **Frame:** 16 x 0x55 preamble, unique word 0xABCDEF01, 256 payload bytes, CRC-16 (CCITT, init 0xFFFF, low byte first), 65 bytes of LDPC parity (rate 0.8,
  2064 information bits + 516 parity bits, repeat-accumulate code by Bill Cowley VK5DSP and David Rowe VK5DGR). On the 115.2 kbaud link every byte is a serial
  word (start bit, 8 data bits LSB first, stop bit); on the 96 kbaud link the bits go out raw, MSB first, XORed with a fixed 125-byte sequence.
- **Payload:** the first byte is the packet type. 0x55 is an SSDV packet: it goes through the same check, Reed-Solomon code and picture assembler as every
  other SSDV packet. Idle (0x56) and the telemetry / text types (0x00 .. 0x05, 0x54) are counted ("Wenet other") but not decoded.
- **Receiver:** mix out the offset, a +-120 kHz low-pass, FM discriminator, an AFC that follows the carrier (up to about +-100 kHz away from the tuned
  frequency), a bit clock locked on the transitions, a unique-word search (five wrong bits of 40 allowed, both polarities), the soft values of the frame
  into a layered min-sum LDPC decoder (up to 40 iterations), then the CRC. A bit lost or repeated by the transmitter's clock costs that frame only; the
  next unique word starts again.
- Advanced > Speed: **Automatic** tries both Wenet links with everything else; "Wenet 115.2 kbaud" or "Wenet 96 kbaud (I2S)" runs only that one.
  Other sound-card baud rates of the I2S transmitter (8, 24, 48 kbaud) are not tried.

## Not handled

These are not handled: SSDV inside AX.25 / IL2P or G3RUH scrambled frames (some satellites), Horus 4-FSK layers, LoRa SSDV, packets of other sizes than 256
bytes, the contents of Wenet telemetry packets (GPS, orientation, CBOR), and Wenet below 700 kHz sample rate. The Horus balloons mode decodes the Horus
telemetry, not the pictures.

## Licence

The packet check, the Reed-Solomon code and the JPEG rebuild are the code of **fsphil/ssdv** (Philip Heron, M0RPH; GPL-3.0-or-later), vendored in
`third_party/ssdv` (the Reed-Solomon routines are Phil Karn's, LGPL), see THIRD_PARTY.md and the credits in the program. The only change to the
original files is that its diagnostic messages to stderr are compiled out. JPEG pictures are shown with stb_image (public domain / MIT).

The Wenet part uses the code tables of **projecthorus/wenet** (Project Horus contributors, Mark Jessop VK5QI and others; GPL-3.0-or-later): the 516 x 12 parity-check
table `Hrow2064.txt` and the 125-byte scrambler sequence, in `core/src/ssdv/wenet_tables.cpp`; the framing, the CRC and the unique word follow its
source and wiki. The LDPC code itself is by Bill Cowley VK5DSP and David Rowe VK5DGR (codec2). The decoder, the demodulator and the generator are new code. The
reference test vector (`tests/ssdv/wenet_vector.h`) is from the same repository. Credit line in THIRD_PARTY.md and in the program's credits.

## How it was checked

With the SSDV encoder's own packets of a test picture (the encoder of the same library) as 2-FSK / GFSK in the generator: clean at 1200 - 19200 baud
and RTTY-framed at 300 / 600 baud; carrier offsets of +-3 kHz, swapped frequencies, a clock off by 1000 ppm, a carrier drift of 200 Hz/s, noise (Eb/N0
16 dB), all of them together, packets with 6 wrong bytes (the Reed-Solomon code repairs them); a fixed wrong speed gives nothing; noise gives nothing.
The picture counts as received when its JPEG is byte for byte the one the packets give directly. No real recording was used.

Wenet: the LDPC decoder reproduces the Wenet reference vector (2580 soft values, every bit); the encoder gives the same parity; a CRC-16 check value; then a
test picture sent as Wenet frames (115.2 kbaud serial-word framing and 96 kbaud raw scrambled) at 2 Msps: clean, carrier offsets of +-20 and +-50 kHz, swapped
frequencies, clock errors of +-1500 ppm, a drift of 2 kHz/s, Gaussian and rectangular bits, noise, all together, a bit lost or repeated every 15000 bits, sample
rates of 1, 2.4, 3.2 and 4 MHz; noise alone and a fixed wrong speed give nothing. The noise figures are in the test output (`ctest -R ssdv_wenet -V`).

Tests: `ctest -R ssdv` (includes `ssdv_wenet`) (`tests/ssdv`), wiring in `tests/app/test_ssdv_mode.cpp`.

## From a WAV recording (3.7)

Choose **File** as the source, press **Open** and pick the WAV. Any sample rate, mono or stereo (the first channel), 8 / 16 / 24 / 32-bit PCM or 32 / 64-bit float. The
file must hold the **audio of an FM receiver carrying the FSK waveform** (the discriminator output: 9600 baud G3RUH-style or any of the 1200 - 19200 baud speeds); OnAir
turns it back into a frequency-modulated carrier (core/src/ssdv/ssdv_audio.cpp) and the normal receiver decodes it. A two-channel IQ WAV is still played as IQ. AFSK tone
pairs (1200 / 2200 Hz style) are not demodulated by this path. Tested on generated packets in 6 WAV layouts (tests/ssdv/test_ssdv_audio.cpp); no real recording yet.
