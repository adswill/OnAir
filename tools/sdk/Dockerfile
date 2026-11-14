# SPDX-License-Identifier: GPL-3.0-or-later
#
# The machine the SDKs are made on, for build_all_releases.sh --docker: Ubuntu 26.04, pinned by digest, with what
# tools/sdk needs (check_host.sh). Built without a context: the OnAir checkout is mounted read-only at run time.
FROM ubuntu:26.04@sha256:6a06111c3b0a8600feadb270aec771dc239bf407b25625f93f44ab00e11f4d3c

ARG UID=1000
ARG GID=1000

# The package lists stay in the image: the SDK's scripts use apt-get download and apt-cache for pinned packages that are not in ./cache yet.
#   git                                OnAir's version, from its checkout
#   cmake ninja-build pkgconf          the builds
#   curl ca-certificates               downloads
#   zstd xz-utils bzip2 unzip pigz     unpacking, and the tarballs
#   gpg gpg-agent gpgv                 signature checks of the downloads
#   python3                            the dependency resolver and the checks
#   make perl nasm patch               the libraries' configure scripts, FFmpeg
#   libwayland-bin                     wayland-scanner, for GLFW's Wayland support (Linux targets)
#   gcc g++ libc6-dev zlib1g-dev liblzma-dev   the packaging tools libdmg-hfsplus and rcodesign, which run on the build machine
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      git \
      cmake ninja-build pkgconf \
      curl ca-certificates \
      zstd xz-utils bzip2 unzip pigz \
      gpg gpg-agent gpgv \
      python3 \
      make perl nasm patch \
      libwayland-bin \
      gcc g++ libc6-dev zlib1g-dev liblzma-dev

# Not root, with the user's own ids so the files in the mounted folders stay theirs.
RUN groupadd -o -g "$GID" sdkbuild && useradd -o -m -u "$UID" -g "$GID" sdkbuild
USER sdkbuild
ENV HOME=/home/sdkbuild USER=sdkbuild LOGNAME=sdkbuild
WORKDIR /work
