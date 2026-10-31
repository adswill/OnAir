# AOR AR5700D (and AR2300 / AR5001D / AR6000 with IQ5001)

Status: **file playback and sound-card input work; live USB I/Q streaming is NOT implemented.** Nothing here was tested with a real receiver.

## What the receiver gives a PC (from AOR's published documents)

- 10 kHz to 3.7 GHz. Two USB ports: one for receiver control (serial), one for the digital I/Q output.
- I/Q output: 1.125 Msps, 64 bits per sample (I and Q as 32-bit values, 9 MB/s, USB 2.0 isochronous), about 0.9 MHz usable bandwidth around the tuned frequency.
- The I/Q USB device is a Cypress FX2 that needs a firmware download after every power-on (AOR ships `fx2fw.hex`, `iqwr.sh`); afterwards it shows as USB `08d0:a001`, "AOR, LTD. DIGI-RECEIVER".
- Windows: AOR's "IQ drivers for all Windows versions" zip plus AR-IQ3 software (needs the USB licence dongle). Linux: AOR's `rel_210601` package (`ariq_rcv`, GPLv3) writes `iqdatIQ.bin`, float32 interleaved I/Q at 1.125 Msps.
- AOR does not publish the USB streaming protocol outside that GPL source and the (inaccessible to us) "I/Q digital output details for developers" manual chapter, so OnAir has no native driver. We did not copy or download AOR's code.

## What works in OnAir today

1. **Recorded I/Q files.** Record with AOR's `ariq_rcv` (Linux) or AR-IQ3 export. A file called `iqdatIQ*.bin` opens in File mode as cf32 at 1.125 Msps automatically (`guessFormat` / `guessSampleRate`). Set the centre frequency to the one you tuned the receiver to. Other file names: choose cf32 and 1.125 Msps by hand.
2. **Sound-card source** (`source_audio.cpp`, see `core/src/source/README.md`): feed the receiver's audio or IF output into a sound card / USB audio device and use the Sound card source for the narrow modes. Not a replacement for the I/Q stream.
3. **Diagnostics:** when the firmware-loaded I/Q device (`08d0:a001`) is plugged in on Linux, the device hints say so and point here.

## What is needed for live streaming (not done)

A libusb driver: download `fx2fw.hex` to the FX2 (vendor request 0xA0 to its 8051 RAM), send the capture-start command, read the isochronous endpoint, drop the leaked bytes (the `bf` value in `ariq_rcv`) and convert to cf32. This needs the GPLv3 `ariq_rcv.cc` as reference (OnAir is GPL-3 compatible, credit AOR Ltd. in THIRD_PARTY.md) and a real receiver to test. The remote-control side (frequency, mode) is a separate serial command set and could be added through the Hamlib rig source.
