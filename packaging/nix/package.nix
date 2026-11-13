# OnAir for Nix: the GUI (pname "onair") or the headless server (server = true, pname "onair-server").
# Verified only by the `nix` GitHub workflow (x86_64-linux and aarch64-linux); see docs/NIXOS.md.
# The GUI package also carries onair-server, onair-cli, onair-scan and the web pages, like the .deb; the server package has onair-server and the web pages.
{
  lib,
  stdenv,
  cmake,
  pkg-config,
  python3,
  cargo,
  rustc,
  makeWrapper,
  # build-time libraries (the CMake pkg_check_modules list)
  hackrf,
  libusb1,
  ffmpeg,
  glfw,
  libGL,
  soapysdr,
  hamlib,
  openssl, # https for Internet TV and the server's TLS (CMake turns DECT2_WITH_TLS on when OpenSSL is found)
  # libraries OnAir loads with dlopen at run time (core/src/source/source_*.cpp, docs/CODECS.md): the wrapper puts them on LD_LIBRARY_PATH
  rtl-sdr,
  vulkan-loader, # the Vulkan LDPC decoder opens libvulkan at run time (core/src/dsp/gpu_ldpc_vulkan.cpp); the CPU decoder runs without it
  # optional radio libraries: not every nixpkgs revision ships all of them (null = skipped)
  airspy ? null,
  airspyhf ? null,
  libbladeRF ? null,
  libiio ? null,
  alsa-lib,
  libpulseaudio,
  zenity,
  # SoapySDR modules (each one a package with lib/SoapySDR/modules*)
  soapyModules ? [ ],
  # more library packages for the wrapper's LD_LIBRARY_PATH, for example [ pkgs.libfdk_aac ]
  extraRuntimeLibs ? [ ],
  server ? false,
  src ? ../..,
}:

let
  # the version comes from CMakeLists.txt: project(OnAir VERSION ...)
  cmakeLines = lib.splitString "\n" (builtins.readFile (src + "/CMakeLists.txt"));
  version = lib.head (
    lib.findFirst (m: m != null) [ null ] (map (builtins.match "[[:space:]]*project\\(OnAir VERSION ([0-9.]+).*") cmakeLines)
  );
  optionalRuntimeLibs = lib.filter (x: x != null) [
    airspy
    airspyhf
    libbladeRF
    libiio
  ];
  runtimeLibs = [
    hackrf
    libusb1
    rtl-sdr
    soapysdr
    hamlib
    openssl
    vulkan-loader
    alsa-lib
    libpulseaudio
    libGL
  ] ++ optionalRuntimeLibs ++ extraRuntimeLibs;
  prog = if server then "onair-server" else "onair";
