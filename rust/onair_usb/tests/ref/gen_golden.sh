#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Regenerates rust/onair_usb/tests/golden/*.log from the ORIGINAL librtlsdr (RTL-SDR Blog fork 1.3.6) over the fake libusb of ref_harness.c.
#   RTLSDR_SRC=/path/to/rtl-sdr-blog-1.3.6 LIBUSB_INC=/opt/homebrew/include/libusb-1.0 rust/onair_usb/tests/ref/gen_golden.sh
# Needs only a C compiler and the libusb header; nothing is linked against libusb and no device is touched. The golden files are committed, so CI and
# `cargo test` do not need the C source; set ONAIR_USB_REF_BIN to a built harness to also re-run the C side from the tests.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
: "${RTLSDR_SRC:?set RTLSDR_SRC to the rtl-sdr-blog 1.3.6 source tree}"
: "${LIBUSB_INC:=/opt/homebrew/include/libusb-1.0}"
bin=${TMPDIR:-/tmp}/onair_usb_ref_harness
cc -O1 -w -I"$RTLSDR_SRC/include" -I"$LIBUSB_INC" -o "$bin" "$here/ref_harness.c" \
  "$RTLSDR_SRC/src/librtlsdr.c" "$RTLSDR_SRC"/src/tuner_*.c
out="$here/../golden"
mkdir -p "$out"
grep -v '^#' "$out/scenarios.txt" | grep . | while IFS='|' read -r name model cmds; do
  # shellcheck disable=SC2086
  "$bin" "$model" $cmds > "$out/$name.log" 2>/dev/null
done
echo "golden logs written to $out"
