# Tektronix RSA306, RSA306B, RSA500A and RSA600A

**Experimental, receive only. Tried on a real RSA306 (see "Measured on an RSA306" below); the RSA306B, RSA500A and RSA600A use the same
API and are listed, but nobody has tried them with OnAir.**

OnAir drives Tektronix's USB real-time spectrum analysers through Tektronix's **RSA API**, the C library that Tektronix offers for
them. OnAir does not ship it; it is loaded at run time when it is installed.

## Install

1. Download and install Tektronix's **RSA API** (free; 64-bit Windows or Linux, no macOS build):
   - Windows: "RSA Application Programming Interface (API) for 64-bit Windows". Version 5.6.0295, the one this driver was tried with:
     [tek.com, RSA API for 64-bit Windows V5.6.0295](https://www.tek.com/en/support/software/application/rsa-application-programming-interface-api-for-64bit-windows-v560295).
   - Linux, or a newer version: on [tek.com](https://www.tek.com) go to Support > Software and search for "RSA API"; Tektronix has one
     page per version and system.
   - On Windows it goes to `C:\Tektronix\RSA_API` and brings the USB driver.
2. Close **SignalVu-PC** and any other program that uses the analyser: it can be connected to one program at a time.
3. Start OnAir and press the refresh button next to the radio list. The device appears as `Tektronix RSA306 <serial> (experimental)`.

OnAir looks for the library in these places:

- Windows: `C:\Tektronix\RSA_API\lib\x64\RSA_API.dll`. The libraries it needs (`RSA300API.dll`, `RSA500API.dll`, `SharedUtils.dll` ...)
  are loaded from the same folder.
- Linux: `libRSA_API.so` in `/opt/tektronix/RSA_API/lib`, `/usr/local/lib`, `/usr/lib`, or wherever the loader finds it.
- Installed elsewhere: set `ONAIR_RSA_DIR` to the folder that holds the library before starting OnAir.

The log (the terminal, or `onair.log` on Windows) says which library was loaded and how many devices the RSA API lists. Without the
library the device is not listed; when an RSA306 or RSA500A / RSA600A is plugged in (USB 0699:a306 or 0699:a500) the radio list and
Help > Hardware show a hint that says what to download, and the Fix it panel has a button that opens the download page.

## What it does

- **Rates.** The API's IQ streaming has thirteen rates: 56 Msps divided by 1, 2, 4 ... 4096 (56, 28, 14, 7, 3.5, 1.75 Msps, 875,
  437.5, 218.75, 109.375 ksps, 54687.5, 27343.75 and 13671.875 sps), each with an acquisition bandwidth of 40 MHz divided by the same
  (5/7 of the rate). OnAir takes the slowest one at or above the rate that was asked for and works with that rate.
- **Retunes.** The device takes no new frequency, level or rate while it runs, so OnAir stops it, sets the values and runs it again
  at every change. That takes about 50 ms, in which no samples arrive.
- **Gain** sets the reference level (`CONFIG_SetReferenceLevel`): 0 dB = +30 dBm, each dB more is 1 dB lower, up to 75 dB = -45 dBm.
  Beyond about 55 dB (-25 dBm) the analyser adds no sensitivity, only level; auto gain finds the best point by itself, and its "ADC
  level low" note can be left alone when the channel's SNR is good. The samples arrive in volts into 50 ohm; OnAir scales them so that the reference level is 1.0.
- **Radio settings:** frequency correction (ppm) and the RF preamplifier of the models that have one (RSA500A, RSA600A). An RSA306 has
  none: with "On" the start message says that the device did not take it. The attenuator is left to the API (automatic).
- **The device stays connected** from its first start until OnAir is closed. Connecting takes 1 to 3.5 s and disconnecting 4 s, while a
  start on a connected device takes 50 ms and a stop 30 ms. So a stop only ends the stream; SignalVu-PC can use the analyser again once
  OnAir is closed.
- **One device at a time.** The RSA API talks to one analyser per program. With two plugged in, both are listed and the one that is
  started is the one in use; starting the other one lets go of the first.
- An unplugged device is looked for and connected again every second until it is back, as for OnAir's other radios.

## Measured on an RSA306

On an RSA306 (firmware V1.7, FPGA V1.1, hardware V5) with the RSA API 5.6.0295 on Windows 11 and a USB 3 port, a wire antenna for
short wave on its input and an IC-705 on the same antenna for comparison.

- `DEVICE_Search` 30 to 470 ms; `DEVICE_Connect` 1.1 to 3.4 s; `DEVICE_Disconnect` 4.0 to 4.1 s. A program that ends without
  disconnecting leaves the device usable for the next one.
- `DEVICE_Run` + `IQSTREAM_Start`: 30 ms, the first block of samples 22 to 70 ms later. `IQSTREAM_Stop` + `DEVICE_Stop`: 10 to 40 ms.
  Five starts and stops in a row, and one after a disconnect and connect, all delivered samples.
- The rates are the ones of the API's manual, exactly (the manual's "24373.75 Sa/s" is 27343.75). A bandwidth between two steps takes
  the rate of the next wider one; above 40 MHz the call returns "IQ Stream Bandwidth Out Of Range" and 40 MHz is taken.
