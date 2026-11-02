# OnAir Android: radio access (USB, network, sound card)

What `android-usb-030` adds: USB radios through Android's `UsbManager` (no root), the network radios of the core, and the sound-card
input through `AudioRecord`. The UI screens (`android-ui-030`) draw it; this layer holds the state and talks to the core.

## Supported radios

| Radio | USB id(s) | Driver on the phone | Needs in the build | Notes |
| --- | --- | --- | --- | --- |
| RTL-SDR (R820T/R820T2/R828D): generic sticks, RTL-SDR Blog V3, V4, V4L, NooElec NESDR | `0bda:2832`, `0bda:2838` + the DVB-T sticks of librtlsdr's list (Terratec, Compro, GTek, Twintech, Dexatek, Genius, HanfTek) | the core's Rust RTL2832U driver over usbfs ioctls (`rtl_fd_shim.h`, `rust/onair_usb`), no libusb | nothing extra (`DECT2_RUST_USB=ON` is set by `native/CMakeLists.txt`) | up to 2.4 Msps on phones (2.56 max); bias tee on the Blog sticks |
| HackRF One, HackRF Pro (and Jawbreaker, rad1o) | `1d50:6089`, `1d50:604b`, `1d50:cc15` | OnAir's own HackRF driver (`core/src/source/hackrf_usb.cpp`) on libusb, the descriptor wrapped with `libusb_wrap_sys_device` | `LIBUSB_SRC` (libusb ≥ 1.0.24 source) | the board id tells One from Pro once opened; one gain slider split over LNA/VGA/amp |
| HydraSDR RFOne | `38af:0001` (firmware 1.0.2 and later; `1d50:60a1` before) | not on Android yet: the desktop driver (`source_hydrasdr.cpp`) would need the descriptor entry point `hydrasdr_open_fd` wired like the Airspy's |
| Airspy R2 / Mini | `1d50:60a1` | libairspy (`airspy_open_fd`) loaded by `source_airspy.cpp` | `LIBUSB_SRC` + `AIRSPY_SRC` | a libairspy without `airspy_open_fd` is refused on Android |
| Airspy HF+ (Dual, Discovery) | `03eb:800c` | libairspyhf (`airspyhf_open_fd`) loaded by `source_airspyhf.cpp` | `LIBUSB_SRC` + `AIRSPYHF_SRC` | same rule |
| SDRplay (RSP1A, RSPdx, ...) | — | **not supported** | — | the SDRplay API is a closed binary service with no Android build; use SpyServer/rtl_tcp... on a computer, or SDRconnect's server |

