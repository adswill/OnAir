// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Help > Hardware: is a codec or container in the FFmpeg OnAir was built with (app/panels/hardware_logic.h lists the names).
#pragma once
#include <string>

namespace dect2::media {

enum class FfKind { Encoder, Decoder, Muxer, Demuxer };

bool ffmpegHas(FfKind kind, const char* name);   // by the FFmpeg name ("libx264", "hevc", "mpegts")
std::string ffmpegVersion();                     // "7.1.2 (libavcodec 61.19.101)"

} // namespace dect2::media