- At 56 Msps for 6 s: 99.94 % of the samples in the time, no status bit set (no loss), on a computer that was compiling at the time.
- Samples: `IQSODT_SINGLE` is float I/Q in volts, the block's `scaleFactor` is 1. Blocks of 65536 samples up to 3.5 Msps, 131072 above.
- While it streams, `CONFIG_SetCenterFreq` returns no error and the device stays where it was: the carriers keep their places in the
  samples until `DEVICE_Stop` and `DEVICE_Run`, after which they are where the new centre puts them. `IQSTREAM_SetAcqBandwidth` is
  accepted too, and the samples that follow are garbage (volts in the hundreds) until the device was stopped and run again. A retune
  through OnAir's driver (stop, set, run) takes 40 to 50 ms.
- The centre frequency goes from 9 kHz to 6.2 GHz; the set functions take any value while the device is stopped.
- The reference level against what arrives (an FM station on 105.9 MHz, a whip, 7 Msps): at +20, 0, -20, -30, -45 and -70 dBm the
  noise and signal at the input read -37, -56, -66, -67, -68 and -67 dBm. Below about -25 dBm the device adds no sensitivity. The float
  samples are not clipped at the reference level: at -70 dBm the peak is 22 dB above it, and the overrange bit stays clear throughout.
  OnAir counts samples beyond 1.0 as an overflow, so the gain stops at -45 dBm, where the peaks of this band were 1 dB below 1.0 (at
  56 Msps, with the whole FM band in the capture, they reach 1.0 at about -35 dBm).
- No preamplifier: `CONFIG_SetRFPreampEnable(true)` returns no error and `CONFIG_GetRFPreampEnable` still says off.
- Frequency and side: broadcast carriers on short wave (3.955 MHz and others) are on their frequencies, not mirrored.
- `ALIGN_GetAlignmentNeeded` says yes and `ALIGN_GetWarmupStatus` no right after the device was plugged in; OnAir does not run the
  alignment (it stops the device for seconds, and reception does not need it).

Not tried: the other models, Linux, a second device, the external 10 MHz reference, triggers, and the manual attenuator.

## Tests

`tests/source/test_rsa.cpp` and `tests/source/test_rsa_missing.cpp` run the driver against `tests/source/rsa_fake.cpp`, a stand-in
library with the same function names that behaves as the measured RSA306: the list, a device in use elsewhere, the rate steps, the
level and scale, the order of I and Q (a tone at +rate/8 must come out at +rate/8), retunes and a new rate (nothing is set while the
device runs), a stop that keeps the connection, the preamplifier with and without one, an unplug, a second source, and a
missing library.

## Test checklist (for someone with another model)

Close SignalVu-PC first. Note OnAir's version, the operating system, the model and the RSA API version. Attach the log (the terminal
output, or `onair.log` on Windows) to the report.

1. **Listing.** Is the device in the radio list with its model? Does the log show `radio library: ...RSA_API...` and
   `Tektronix RSA: connected to ...` at the first start?
2. **FM broadcast.** Mode FM, a strong local station. Does it play, at the right frequency (no offset, not mirrored)?
3. **Gain.** Move the gain slider while it plays. Does the level follow, without dropouts?
4. **Rates.** Try 3.5, 14, 28 and 56 Msps. Does the waterfall stay continuous? Any "samples were lost" line in the log?
5. **Retune.** Type frequencies far apart (7 MHz, 100 MHz, 1 GHz, the top of the model). Does it follow at once?
6. **Preamplifier** (RSA500A / RSA600A). Radio settings, RF preamplifier = On, restart: does the noise floor change?
7. **Stop and start.** Ten times in a row. Is every start immediate?
8. **Unplug.** Pull the USB cable while receiving, wait 5 s, plug it in. Does the stream come back by itself?
