// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The macOS app's Info.plist (written by tools/package/make_dmg.sh; packaging/macos/Info.plist for local bundles) has a usage text for every
// protected resource the code opens (#358). Without NSMicrophoneUsageDescription macOS never asks and a sound card input (an IC-7300's USB
// Audio CODEC) delivers silence; without NSCameraUsageDescription opening a camera ends the program; without the location texts the ADS-B
// antenna position is never asked for. The check follows the code: a marker found in a source file needs its key in both plists.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static std::string slurp(const std::string& rel) {
    std::ifstream f(std::string(ONAIR_SOURCE_DIR) + "/" + rel, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

int main() {
    const std::string dmg = slurp("tools/package/make_dmg.sh"), local = slurp("packaging/macos/Info.plist");
    CHECK(!dmg.empty() && !local.empty(), "both plist sources are readable");
    struct Need { const char* file; const char* marker; const char* key; } needs[] = {
        {"core/src/audio/audioin_coreaudio.mm", "AVMediaTypeAudio", "NSMicrophoneUsageDescription"},
        {"core/src/audio/videoin_avfoundation.mm", "AVCaptureSession", "NSCameraUsageDescription"},
        {"app/platform/platform_cocoa.mm", "CLLocationManager", "NSLocationWhenInUseUsageDescription"},
    };
    int used = 0;
    for (const Need& n : needs) {
        const std::string src = slurp(n.file);
        if (src.find(n.marker) == std::string::npos) continue;   // the code no longer opens it
        used++;
        const std::string key = std::string("<key>") + n.key + "</key><string>";
        CHECK(dmg.find(key) != std::string::npos, "make_dmg.sh: %s uses %s but the shipped Info.plist has no %s", n.file, n.marker, n.key);
        CHECK(local.find(key) != std::string::npos, "packaging/macos/Info.plist: %s uses %s but there is no %s", n.file, n.marker, n.key);
        // an empty text is rejected by macOS like a missing one
        const size_t at = dmg.find(key);
        if (at != std::string::npos) CHECK(dmg.compare(at + key.size(), 9, "</string>") != 0, "make_dmg.sh: %s is empty", n.key);
    }
    CHECK(used == 3, "the three protected resources are still found in the sources (%d)", used);
    // the microphone input asks macOS when it was never asked (a HAL unit alone does not), instead of delivering silence
    CHECK(slurp("core/src/audio/audioin_coreaudio.mm").find("requestAccessForMediaType:AVMediaTypeAudio") != std::string::npos, "the input asks for microphone access");
    std::printf("macos_plist: %d resources checked, %d failures\n", used, failures);
    return failures ? 1 : 0;
}
