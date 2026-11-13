# Install

Short version: [README, Download and install](../README.md#download-and-install). The details are here.

Download the file for your system from the **[latest release](https://github.com/adswill/OnAir/releases/latest)**. There is nothing to compile.

| System | Download | How to install |
|---|---|---|
| **macOS** 15 or later | `OnAir-…-macos-arm64.dmg` (Apple silicon)<br>`OnAir-…-macos-x86_64.dmg` (Intel) | Open the `.dmg` and drag OnAir to Applications. The app is not signed with a developer certificate yet, so on the first launch right-click it and choose *Open*. Intel Macs need a CPU with AVX2 (2013 or newer). |
| **Windows** 10 / 11, 64-bit | `OnAir-…-windows-x64-setup.exe`<br>or `OnAir-…-windows-x64.zip` | Run the installer, or unpack the zip and start `OnAir.exe`. Windows may warn about an unknown publisher because the installer is not code-signed. Needs a CPU with AVX2 (Intel or AMD from about 2013 on). |
| **Linux** x86-64 and arm64 | `onair_…_amd64.deb` / `onair_…_arm64.deb`<br>`OnAir-…-linux-<arch>.AppImage`<br>or `onair-…-portable.tar.gz` | Debian / Ubuntu / Linux Mint / LMDE: `sudo apt install ./onair_<version>_<arch>.deb`, then start OnAir from the menu or with `onair`. If nothing opens, run `onair` in a terminal to see why (the reason is also kept in `~/.config/onair/startup-error.txt`); Linux Mint steps: [docs/LINUX-MINT-4.0.md](LINUX-MINT-4.0.md). The `.deb` carries its own video decoder, so it does not depend on your system's FFmpeg. Other distributions: make the AppImage executable and run it (it updates itself in place, no root), or unpack the portable archive and run `bin/onair`. |
| **Android** 9+, arm64 | `OnAir-…-android.apk` | Sideload the APK, see [Android app](INSTALL.md#android-app). |
| **Docker** (a server without a screen, used from a browser) | `packaging/docker` in this repository | create a `.env` file with `ONAIR_PASSWORD=<a password>` in that folder, run `docker compose up -d --build`, then open `https://<server>:3001`. See [packaging/docker/README.md](../packaging/docker/README.md) for radios, graphics chips and settings. |

> [!NOTE]
> **SDRplay RSPs (RSP1, RSP1A, RSP1B, RSP2, RSPduo, RSPdx):** install the free **[SDRplay Hardware API](https://sdrplay.com/hardware-api/)** once, separately from SDRconnect, and restart OnAir. OnAir loads SDRplay's own library and service; it does not bundle them. Make sure the "SDRplay API Service" is running (the installer starts it). For the RSPduo, select the input with the antenna: **Tuner 1**, **Tuner 2**, or **Tuner 1 Hi-Z**. OnAir uses one tuner at a time, at 62.5 kS/s to 10 MS/s; see [tested features and limits](../DEVICES.md#sdrplay-rspduo). An RSP-compatible radio not made by SDRplay (for example a Mirics MSi2500 "RSP1A clone") is not seen by the SDRplay API; use SoapySDR's `mirisdr` module instead.

> [!IMPORTANT]
> **Windows USB drivers:** for a HackRF, RTL-SDR, Airspy, Airspy HF+, Fobos SDR or RX888 MkII (its boot loader 04b4:00f3 and its running firmware 04b4:00f1 both need it), install the WinUSB driver once with [Zadig](https://zadig.akeo.ie). A LimeSDR Mini needs FTDI's FT60x driver (see the README inside the download). A LimeSDR-USB (Cypress FX3) needs the Cypress/LimeSuite USB driver (CyUSB3, the one LimeGUI installs), not WinUSB: do not replace it with Zadig. OnAir's own LimeSuite has no FX3 support, so for a LimeSDR-USB also install [PothosSDR](https://downloads.myriadrf.org/builds/PothosSDR/) or LimeSuite; OnAir finds its LimeSuite.dll by itself and the log (onair.log) says which copy saw the radio ("LimeSuite ...: N radio(s)").

### Nix / NixOS

The repository is a flake (x86_64-linux and aarch64-linux). Details, the module options and what is verified: [docs/NIXOS.md](NIXOS.md).

```
nix run github:adswill/OnAir                    # the GUI
nix run github:adswill/OnAir#onair-server       # the headless server
```

As a flake input with the NixOS module (udev rules, the TV-driver blacklist, an optional server service):

```nix
{
  inputs.onair.url = "github:adswill/OnAir";
  inputs.onair.inputs.nixpkgs.follows = "nixpkgs";
  outputs = { nixpkgs, onair, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        onair.nixosModules.default
        {
          programs.onair.enable = true;
          programs.onair.users = [ "alice" ];
          # services.onair-server = { enable = true; web.enable = true; openFirewall = true; };
        }
      ];
    };
  };
}
```

Voice codecs are never part of the package: put them in `~/.local/share/onair/codecs/` ([docs/CODECS.md](CODECS.md)).

### Android app

<details>
<summary><b>The Android app: sources, limits, how to install</b></summary>

OnAir for Android is the desktop decoder core (the same C++ code, built as `libonair.so`) with a phone interface in Kotlin Compose: spectrum and waterfall, the mode list with search and groups, a results tab with the decoded data of the selected mode, and a log.

- **Receive only.** The app never transmits. Network access (for rtl_tcp, SpyServer, map tiles and the in-app update) is its only data link: nothing is sent to any server. Other permissions are for the receive service and the sound (foreground service, wake lock, notifications), the sound-card source (microphone, only while that source runs), a walk-around direction finder (location, only while a walk runs) and installing the in-app update (you confirm it).
- **Sources:** an RTL-SDR on the USB port (0.2.7), `rtl_tcp` (an RTL-SDR served over the network, or on the phone itself), SpyServer, an IQ file (cs8, cu8, cf32, ci16, cu16, with its sample rate) and a built-in test signal for the selected mode that needs no radio.
- **Results:** per-mode results (cards and tables), an OpenStreetMap map (tiles are only fetched after you agree), and FM with RDS.
- **Modes:** the mode list has every row of the desktop list (388 in 4.0; the 3.7 audit found 3 that cannot be used on a phone). Results cards and tables are generated for 107 more modes than before. This was checked on an Android 15 emulator against the built-in test signal, not on a phone or with a real radio ([docs/ANDROID-3.7.md](ANDROID-3.7.md)).
- **Use it:** pick a source on the Receiver tab, a mode on the Mode tab, press play. Tap the spectrum to tune, drag to move, pinch to zoom (double tap resets).
- **Install:** download `OnAir-…-android.apk` from the [release](https://github.com/adswill/OnAir/releases/latest), copy it to the phone, open it and allow installs from unknown sources for the app you opened it with. The APK is signed with the project's own key, not a Play Store key; updating later needs the same key.
- **Limits:** arm64-v8a only, Android 9 (API 28) or newer. Direct USB works for RTL-SDR radios (new in 0.2.7); HackRF and Airspy USB are not supported yet, so use rtl_tcp or SpyServer for those. No vocoder voice (DMR, P25, NXDN, D-STAR, TETRA), no TV video or sound, and no images yet (the satellite pictures and the SSTV / fax pictures). Wide modes (DVB, ATSC, ISDB, DTMB) will usually not keep up on a typical phone. No channel scan and no import screen for voice codec plug-ins.
</details>

### Development version ("nightly")

Fixes and new features land on the `main` branch first, before they go into a release. To try them early, [build OnAir from source](BUILD.md) from the latest `main`.

> [!WARNING]
> The development version is **not tested like a release**. It can contain bugs, unfinished features or changes that stop parts of the app (or the whole app) from working. Use the [latest release](https://github.com/adswill/OnAir/releases/latest) for everyday use, and if something breaks in a development build, please say so on [Discord](https://discord.gg/ptxkzCuzR) or in [GitHub Issues](https://github.com/adswill/OnAir/issues), mentioning the commit you built.
