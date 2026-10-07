#!/bin/sh
# Runs the ATSC 3.0 test signal (core/src/atsc3_synth.cpp) through the real receiver over the whole matrix: output rates 6.144, 8, 10 and 20 Msps, SNR 25
# and 15 dB, carrier offset +-3 kHz, clock offset +-20 ppm, generator chunks of 1, 7, 4096 and 65536 samples. Prints one line per case.
#   tools/dev/atsc3_synth_matrix.sh [seconds per case, default 20]       (needs the normal build in build/, or BUILD=dir)
# One case by hand: build/test_atsc3_synth_rx --one rate_msps snr_db cfo_hz clock_ppm chunk seconds [modeOpt0 .. modeOpt5]
set -e
BUILD=${BUILD:-build}
"$BUILD/test_atsc3_synth_rx" --full "${1:-20}"
