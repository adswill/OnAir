# OnAir on a Raspberry Pi

**Status: untested on real hardware.** OnAir ships Linux arm64 builds that pass the automated tests on a GitHub arm64 runner, but nobody on the team owns a Raspberry Pi. Everything below is expected to work, not known to work. If you try it, please report what happened (issue tracker).

## Which Pi

- Raspberry Pi 5 and Pi 4 (4 GB or more) on **64-bit** Raspberry Pi OS (Bookworm or Trixie).
- Pi 3, Zero 2 W and 32-bit Pi OS are not built (no armhf package).

## Install

1. Download `onair_<version>_arm64.deb` from the releases page and run `sudo apt install ./onair_<version>_arm64.deb`. The package installs the udev rules and keeps the kernel's TV driver off RTL-SDR dongles.
2. No apt: unpack `onair-<version>-linux-aarch64-portable.tar.gz`, run `sudo ./install-udev-rules.sh` once, then start `onair`.
3. Replug the radio. If it is still not found over an SSH login, add yourself to the radio group: `sudo usermod -aG plugdev $USER`, then log in again.

The rules cover RTL-SDR (RTL2832U, including RTL-SDR Blog and Nooelec), Airspy (R2, Mini, HF+), HackRF (One, Pro) and several others (see `packaging/linux/60-onair-sdr.rules`).

## Headless use (roof or attic receiver)

Run `onair-server --lan` on the Pi and open the printed address in a browser on another device. See [headless-server.md](headless-server.md). To start it at boot, use `packaging/linux/onair-server.service` as a systemd unit. This is the recommended way to use a Pi: no desktop, less CPU, the spectrum and audio come to your phone or laptop.

## What should fit

| Pi 5 | Pi 4 |
| --- | --- |
| Narrow modes (FM, AM, airband, ADS-B, APRS, AIS, pagers, FT8/WSPR, LoRa, DMR, most data modes): expected fine. | Narrow modes (up to a few hundred kHz of bandwidth): expected fine. |
| DVB-T/T2, DTMB, ATSC, DVB-S2: may run at low rates, unverified. | DVB-T2, DTMB, ATSC and other wide TV modes: **not expected** to keep up in real time. |

## The GPU (Pi 4 and Pi 5)

The interface should be drawn by the V3D GPU. The terminal says which renderer OnAir got: `renderer: V3D 7.1 ...` is the GPU,
`renderer: llvmpipe ...` is the processor. Since 4.1, when desktop OpenGL comes back as llvmpipe and a GPU is there, OnAir tries OpenGL ES 3.0
through EGL and uses it when that reaches the GPU (`renderer: using OpenGL ES 3.0 through EGL on V3D ...`). The lines before it list the GPU
(`/dev/dri/renderD128 (v3d)`) and say when your user may not open it (add yourself to the `render` group and log in again) or when a variable such
as `LIBGL_ALWAYS_SOFTWARE` keeps Mesa on the processor. `ONAIR_GL=desktop` keeps desktop OpenGL, `ONAIR_GL=gles` asks for OpenGL ES first.
If `glxinfo -B` and `eglinfo -B` both show llvmpipe, Mesa's V3D driver is not used on that system at all and OnAir cannot change that.
The line `GpuLdpc: Vulkan device 0: V3D ... skipped, limits too small` is normal: only the DVB-T2 error correction would use Vulkan, and the V3D
is too small for it, so the processor does that part.

## Tips

- Use the lowest sample rate the mode needs; the Pi spends most of its CPU on the spectrum and the DSP.
- Close the GUI when the receiver runs unattended (headless server).
- Power the radio from a good supply; a powered USB hub helps HackRF and Airspy on a Pi 4.
- Use a fast SD card or an SSD for recordings, and a heatsink or fan: sustained decoding throttles an uncooled Pi.
- If audio is silent on the desktop, pick the output in the Pi OS sound menu (PipeWire/ALSA).
- If the window is black or slow, the GUI needs OpenGL: run it on the desktop session (not over plain SSH), or use the headless server.
- The Pi 4 and 5 GPU (V3D) offers OpenGL 3.1 and textures of at most 4096 pixels. OnAir falls back to OpenGL 3.1 / 3.0 on its own (#283)
  and keeps pictures wider than the GPU takes (the 8192-column waterfall) at 4096, keeping the strongest signal of each pair of
  columns (#284). `DECT2_MAX_TEXTURE=4096` sets the same limit on another computer, to try it.
