// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared by the voice-chain tests with the stub codec plug-in (tests/codec/stub_codec_plugin.c, no codec): copies the stub into a temporary codecs
// folder under the plug-in file name for an id, registers it in the codec registry and counts the frames the receiver hands to it.
#pragma once
#include "dect2/codec/codec_registry.h"
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace plugtest {

inline std::atomic<int> gFrames{0}, gBadSize{0};

class Counting final : public dect2::codec::SpeechCodec {
public:
    Counting(std::unique_ptr<dect2::codec::SpeechCodec> in, size_t size) : in_(std::move(in)), size_(size) {}
    int sampleRate() const override { return in_->sampleRate(); }
    std::vector<int16_t> decode(const std::vector<uint8_t>& coded) override {
        gFrames++;
        if (coded.size() != size_) gBadSize++;
        return in_->decode(coded);
    }
private:
    std::unique_ptr<dect2::codec::SpeechCodec> in_;
    size_t size_;
};

// no mbelib may answer instead of the plug-in; then the stub is registered under `id`
inline bool install(const std::string& id, const std::string& stubPath) {
#ifdef _WIN32
    _putenv_s("DECT2_MBELIB_PATH", "none");
#else
    setenv("DECT2_MBELIB_PATH", "none", 1);
#endif
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("onair-test-plugin-" + id);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    fs::copy_file(stubPath, dir / dect2::codec::speechCodecFileName(id), ec);
    if (ec) return false;
    const std::string folder = dir.string();
    const size_t size = dect2::codec::pluginFrameBytes(id);
    dect2::codec::setPluginFactory(id, [folder, id, size]() -> std::unique_ptr<dect2::codec::SpeechCodec> {
        std::unique_ptr<dect2::codec::SpeechCodec> c;
        dect2::codec::loadSpeechCodec(id, folder, true, c);
        return c ? std::make_unique<Counting>(std::move(c), size) : nullptr;
    }, folder + "/" + id);
    return true;
}

} // namespace plugtest
