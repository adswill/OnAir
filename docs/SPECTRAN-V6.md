# Aaronia SPECTRAN V6 and V6 ECO

**Experimental: written from Aaronia's public documentation and tested only against a fake RTSA API. It has not run on a real SPECTRAN.
Reports are welcome (issue #482).**

OnAir drives the SPECTRAN V6 and the V6 ECO through Aaronia's **RTSA API**, the C library that comes with the **RTSA-Suite PRO**.
OnAir does not ship it; it is loaded at run time when it is installed. Receive and transmit both go through it.

## Install

1. Install the **RTSA-Suite PRO** from [aaronia.com/downloads](https://aaronia.com/downloads) (64-bit Windows or Linux; Aaronia
   offers no macOS build).
   - Windows: the default folder is `C:\Program Files\Aaronia AG\Aaronia RTSA-Suite PRO`. Choose "Install SPECTRAN V6 driver" in the
     installer.
   - Linux: the default folder is `/opt/aaronia-rtsa-suite/Aaronia-RTSA-Suite-PRO`. Add Aaronia's udev rule as its installer or manual
     explains, then unplug and replug the device. For the high rates, raise the USB buffer memory (kernel parameter
     `usbcore.usbfs_memory_mb`, often 16 MB by default) as Aaronia's Linux notes describe.
2. Start the RTSA-Suite PRO once with the device plugged in, so that it can update the device's firmware, then **close it**: the device
   can be open in one program at a time.
3. Start OnAir and press the refresh button next to the radio list. The device appears as `Aaronia SPECTRAN V6 <serial> (experimental)`
   or `Aaronia SPECTRAN V6 ECO <serial> (experimental)`.

OnAir looks for the library in these places:

- Windows: `<install>\sdk\AaroniaRTSAAPI.dll`, then `<install>\AaroniaRTSAAPI.dll`, where `<install>` is the folder above (under
  `ProgramW6432` / `ProgramFiles`). The libraries it needs are loaded from `<install>`.
- Linux: `<install>/libAaroniaRTSAAPI.so`, then `<install>/sdk/libAaroniaRTSAAPI.so`. If the library cannot find its own dependencies,
  start OnAir with `LD_LIBRARY_PATH=/opt/aaronia-rtsa-suite/Aaronia-RTSA-Suite-PRO`.
- Installed elsewhere: set `ONAIR_RTSA_DIR` to the install folder before starting OnAir.

The log (the terminal, or `onair.log` on Windows) says which library was loaded and how many devices the RTSA API lists. Without the
library, starting the device says "Aaronia RTSA-Suite PRO is not installed", and the Fix it panel links to Aaronia's download page.
When an Aaronia USB device (vendor id 2f72) is plugged in but the library is missing, the radio list shows a hint as well.

## What it does

- **Receive.** The device is opened as `spectranv6/iqreceiver` (or `spectranv6eco/iqreceiver`). The RTSA API resamples, so any sample
  rate works: OnAir sets `main/spanfreq` to the rate / 1.5 (Aaronia's README: the packets come at 1.5 x the span) and uses the rate that the
  packets report. On the V6 the receiver clock is the slowest one that carries the rate (46.08 to 245.76 MHz), or the one chosen in the
  radio settings. OnAir sets no limit of its own; what the device cannot do it adjusts, and the start message says so.
- **Raw mode** (radio settings: Mode = Raw): `spectranv6/raw`, the receiver clock divided by 1 to 512 (`main/decimation`). OnAir takes the
  smallest of those rates at or above what the mode asks for. The V6 ECO runs at 61.44 MHz in this mode.
- **Gain** sets the reference level (`main/reflevel`, the level that fills the converter): 0 dB = +10 dBm, each dB more is 1 dB lower
  (30 dB = -20 dBm). It changes while the stream runs. The samples arrive in volts; OnAir scales them so that the reference level is 1.0.
- **Radio settings:** frequency correction (ppm), Mode, RF amplifier (`calibration/preamp`), USB compression, stream clock source (GPS,
  PPS, 10 MHz reference), TX lead time; on the V6 also the receiver clock, the RX 1 / RX 2 input and the gain control (manual, peak, power).
  "Keep" leaves the device's own setting alone.
- **Transmit** (the devices with the TX option): the device is opened as `spectranv6/iqtransmitter`. `main/centerfreq`, `main/spanfreq`
  (the sample rate) and `main/transgain` (the TX gain in dB) are set, and the samples go out as timed packets, kept a lead time ahead of the
  device's stream clock (50 ms on the V6, 500 ms on the V6 ECO, as Aaronia's samples; radio setting "TX lead time"). Full scale (1.0) is
  sent as 0 dBm before `main/transgain`. OnAir warns but never blocks a transmission; frequency, rate and level are passed as given.
- The receiver and the transmitter cannot have the device at the same time (it is opened in one mode at a time). Full duplex through
  `iqtransceiver` is not used yet.
- An unplugged device is opened again every second until it is back, as for OnAir's other radios.

## Not verified on a device

Everything below comes from Aaronia's public samples and README and from how SDR++ and SatDump use the API, not from a measurement:

- the library paths and `AARTSAAPI_Init_With_Path` with the install folder (older libraries without it get `AARTSAAPI_Init`)
- the layouts of `AARTSAAPI_Packet` and `AARTSAAPI_DeviceInfo` and the result codes (declared in `core/src/source/aartsa_min.h`)
- the sample scale: volts, 0 dBm = an amplitude of sqrt(0.05); a different scale only moves OnAir's level meter, the AGC copes
- the reference level range (the device adjusts it; OnAir reads back what it took) and the RF amplifier names of each model
- that the iqreceiver rate is 1.5 x the span on every model, and the receiver clock names the V6 accepts
- transmit: the packet times and `startFrequency` = centre - rate / 2, the span OnAir sets (= the rate), the `main/transgain` range, the
  lead times, and the end of a transmission (a short silent packet with the end flags)
- the frequency ranges OnAir shows (9 kHz to 20 GHz for the V6, 9 kHz to 8 GHz for the ECO) are notes only; the RTSA API decides

The tests (`tests/source/test_spectran.cpp`, `tests/source/test_spectran_missing.cpp`) run the driver against
`tests/source/spectran_fake.cpp`, a stand-in library with the same function names: listing, both receive modes, the rate plan, the
level and scale, the order of I and Q (a tone at +rate/8 must come out at +rate/8), live retunes, rate changes (nothing that sets the rate
is sent while the device streams), an unplug, a busy device, transmit pacing, gaps and flags, a missing library and a library that does
not start. They show the driver follows the documentation; they cannot show the documentation matches the device.

## Test checklist (for someone with the device)

Close the RTSA-Suite PRO first. Note OnAir's version, the operating system, the model (V6 / V6 PLUS / V6 ECO) and the RTSA-Suite PRO
version. Attach the log (the terminal output, or `onair.log` on Windows) to the report.

1. **Listing.** Is the device in the radio list? Does the log show `radio library: ...AaroniaRTSAAPI...` and `SPECTRAN: the RTSA API lists 1 device(s)`?
2. **FM broadcast.** Pick the device, mode FM, a strong local station, sample rate left at the default. Does it play? Is the station at
   the right frequency (no offset, not mirrored)? Does the log say `the device delivers ... Msps`, and is that the rate the app shows?
3. **Gain.** Move the gain slider from 0 to 60 dB while it plays. Does the level change smoothly, with no dropouts? Any "adjusted by the
   device" note? At what gain does a strong station start to distort?
4. **Rates.** Try 1, 10, 50 Msps (V6 ECO: up to 61.44) and on the V6 also 100 and 245.76 Msps. Does the waterfall stay continuous? Any
   USB errors in the log?
5. **Raw mode.** Radio settings, Mode = Raw, restart. Same FM station: does it still play, and at which rate?
6. **Retune.** Click across the band and type frequencies far apart (100 MHz, 1 GHz, 2.4 GHz). Does it follow at once?
7. **Unplug.** Pull the USB cable while receiving, wait 5 s, plug it in. Does the stream come back by itself?
8. **Transmit (into a dummy load or an attenuator, on a frequency you may use).** Open the TX window, choose the SPECTRAN, send a carrier
   or a test tone at -30 dB TX gain. Is it on the right frequency? Is the level roughly 0 dBm + TX gain? Any gaps or clicks (try TX lead
   time 0.05, 0.2 and 0.5 s)? Does it stop cleanly when you stop it?
9. **Other model.** If a second SPECTRAN model is at hand (V6 PLUS and V6 ECO), repeat 1, 2 and 8 with it.
