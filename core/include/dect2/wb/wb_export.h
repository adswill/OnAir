// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench: writing what was found: a selection of the recording (a time range, optionally one band moved to baseband) as .wav, .cf32
// or SigMF, demodulated audio as .wav, the peak table as CSV.
#pragma once
#include "wb_source.h"
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace wb {

enum ExportFormat { ExWav = 0, ExCf32, ExSigmf };
const char* exportExt(int fmt);

// [t0, t1) of the source; with f1 > f0 only that band, moved to baseband and decimated (the file's rate is then lower); without: the samples as they are.
// A real source without a band gives a mono WAV (or a real-valued cf32 / SigMF); everything else two-channel float WAV / cf32 / SigMF.
bool exportSelection(const Source& s, uint64_t t0, uint64_t t1, double f0, double f1, const std::string& path, int fmt, std::string& err,
                     const std::function<bool(double)>& progress = nullptr);

bool writeWavMono16(const std::string& path, const std::vector<float>& a, int rate, std::string& err);
bool writeTextFile(const std::string& path, const std::string& text, std::string& err);
// "name.ext" with the extension replaced / added
std::string withExt(const std::string& path, const std::string& ext);

} // namespace wb
} // namespace dect2
