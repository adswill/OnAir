// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Help > Hardware: FFmpeg lookups by name (dect2/tv/media_probe.h). Only asks: nothing is opened.
#include "dect2/tv/media_probe.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace dect2::media {

bool ffmpegHas(FfKind kind, const char* name) {
    if (!name || !*name) return false;
    switch (kind) {
    case FfKind::Encoder: return avcodec_find_encoder_by_name(name) != nullptr;
    case FfKind::Decoder: return avcodec_find_decoder_by_name(name) != nullptr;
    case FfKind::Muxer: return av_guess_format(name, nullptr, nullptr) != nullptr;
    case FfKind::Demuxer: return av_find_input_format(name) != nullptr;
    }
    return false;
}

std::string ffmpegVersion() {
    const unsigned v = avcodec_version();
    return std::string(av_version_info()) + " (libavcodec " + std::to_string(AV_VERSION_MAJOR(v)) + "." + std::to_string(AV_VERSION_MINOR(v)) + "." +
           std::to_string(AV_VERSION_MICRO(v)) + ")";
}

} // namespace dect2::media