The table lives in three places that must agree: `res/xml/usb_device_filter.xml` (Android's "open with" filter),
`usb/UsbCatalog.kt` (names, ranges, power) and `usbFdKindForId()` in `core/src/source/usb_fd.cpp`. `DeviceFilterTest` checks the first
two; the C++ test `usb_fd_open` checks the core's ids.

## Permission flow

1. **Plugged in while OnAir is closed.** `MainActivity` has a `USB_DEVICE_ATTACHED` intent filter with the device filter. Android asks
   "Open OnAir to handle …?" with an *Always open for this device* box. Ticking it grants the permission for good (until the app is
   reinstalled); OnAir starts and `MainActivity.usbAttached` hands the device to the radio layer.
2. **Plugged in while OnAir runs.** `AndroidUsbBackend.listen` receives the attach broadcast; without a permission the system dialog
   is shown at once (`UsbManager.requestPermission` with a mutable, package-explicit `PendingIntent`, as Android 12/14 require).
3. **At start** (`RadioRepository.start` → `UsbRadioController.rescan(askPermission = false)`): radios already plugged in are listed;
   those with a permission are opened, the others wait for a tap (no dialog appears by itself at app start).
4. **Answer.** Yes → `openDevice` → the descriptor (`UsbDeviceConnection.fileDescriptor`) and the strings (serial is readable only now)
   go to the core (`RadioNative.nativeUsbAttach` → `usbFdAttach`). No → `DENIED`, with "tap to ask again".

States (`UsbRadioStatus`): `NEEDS_PERMISSION`, `ASKING`, `DENIED`, `READY`, `FAILED`, `NO_DRIVER` (this build has no driver for it). The
device picker (`usb/UsbDevicePicker.kt`) shows them with a plain-language line each.

## Hot-plug

`USB_DEVICE_DETACHED` → `UsbRadioController.detached`, strictly in this order:

1. `onLost`: when the radio is the one in use, the repository stops the session (`ReceiverRepository.stopBecause`) and waits until the
   engine let go (the driver may already have stopped by itself: usbfs answers `ENODEV`, the RTL stream reports `kUsbNoDevice`);
2. the core forgets the descriptor (`nativeUsbDetach`);
3. the connection is closed.

So no driver ever reads a closed (or reused) descriptor. The user sees "The radio was unplugged (or lost power) …". Plugging it back in
gives a new device name and descriptor; the chosen radio is remembered as `vid:pid:serial` (`Settings.usbDevice`), so it is picked
again. A missed broadcast is caught by the next `rescan`.

## Into the core ("open from fd")

- Registry: `dect2/source/usb_fd.h` (`usbFdAttach/Detach/List`, `usbFdPlatform()` = true on Android: no driver scans the bus).
- `usbFdOpen(kind, serial, wrap, …)`: picks the radio (serial whole or its end) and calls `wrap(fd)`; failures come back as plain
  words (`usbFdErrorText`: permission, unplugged/power, another app, timeout/cable …). The C++ test `tests/source/test_usb_fd_open.cpp`
  runs it with a stub wrapper.
- HackRF: `acquireContext` sets `LIBUSB_OPTION_NO_DEVICE_DISCOVERY` on the default context before `libusb_init` (no `/dev/bus/usb` scan,
  which fails without root), `openHackrfFromFd` wraps the descriptor; listing wraps each one for a moment to read the board id.
  `libusb_close` of a wrapped handle leaves the descriptor open (the app owns it).
- Airspy / HF+: `open_fd` is an optional symbol of the loaded library; on Android only it is used.
- The bridge: `SourceKind.USB` (= 4) with `host` = the registry name; `jni_radio.cpp` `radioDevice` finds the driver's entry for it.

### Transfers

The RTL-SDR stream keeps **64 URBs of 16 KiB** in flight (about 1 MiB, ~100 ms at 2.4 Msps × 2 bytes ≈ 4.8 MB/s) and hands the
receiver 64 KiB blocks (0.2.8 fix d554b573, `usb_fd_io.cpp` SUBMITURB/REAPURB); 16 KiB per URB is what older usbfs kernels accept
per bulk request. The HackRF keeps **8 × 256 KiB** in flight (0.1 s at 10 Msps), as on the desktop; libusb splits them for usbfs.
Phones with weak USB controllers drop samples above ~10 Msps: the HackRF's mode rates stay at 8–10 Msps unless a mode needs 20.

## Power (OTG)

`UsbCatalog.Model.typicalMa` and the configuration's `maxPower` give a warning line for radios that draw a lot: HackRF ~500 mA,
Airspy R2 ~400 mA (warning), RTL-SDR / Airspy Mini / HF+ 260–300 mA (a softer note at ≥280 mA). Many phones give 500 mA or less on
OTG and cut it when the battery is low; the remedy is a powered hub or an OTG Y cable. A radio that disconnects in the middle of a
session gets the "unplugged (or lost power)" message.

## Network radios

`netradio/NetworkRadio.kt`: `NetRadioType` = the core's `NetRadio::Type` (rtl_tcp, SpyServer, KiwiSDR, TCI, SDR++ server; SoapyRemote
is listed but refused: it needs SoapySDR, which the app does not have). The list is stored in the core's own text form
(`rtltcp,host,port;kiwisdr,host,port,password`) plus the user's labels (`Settings.netRadios`, `netSelected`), so the format is the one
the desktop and the Rust parser use. `AddNetworkRadioForm` is the data model of the "Add network radio" dialog (fields, `withType`
keeps the default port in step, `validate` gives per-field plain-language errors; "host:port" pasted into the host field is split).
`RadioRepository.addNetworkRadio / removeNetworkRadio / selectNetworkRadio / probe / describe`. The connect timeout is the core's
8 s (`source_rtltcp.cpp` `kConnectMs`); `probe` uses the same.

