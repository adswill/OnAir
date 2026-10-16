# OnAir in a browser: the whole desktop GUI runs on a virtual display inside the container and Selkies streams picture and
# sound to any browser (https://<host>:3001). For servers and NAS boxes without a screen. See README.md here.
#   docker build -t onair packaging/docker                                  (the latest release that this file names)
#   docker build -t onair --build-arg ONAIR_VERSION=0.2.6 packaging/docker  (another release)

# ---------- full: the Selkies desktop with OnAir installed, minus what no OnAir user needs ----------
FROM ghcr.io/linuxserver/baseimage-selkies:ubuntunoble AS full

ARG ONAIR_VERSION=0.2.5
ARG TARGETARCH

# The release .deb: it carries its own FFmpeg and the libraries of the radios OnAir drives itself (RTL-SDR: osmocom 2.0.3, in
# /usr/lib/onair); the SoapySDR modules add the rest. The distribution's librtlsdr0 and the SoapySDR rtlsdr module are purged below, so
# that nothing in the image can load a distribution librtlsdr.so.0 (not the verified 2.0.3) in place of the bundled one.
# libfaad2: the SBR and parametric stereo of DAB+ HE-AAC stations (found at run time). mbelib and codec2 come with OnAir.
# fonts-wqy-microhei: Chinese for the UI (one of the fonts OnAir looks for) in 5 MB, instead of the 90 MB of Noto CJK.
RUN set -eux; \
    apt-get update; \
    curl -fsSL -o /tmp/onair.deb \
      "https://github.com/adswill/OnAir/releases/download/v${ONAIR_VERSION}/onair_${ONAIR_VERSION}_${TARGETARCH:-$(dpkg --print-architecture)}.deb"; \
    apt-get install -y --no-install-recommends /tmp/onair.deb zenity soapysdr-module-all libfaad2 fonts-dejavu-core fonts-wqy-microhei; \
    # The stock librtlsdr0 and the SoapySDR rtlsdr module (the metapackage soapysdr-module-all goes with it, the other modules stay),
    # Docker Engine (the base image can run Docker inside), the compiler tool chain, every locale at once (the one LANG names is
    # made at the start instead, init-onair) and Noto CJK. Matched by name so that the same list works on x86-64 and arm64.
    dpkg-query -W -f='${Package}\n' | grep -E '^(docker-ce(-cli)?|docker-(buildx|compose)-plugin|containerd\.io|librtlsdr0|soapysdr0\.8-module-rtlsdr|iptables|nftables|cmake(-data)?|git(-man)?|locales-all|fonts-noto-cjk|(gcc|g\+\+)(-[0-9]+)?(-[a-z0-9_-]+-linux-gnu)?|lib(stdc\+\+|gcc)-[0-9]+-dev|libc6-dev|libc-dev-bin|linux-libc-dev|lib(asan|tsan|lsan|ubsan|hwasan)[0-9]+|binutils(-common|-[a-z0-9_-]+-linux-gnu)?|libbinutils|libgprofng0|libctf0|libctf-nobfd0|libsframe1)$' | xargs -r apt-get purge -y; \
    locale-gen en_US.UTF-8; \
    rm -rf /tmp/* /var/lib/apt/lists/* /var/cache/apt/* /var/log/apt/* /var/log/dpkg.log /usr/share/doc/* /usr/share/man/* /usr/share/info/*

COPY root/ /

# ---------- final: one layer, so that what was removed above is not still carried in the base image's layers ----------
FROM scratch
COPY --from=full / /

LABEL org.opencontainers.image.title="OnAir" \
      org.opencontainers.image.description="OnAir software-defined radio receiver, its full GUI in the browser" \
      org.opencontainers.image.source="https://github.com/adswill/OnAir" \
      org.opencontainers.image.licenses="GPL-3.0-or-later"

# The settings of the base image (docker image inspect ghcr.io/linuxserver/baseimage-selkies:ubuntunoble): a single-layer image does
# not inherit them, so check this list when the base changes. Changed here: START_DOCKER, TITLE. Added: the last four.
ENV PATH=/lsiopy/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    HOME=/config \
    LANGUAGE=en_US.UTF-8 \
    LANG=en_US.UTF-8 \
    TERM=xterm \
    S6_CMD_WAIT_FOR_SERVICES_MAXTIME=0 \
    S6_VERBOSITY=1 \
    S6_STAGE2_HOOK=/docker-mods \
    VIRTUAL_ENV=/lsiopy \
    DISPLAY=:1 \
    PERL5LIB=/usr/local/bin \
    START_DOCKER=false \
    PULSE_RUNTIME_PATH=/defaults \
    SELKIES_INTERPOSER=/usr/lib/selkies_joystick_interposer.so \
    NVIDIA_DRIVER_CAPABILITIES=all \
    DISABLE_ZINK=false \
    DISABLE_DRI3=false \
    SELKIES_ENCODER=x264enc,jpeg \
    TITLE=OnAir \
    SELKIES_UI_TITLE=OnAir \
    MAX_RES=3840x2160 \
    ONAIR_NO_UPDATE_CHECK=1

EXPOSE 3000 3001
VOLUME /config
# s6-overlay starts as root on purpose: its init steps need it (init-onair: chgrp of the radios, locale-gen), and the base image
# starts every long-running service as the user abc (s6-setuidgid abc): Xvfb, the desktop and OnAir. A USER line would stop s6.
# nosemgrep: dockerfile.security.missing-user-entrypoint.missing-user-entrypoint
ENTRYPOINT ["/init"]
