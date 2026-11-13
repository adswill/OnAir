# OnAir on Linux Mint (4.0)

A Discord report said OnAir did not work on the newest Linux Mint. The user moved to Ubuntu, where it worked. A second user asked for help with OnAir on Mint. Neither gave details.

## Which Mint is the newest

- **Linux Mint 22.3 "Zena"** is the newest stable release. It is based on Ubuntu 24.04 (noble). packages.linuxmint.com lists `zena` as its newest suite.
- The next release is planned for December 2026 and is based on Ubuntu 26.04 (resolute). Docker Hub has it as `linuxmintd/mint27-amd64` ("adrien"). The `linuxmintd/mint23-amd64` image points at the `alfa` suite, which is no longer on packages.linuxmint.com, so `apt-get update` fails in it.
- LMDE 7 "Gigi" is based on Debian 13.

## What was tested

`.github/workflows/linux-distros.yml` is a new manual workflow. It downloads a published release, installs the `.deb` the way the README says (`apt install ./onair_<v>_amd64.deb`), extracts and runs the AppImage, and unpacks the portable archive. For each of the three it checks `ldd` for missing libraries and starts OnAir under xvfb (`--hidden --mute --no-first-run --shot`). A start passes when the screenshot is larger than 10 kB.

Results for v3.9.0 (amd64), GitHub run 43265138175:

| Image | Base | glibc / GLIBCXX | .deb | AppImage | portable |
|---|---|---|---|---|---|
| linuxmintd/mint27-amd64 | Ubuntu 26.04 | 2.43 / 3.4.35 | starts | starts | starts |
| linuxmintd/mint22.3-amd64 | Ubuntu 24.04 | 2.39 / 3.4.33 | starts | starts | starts |
| linuxmintd/lmde7-amd64 | Debian 13 | 2.41 / 3.4.33 | starts | starts | starts |
| ubuntu:26.04 | | 2.43 / 3.4.35 | starts | starts | starts |
| ubuntu:24.04 | | 2.39 / 3.4.33 | starts | starts | starts |
| ubuntu:22.04 | | 2.35 / 3.4.30 | starts | starts | starts |
| debian:13 | | 2.41 / 3.4.33 | starts | starts | starts |

- `ldd` finds every library in all three packages on every image. Every `.deb` dependency resolves; on Mint 22.3, zenity comes from Mint's own repository.
- AppImage: the 3.9.0 AppImage uses the static type-2 runtime, so it does not need libfuse2. It needs `fusermount3` (package fuse3), which is installed on Mint 22.3 and on the Mint 27 image. In the privileged container it mounted with FUSE and ran.
- The messages about the radio libraries (`libsdrplay_api.so` and others not found), ALSA, JACK and Pulse are normal in a container without sound or radios.

So the packages install and start on Mint. The problem did not reproduce in the containers. A container has no real Mint desktop: no Cinnamon X11 session, no GPU driver, Ubuntu's `/etc/os-release`, and none of Mint's tools.

## The fix: a start error is shown, not hidden

On Linux every fatal start error went only to stderr. These include no OpenGL 3.0 context, the window not opening, and the graphics back end failing. Started from the Mint menu, stderr goes nowhere the user sees, so OnAir seemed to do nothing. On Ubuntu, the same machine with a different GPU path or session (GNOME on Wayland and XWayland, compared with Cinnamon on X11) can work. That matches the report.

`plat::showFatalError` (app/platform/platform_posix.cpp, Linux) now also:
- writes the message to `~/.config/onair/startup-error.txt` (or `$XDG_CONFIG_HOME/onair/`),
- shows it in a dialog when a desktop is present and stderr is not a terminal. It tries zenity, then kdialog, then notify-send, then xmessage. Each dialog closes by itself after 2 minutes, so an unattended start still ends. `ONAIR_NO_DIALOG=1` turns the dialog off.

Packaging is unchanged.

## Installing on Linux Mint

1. Download `onair_<version>_amd64.deb` (or `_arm64`) from the latest release.
2. In a terminal in the download folder: `sudo apt install ./onair_<version>_amd64.deb`. Double-clicking the `.deb` also opens Mint's package installer.
3. Start OnAir from the menu (Sound & Video) or with `onair`.

AppImage instead: right-click it, then Properties > Permissions > *Allow executing file as program*, then double-click it. If it says FUSE is missing, run `sudo apt install fuse3`, or start it with `--appimage-extract-and-run`.

### If nothing opens

- Run `onair` in a terminal (or `./OnAir-<v>-linux-x86_64.AppImage`) and read the last lines. `cat ~/.config/onair/startup-error.txt` shows the last start error.
- "no OpenGL 3.0 context" or "could not open its window": the graphics driver offers too old an OpenGL. Check with `glxinfo -B` (package mesa-utils). In a virtual machine, turn on 3D acceleration. With NVIDIA, use the driver from Driver Manager. Older Mesa: `MESA_GL_VERSION_OVERRIDE=3.3 MESA_GLSL_VERSION_OVERRIDE=330 onair`.
- A radio is not found: replug it after the install (the `.deb` installs the udev rules), or run `sudo ./install-udev-rules.sh` from the portable folder.

## Still open

- There is no log from an affected user. If the dialog shows a reason other than OpenGL, that is the next lead.
- Only amd64 was tested in the containers. The arm64 packages are built the same way.