in
stdenv.mkDerivation {
  pname = prog;
  inherit version;
  inherit src;

  # Nix adds -Werror=format-security; the UI code has printf-style calls with non-literal formats (app/dtmb/dtmb_ui.cpp, app/dmr/dmr_ui.cpp, ...)
  hardeningDisable = [ "format" ];

  nativeBuildInputs = [
    cmake
    pkg-config
    cargo # the Rust workspace rust/ is mandatory (cmake/OnairRust.cmake): `cargo build --offline`; Cargo.lock has no external crates
    rustc
    makeWrapper
  ] ++ lib.optionals (!server) [ python3 ];

  buildInputs =
    [
      hackrf
      libusb1
      ffmpeg
      soapysdr
      hamlib
      openssl
    ]
    ++ lib.optionals (!server) [
      glfw
      libGL
    ];

  cmakeFlags = [
    (lib.cmakeBool "DECT2_BUILD_APP" (!server))
    (lib.cmakeBool "DECT2_BUILD_TESTS" (!server)) # the GUI package builds only the targets dect2, dect2cli and dect2scan (the tests are not compiled); dect2cli is defined in the tests block
    (lib.cmakeBool "DECT2_CCACHE" false)
    (lib.cmakeBool "DECT2_WITH_SOAPY" true)
    (lib.cmakeBool "DECT2_HAVE_HAMLIB" true)
    (lib.cmakeBool "DECT2_WITH_RFNM" false) # librfnm is not in nixpkgs
    (lib.cmakeBool "DECT2_WITH_TLS" true)
    (lib.cmakeBool "DECT2_BUNDLE_CODEC2" (!server)) # codec2 (FreeDV, M17) as a shared library built from third_party/codec2
    (lib.cmakeBool "DECT2_BUILD_PLUGIN_EXAMPLES" (!server))
    (lib.cmakeFeature "DECT2_UI_BACKEND" "OpenGL3")
  ];

  # cargo wants a writable home even when it works offline
  preConfigure = ''
    export CARGO_HOME=$TMPDIR/cargo
  '';

  # `cmake --install` also writes /etc/modprobe.d (an absolute path) which a Nix build cannot do: install by hand, same layout as the .deb.
  dontUseCmakeInstall = true;
  buildPhase = ''
    runHook preBuild
    cmake --build . --parallel $NIX_BUILD_CORES --target ${if server then "onair-server" else "dect2 dect2cli dect2scan"}
    runHook postBuild
  '';

  installPhase =
    ''
      runHook preInstall
      mkdir -p $out/bin $out/lib/udev/rules.d $out/share/doc/onair
      install -m644 ../packaging/linux/60-onair-sdr.rules $out/lib/udev/rules.d/60-onair-sdr.rules
      install -m644 ../LICENSE ../THIRD_PARTY.md $out/share/doc/onair/
      cp -r ../LICENSES $out/share/doc/onair/
      # the browser pages: onair-server looks in ../share/onair/web of its own folder
      mkdir -p $out/share/onair/web
      cp -r ../web/onair-web ../web/onair-app $out/share/onair/web/
    ''
    + (
      if server then
        ''
          install -m755 onair-server $out/bin/onair-server
        ''
      else
        ''
          install -m755 dect2 $out/bin/onair
          install -m755 onair-server $out/bin/onair-server
          install -m755 dect2cli $out/bin/onair-cli
          install -m755 dect2scan $out/bin/onair-scan
          mkdir -p $out/lib/onair
          install -m755 onair-plugin-host onair-extcap $out/lib/onair/   # the program looks in ../lib/onair of its own folder
          if [ -d plugins ]; then cp -r plugins $out/lib/onair/plugins; fi
          find . -maxdepth 2 -name 'libonair_codec2.so*' -exec cp -P {} $out/lib/onair/ \;
          install -Dm644 ../packaging/icons/onair.svg $out/share/icons/hicolor/scalable/apps/onair.svg
          install -Dm644 ../packaging/icons/onair_1024.png $out/share/icons/hicolor/1024x1024/apps/onair.png
          install -Dm644 ../packaging/linux/onair.desktop $out/share/applications/onair.desktop
        ''
    )
    + ''
      runHook postInstall
    '';

  # LD_LIBRARY_PATH for the radio, audio and OpenGL libraries OnAir opens with dlopen; the SoapySDR modules; zenity for file dialogs.
  # User-installed codecs (mbelib, ...) are not touched: they live in ~/.local/share/onair/codecs (docs/CODECS.md).
  postFixup = ''
    soapy=""
    for m in ${lib.concatStringsSep " " (map toString soapyModules)}; do
      for d in $m/lib/SoapySDR/modules*; do
        if [ -d "$d" ]; then soapy="$soapy''${soapy:+:}$d"; fi
      done
    done
    for p in ${lib.concatStringsSep " " (if server then [ "onair-server" ] else [ "onair" "onair-server" "onair-cli" "onair-scan" ])}; do
      wrapProgram $out/bin/$p \
        --prefix LD_LIBRARY_PATH : ${lib.makeLibraryPath runtimeLibs} \
        ${lib.optionalString (!server) "--suffix PATH : ${lib.makeBinPath [ zenity ]}"} \
        ''${soapy:+--prefix SOAPY_SDR_PLUGIN_PATH : "$soapy"}
    done
  '';

  meta = {
    description = "Software radio receiver for HackRF and other SDRs: DVB-T2, DVB-T, ATSC, FM and many more modes";
    homepage = "https://github.com/adswill/OnAir";
    license = lib.licenses.gpl3Only; # LICENSE is GPL-3.0; THIRD_PARTY.md lists the bundled code
    platforms = lib.platforms.linux;
    mainProgram = prog;
  };
}