**Discovery** (`NetDiscovery`, NsdManager / DNS-SD): **off by default** (`Settings.netDiscovery = false`, the 0.2.9 rule: no network
scans unless the user asks). It runs only while the user allowed it *and* the add dialog is open (`discoveryWanted(true/false)`),
and it only listens to announcements (`_rtl_tcp._tcp`, `_spyserver._tcp`, `_kiwisdr._tcp`, `_sdrpp._tcp`); none of these is an
official type, so most servers are not found and the address is typed. "OnAir Web" (a phone receiving from another OnAir) comes later.

## Sound card

`SourceKind.SOUND_CARD` (= 6): `audio/SoundCardInput.kt` records stereo float at 48 kHz with `AudioRecord` (source `UNPROCESSED`
where the phone has it, so no noise suppression or AGC alters the IF; else `VOICE_RECOGNITION`, else `MIC`) on its own thread and
pushes 20 ms blocks into the core (`dect2/source/audio_push.h`, a 1 s ring that drops the oldest when the receiver is slow). The core's
sound-card source (`source_audio.cpp`, device serial `push`) does the rest exactly as on the desktop (IQ mapping, IF offset,
interpolation for modes that want more than 48 kHz). The microphone is open only during such a session; `RECORD_AUDIO` is declared
and the UI asks for it; without it the start says so in words.

## Foreground service

`ReceiverService.foregroundType`: `mediaPlayback` always, `connectedDevice` while a granted USB radio is the source, `microphone`
while the sound card records. Android 14 throws when a type's prerequisite is missing, so a type is only added when it holds.

## Building with HackRF / Airspy

```sh
git clone --branch v1.0.27 https://github.com/libusb/libusb ~/src/libusb          # LGPL-2.1: ship its licence, offer its source
git clone https://github.com/airspy/airspyone_host ~/src/airspyone_host            # optional, BSD
git clone https://github.com/airspy/airspyhf ~/src/airspyhf                        # optional, BSD
LIBUSB_SRC=~/src/libusb AIRSPY_SRC=~/src/airspyone_host AIRSPYHF_SRC=~/src/airspyhf android/tools/build_native.sh
```

This puts `libusb1.0.so` (+ `libairspy.so`, `libairspyhf.so`) next to `libonair.so` in `jniLibs/arm64-v8a/`. Without `LIBUSB_SRC`
the APK has the RTL-SDR only; the HackRF/Airspy entries then say "not available in this build" (`nativeUsbFeatures`). The build
machine's `libusb.h` (pkg-config; Homebrew's is 1.0.27) must be ≥ 1.0.24, or the HackRF descriptor path is compiled out.

## Known limits

- No SDRplay (closed API). No bladeRF, LimeSDR, PlutoSDR (USB), RX888, FOBOS on USB yet (their libraries are not built for Android;
  a PlutoSDR works over the network as on the desktop when the core's network source covers it).
- Airspy needs a libairspy / libairspyhf source that has `airspy_open_fd` / `airspyhf_open_fd` (check the checkout: older releases
  do not); the core refuses an Airspy library without it on Android rather than letting it scan the bus.
- Two radios at once: the registry and the drivers handle several, the app runs one session (one engine).
- One permission dialog per plug-in; "always open" is per device and is forgotten when the app is reinstalled.
- mDNS finds only servers that announce themselves.

## Not verified without a phone

Everything above was written and checked statically: the C++ compiles with the NDK (`-fsyntax-only`), `test_usb_fd_open` passes on
macOS, the Kotlin JVM tests (`DeviceFilterTest`, `UsbRadioControllerTest`, `NetworkRadioTest`) pass, `tools/check_jni.py` is clean.
Not tested: a real dongle through a real Android descriptor (RTL, HackRF, Airspy), the libusb Android build with `LIBUSB_SRC`, the
permission dialog and "always open" behaviour on Android 9–15, attach broadcasts delivered to a not-exported receiver on every
vendor's Android, the `connectedDevice`/`microphone` foreground types on Android 14+, `AudioRecord UNPROCESSED` on real phones,
NsdManager on Android 14+ (`hostAddresses`), and sample loss at 10–20 Msps on phone USB controllers.
