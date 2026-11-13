// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pictures for KAIT transmit: load a PNG or a binary PPM (P6) file, shrink to fit 1920x1080, write a PPM. Plain C++, no library.
// Other formats (JPEG, ...) are converted to PPM by the app, which has the system's picture decoder (app/kait/kait_ui.cpp).
#pragma once
#include "dect2/kait/kait_codec.h"
#include <string>

namespace dect2::kait {

bool loadPicture(const std::string& path, Image& out, std::string& err);   // PNG (any colour type) or PPM P6 with maxval 255; alpha is dropped
Image fitPicture(const Image& in, int maxW = 1920, int maxH = 1080);        // box-filter shrink keeping the aspect ratio; unchanged when it fits
bool writePpm(const std::string& path, const Image& img);

} // namespace dect2::kait
