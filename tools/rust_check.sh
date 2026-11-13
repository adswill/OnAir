#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

# Checks for the Rust workspace in rust/: fmt (if rustfmt exists), clippy -D warnings (if clippy exists), cargo test --offline.
#   tools/rust_check.sh            fmt + clippy + tests
#   tools/rust_check.sh --sanitize also a sanitizer build of the C++ side with the Rust library linked and the AIS differential/fuzz tests
#                                  (needs cmake, ninja and a compiler with ASan/UBSan; build dir: ${TMPDIR:-/tmp}/onair-rust-san)
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
python3 "$root/tools/check_ffi_widths.py" "$root/rust"
cd "$root/rust"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$root/rust/target}"
if ! command -v cargo >/dev/null 2>&1; then echo "rust_check: cargo not found"; exit 1; fi

if cargo fmt --version >/dev/null 2>&1; then
  echo "== cargo fmt --check"; cargo fmt --all -- --check
else echo "== rustfmt not installed: skipped"; fi

if cargo clippy --version >/dev/null 2>&1; then
  echo "== cargo clippy"; cargo clippy --offline --workspace --all-targets -- -D warnings
else echo "== clippy not installed: skipped"; fi

echo "== cargo test"; cargo test --offline --workspace
echo "== cargo build --release -p onair --features bare (the archive CMake links)"
cargo build --offline --release -p onair --features bare

if [ "${1:-}" = "--sanitize" ]; then
  b="${TMPDIR:-/tmp}/onair-rust-san"
  echo "== sanitizer build in $b"
  cmake -S "$root" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDECT2_SANITIZE=ON
  cmake --build "$b" --target test_ais_proto_rs fuzz_ais_proto
  (cd "$b" && ctest -R 'ais|rust' --output-on-failure)
fi
echo "rust_check: ok"
