# Ettus USRP B210 (and B200 / B200mini / B205mini) on Windows

The OnAir Windows package does **not** carry UHD, the USRP firmware images or the Ettus USB driver. A B210 needs three things
installed once by the user. OnAir then talks to UHD through its C API (`uhd_usrp_*`), loaded at run time like the other radio libraries
(`core/src/source/source_usrp.cpp`).

## What to install

1. **UHD for Windows (Ettus installer, 64 bit, UHD 4.x; 3.15 or newer works).**
   From Ettus Research (files.ettus.com/binary_installers or the "UHD binary installers" page). The installer puts these in
   `C:\Program Files\UHD`:
   - `bin\uhd.dll` and the DLLs it needs (Boost, `libusb-1.0.dll`, and the MSVC runtime if it is missing). Keep them together in `bin`.
   - `share\uhd\images` (firmware / FPGA images of that UHD version, including the B2xx ones). If the images are missing, run
     `uhd_images_downloader.py` once (installed in `lib\uhd\utils`) or install the "images" package of the same version.
   - `share\uhd\usbdriver` (the USB driver files for the B2xx).
   Let the installer add `C:\Program Files\UHD\bin` to the `PATH` (a tick box in the installer). Without it OnAir cannot find `uhd.dll`.
   Set `UHD_IMAGES_DIR` to `C:\Program Files\UHD\share\uhd\images` if UHD does not find its images by itself.
   Install the Microsoft Visual C++ redistributable (x64) if `uhd.dll` reports a missing `VCRUNTIME` / `MSVCP` DLL.
2. **The USB driver.** Plug in the B210 (USB 3.0 port, USB 3 cable). In Device Manager the radio shows as "WestBridge" /
   "Cypress FX3" / unknown device without a driver. Right-click it, "Update driver", "Browse my computer", point to
   `C:\Program Files\UHD\share\uhd\usbdriver`, and accept the unsigned-publisher prompt for the Ettus driver. (A WinUSB driver made with
   Zadig for the same device also works.) After the driver is in, UHD loads the firmware: the device re-appears as "USRP B210"
   and the LED changes.
3. **Check with Ettus' own tool first:** open a command prompt and run `uhd_find_devices` and then `uhd_usrp_probe`. If those do not show
   the B210, OnAir will not either; fix that first (driver, images, USB port).

## What OnAir does

- OnAir looks for `uhd.dll` (the Ettus install, on the `PATH`) first and for a `libuhd.dll` next to the program only after that. The
  package does not ship a `libuhd.dll`, so the radio shows up only when UHD is installed. The UHD DLLs are loaded with their own folder in
  the search path, so `libusb-1.0.dll` and Boost are taken from `C:\Program Files\UHD\bin`; do not copy them next to `onair.exe`.
- A B210 only appears after UHD has loaded its firmware from the images of **its own version** (`UHD_IMAGES_DIR`). Mixing a newer
  `uhd.dll` with old images (or the other way round) makes the radio disappear.
- Use a B210 with sample rates up to about 30 Msps on USB 3 (61.44 Msps needs one channel and a good USB 3 controller). On USB 2 only a
  few Msps work.
- Receive and transmit use the same UHD handle; see `docs/DEVICES.md` for the transmit notes.

## If the radio is not found

| Symptom | Cause | Fix |
|---|---|---|
| No USRP in the radio list | `uhd.dll` not on the `PATH` | add `C:\Program Files\UHD\bin` to `PATH`, restart OnAir |
| "USRP firmware/FPGA images not found" | images missing or another version | run `uhd_images_downloader.py`, or set `UHD_IMAGES_DIR` |
| `uhd_find_devices` finds nothing | no USB driver | install it from `share\uhd\usbdriver` (step 2), use a USB 3 port |
| Starts, then drops samples (O / overflow) | USB 2 port, hub, long cable or too high a rate | direct USB 3 port, 10 Msps or lower |
| A DLL is "not a valid Win32 application" | 32 bit UHD with the 64 bit program | install the 64 bit UHD |

Not tested on real hardware by the OnAir developers on Windows: the steps above follow the Ettus installer layout and the loader in
`source_usrp.cpp`; please report differences.
