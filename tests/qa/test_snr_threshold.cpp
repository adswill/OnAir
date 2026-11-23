// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SNR threshold table (4.6 step 1: measure). One row per decoding mode that has a synthetic generator: the lowest SNR at which it still decodes.
//
// SNR definition: signal power (mean over the active 1 ms blocks, those above a tenth of the strongest) divided by the power of white
// Gaussian noise inside the signal's own 99 % band (the generator's measured occupied bandwidth), the same as the QA sweep's "snr" file.
// Noise is added at that level over the whole sample rate (the noise density follows from the band), so the figure is the SNR in the
// channel bandwidth, not over the sample rate and not per bit (no Eb/N0).
// Decoded: the receiver's own checked count (blocksOk / saved results / lock for lock-only modes) is at least half the clean run's count in
// every trial (2 trials, fixed seeds: QA_SEED = trial number). Threshold: the lowest SNR reached from +20 dB downwards (4 dB steps, then 2 dB
// near the edge, floor -24 dB) without a failing point. Empty threshold: no decode up to +44 dB (see notes).
//
// This file only drives: it runs tools/sens/qa_sens.py (the generator and Engine stages are test_qa_sweep --gen / --run) and
// tools/sens/snr_threshold_table.py (CSV + table). Not part of the fast CI: label "snr" only, not "slow" so the nightly -L slow skips it. Run through quick.yml, see scratchpad/SNR-BASELINE-4.6.md.
//   env: SNR_OUT dir (default snr-out), SNR_RE mode-id regex, SNR_LIMIT first N modes (0 = all), SNR_JOBS (default 3), SNR_CAP seconds per mode (default 240), SNR_NOISE_ONLY=1 (only the noise-only false-decode check: modes counting decodes on noise are flagged FALSE_DECODE and their threshold is not trusted)
//   usage: test_snr_threshold <path of test_qa_sweep> <tools/sens dir>
#include <cstdio>
#include <cstdlib>
#include <string>

static std::string env(const char* k, const char* d) { const char* v = getenv(k); return v && *v ? v : d; }

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: test_snr_threshold <test_qa_sweep> <tools/sens dir>\n"); return 2; }
    const std::string bin = argv[1], tools = argv[2], out = env("SNR_OUT", "snr-out");
    const std::string cmd = "python3 \"" + tools + "/qa_sens.py\" --bin \"" + bin + "\" --out \"" + out + "\" --re \"" + env("SNR_RE", ".*") +
                            "\" --limit " + env("SNR_LIMIT", "0") + " --jobs " + env("SNR_JOBS", "3") + " --trials 2 --secs 8 --hi 20 --lo -20" +
                            " --mode-cap " + env("SNR_CAP", "240") + " --budget-min 300" + (getenv("SNR_NOISE_ONLY") ? " --noise-only" : "");
    printf("%s\n", cmd.c_str());
    fflush(stdout);
    if (system(cmd.c_str()) != 0) { fprintf(stderr, "qa_sens.py failed\n"); return 1; }
    const std::string tab = "python3 \"" + tools + "/snr_threshold_table.py\" \"" + out + "\"";
    return system(tab.c_str()) == 0 ? 0 : 1;
}
