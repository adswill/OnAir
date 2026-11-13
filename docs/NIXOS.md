# OnAir on NixOS and Nix

> **Status.** There is no Nix on the maintainer's machine. The `nix` GitHub workflow (`.github/workflows/nix.yml`, started by hand from the Actions tab; part of the release checklist) runs `nix flake check` (builds both packages and evaluates a NixOS configuration with the module) and `nix build .#onair .#onair-server` on x86_64-linux and aarch64-linux, then checks the installed files. Only what that run covers is verified. Running the program with a real radio on NixOS is not. macOS is not a flake system (the package is Linux-only).

The repository has a `flake.nix` (packages `onair` and `onair-server`, a NixOS module, a dev shell). The files are in `packaging/nix/`.

## Try it

```
nix run github:adswill/OnAir              # the GUI
nix run github:adswill/OnAir#onair-server # the headless server (docs/headless-server.md)
```

This does not give your user access to the USB radio: for that use the NixOS module below, which installs the udev rules (`packaging/linux/install-udev-rules.sh` writes into `/etc` and does not suit NixOS).

## Overlay

```nix
nixpkgs.overlays = [ onair.overlays.default ];   # then pkgs.onair and pkgs.onair-server exist
```

## What the flake provides

- `packages.<system>.onair` (the GUI `onair`, plus `onair-server`, `onair-cli`, `onair-scan`, the web pages and the Wireshark extcap helper, like the .deb) and `.onair-server` (the server and the web pages only), `checks.<system>` (the two packages and `module`, an evaluated NixOS configuration, for `nix flake check`), `overlays.default`, `nixosModules.default`, `devShells.default`.
- The package version is read from `project(OnAir VERSION ...)` in `CMakeLists.txt`.
- Radio libraries that a nixpkgs revision lacks (`airspy`, `airspyhf`, `libbladeRF`, `libiio`) are skipped instead of failing the evaluation.
- The module's `package` option falls back to building the package itself when the flake has no package for the host's system.
- With no system monospaced font (NixOS has none under `/usr/share/fonts`) the Classic look uses the embedded JetBrains Mono.

## NixOS module

```nix
# flake.nix
{
  inputs.onair.url = "github:adswill/OnAir";
  inputs.onair.inputs.nixpkgs.follows = "nixpkgs";   # optional

  outputs = { nixpkgs, onair, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        onair.nixosModules.default
        {
          programs.onair.enable = true;
          programs.onair.users = [ "alice" ];        # added to the radio group
          # optional headless server:
          # services.onair-server.enable = true;
          # services.onair-server.extraArgs = [ "--lan" ];
          # services.onair-server.openFirewall = true;
        }
      ];
    };
  };
}
```

What the module does:

- installs `onair` in the system profile;
- `services.udev.packages`: `60-onair-sdr.rules` (HackRF, RTL-SDR, Airspy, bladeRF, LimeSDR, PlutoSDR, USRP, SDRplay ...). The rules use `GROUP="plugdev"` with mode 0660 and also `TAG+="uaccess"`, so the user at the desk gets access without any group. `programs.onair.users` adds users (for example over ssh) to the group `programs.onair.group` (default `plugdev`; the module creates it);
- `boot.blacklistedKernelModules`: the kernel's DVB-T stick drivers (`dvb_usb_rtl28xxu`, `rtl2832`, ...) and the Mirics drivers, the same lists as `packaging/linux/onair-*-blacklist.conf` (turn off with `programs.onair.blacklistTvDrivers = false`; reboot or `modprobe -r` once);
- `services.onair-server`: a hardened systemd unit, a system user `onair`, settings in `/var/lib/onair/server.conf`. Options: `port` (`--port`), `lan` (`--lan`), `web.enable` / `web.port` / `web.lan` (`--web`, `--web-port`, `--web-lan`: OnAir Web), `openFirewall` (opens `port` and the web port), `extraArgs` (any other flag, for example `--mcp-stdio`). For access from other computers run `sudo -u onair onair-server --config /var/lib/onair/server.conf --lan --new-token` once.

More radios (SoapySDR modules) and libraries:

```nix
programs.onair.package = onair.packages.x86_64-linux.onair.override {
  soapyModules = with pkgs; [ soapyrtlsdr soapyhackrf soapyairspy soapyremote soapysdrplay3 ];
  extraRuntimeLibs = [ pkgs.libfdk_aac ];
};
```

## Libraries loaded at run time

OnAir opens several libraries with `dlopen` instead of linking them: `librtlsdr`, `libairspy`, `libairspyhf`, `libbladeRF`, `libiio`, `libsdrplay_api`, `libfobos_sdr`, `libasound`, `libfdk-aac` and others. NixOS has no `/usr/lib`, so the `onair` and `onair-server` programs are wrappers that set `LD_LIBRARY_PATH` to rtl-sdr, airspy, airspyhf, bladeRF, libiio, hackrf, libusb, SoapySDR, hamlib, ALSA, PulseAudio and libGL, and `SOAPY_SDR_PLUGIN_PATH` to the SoapySDR modules you passed. Add more with `extraRuntimeLibs`. `libsdrplay_api` (SDRplay) and `libfobos_sdr` are not in nixpkgs as free software: add your own package to `extraRuntimeLibs` if you have one. The dev shell (`nix develop`) sets the same path for a build-folder run.

## Codecs (mbelib and others)

OnAir never ships patented codecs (DMR, P25, NXDN, D-STAR, HD Radio ... , see `docs/CODECS.md`). Put the library files in `~/.local/share/onair/codecs/` (or `$XDG_DATA_HOME/onair/codecs/`); the **?** button next to the sound has an **Open codecs folder** button and **Look again**. A library built by Nix lives in the store: copy or link the `.so` file into that folder, or point the environment variable `DECT2_MBELIB_PATH` / `DECT2_CODEC2_PATH` at it. codec2 for FreeDV and M17 is built from `third_party/codec2` and installed in `lib/onair` of the package; nothing to do.

## Development shell

```
nix develop
cmake -S . -B build -G Ninja -DDECT2_BUILD_APP=ON
cmake --build build
```

The Rust workspace (`rust/`) is built by CMake with `cargo build --offline`; `rust/Cargo.lock` lists no external crates (checked for 4.1), so no vendoring (`cargoLock`) is needed. If a crate from crates.io is ever added, the package needs `cargoDeps` / `cargoSetupHook` and this paragraph changes.

## Not verified yet

- that the build passes in the Nix sandbox (cargo `--features bare` needs a recent enough stable `rustc`; the code asks for 1.70 or newer);
- the attribute names `soapyrtlsdr`, `soapyhackrf`, `soapyairspy`, `soapyremote` in the flake (missing ones are skipped);
- that GLFW finds X11 or Wayland and the audio output (miniaudio) finds ALSA/PulseAudio/PipeWire through the wrapper's library path;
- the plug-in examples folder path (`build/plugins`) and `libonair_codec2.so*` location in the build tree;
- librfnm (RFNM) is switched off: it is not in nixpkgs;
- the licence field (`gpl3Only`) follows `LICENSE`; check `THIRD_PARTY.md` for the bundled code;
- 

`.github/workflows/nix.yml` is started by hand only (`workflow_dispatch`): `gh workflow run nix.yml --ref <branch>`.
