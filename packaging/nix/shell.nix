# Development shell: `nix develop`, then
#   cmake -S . -B build -G Ninja -DDECT2_BUILD_APP=ON && cmake --build build
{ pkgs, onair }:
let
  inherit (pkgs) lib;
in
pkgs.mkShell {
  inputsFrom = [ onair ]; # every build input of the package (compiler, cmake, pkg-config, cargo, rustc, hackrf, libusb, ffmpeg, glfw, soapysdr, hamlib, openssl)
  packages =
    (with pkgs; [
      ninja
      gdb
      git
      python3 # the CJK font embed step of the app and the helper scripts in tools/
      ccache
      rtl-sdr
      alsa-lib
      libpulseaudio
      zenity
    ])
    ++ lib.optionals (pkgs ? clippy) [ pkgs.clippy pkgs.rustfmt pkgs.rust-analyzer ] # the Rust workspace rust/ (cargo and rustc come from the package inputs)
    ++ lib.optional (pkgs ? airspy) pkgs.airspy
    ++ lib.optional (pkgs ? airspyhf) pkgs.airspyhf
    ++ lib.optional (pkgs ? libbladeRF) pkgs.libbladeRF
    ++ lib.optional (pkgs ? libiio) pkgs.libiio;
  # libraries OnAir opens with dlopen when it runs from the build folder
  LD_LIBRARY_PATH = lib.makeLibraryPath (
    (with pkgs; [ hackrf libusb1 rtl-sdr soapysdr hamlib openssl vulkan-loader alsa-lib libpulseaudio libGL ])
    ++ lib.optional (pkgs ? airspy) pkgs.airspy
    ++ lib.optional (pkgs ? airspyhf) pkgs.airspyhf
    ++ lib.optional (pkgs ? libbladeRF) pkgs.libbladeRF
    ++ lib.optional (pkgs ? libiio) pkgs.libiio
  );
}
