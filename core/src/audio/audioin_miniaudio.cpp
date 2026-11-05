// Capture through miniaudio (WASAPI / ALSA / Pulse / PipeWire). MINIAUDIO_IMPLEMENTATION lives in audioout_miniaudio.cpp.
#include "audioin_impl.h"
#include "miniaudio.h"
#include "win_privacy.h"
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

namespace {
struct Cap {
    ma_context ctx{};
    ma_device dev{};
    bool haveCtx = false, haveDev = false;
};

void dataCb(ma_device* d, void*, const void* in, ma_uint32 frames) {
    static_cast<AudioIn::Impl*>(d->pUserData)->push(static_cast<const float*>(in), frames);
}

// A microphone is saved by its name, not by its place in the list: the order changes when a headset or a USB microphone comes or goes, and a
// saved number then opened another microphone. Two with the same name get " #2", " #3" (in the order the sound system gives them).
struct Mic {
    std::string id, name;
    ma_device_id dev;
};

std::vector<Mic> listCapture(ma_context* ctx) {
    std::vector<Mic> v;
    ma_device_info *play = nullptr, *cap = nullptr;
    ma_uint32 nPlay = 0, nCap = 0;
    if (ma_context_get_devices(ctx, &play, &nPlay, &cap, &nCap) != MA_SUCCESS) return v;
    for (ma_uint32 i = 0; i < nCap; i++) {
        const std::string name = cap[i].name;
        int same = 1;
        for (const Mic& m : v) same += m.name == name;
        v.push_back({same == 1 ? name : name + " #" + std::to_string(same), name, cap[i].id});
    }
    return v;
}

// deviceId from listCapture, or a number: the settings of older versions saved the place in the list
const Mic* findMic(const std::vector<Mic>& mics, const std::string& deviceId) {
    for (const Mic& m : mics) if (m.id == deviceId) return &m;
    if (deviceId.find_first_not_of("0123456789") == std::string::npos && deviceId.size() < 4) {
        const size_t idx = std::stoul(deviceId);
        if (idx < mics.size()) return &mics[idx];
    }
    return nullptr;
}
}

bool audioInMicStart(AudioIn::Impl* I, int rate, const std::string& deviceId, std::string& err) {
    auto* b = new Cap;
    ma_context_config cc = ma_context_config_init();
    const ma_result cr = ma_context_init(nullptr, 0, &cc, &b->ctx);
    if (cr != MA_SUCCESS) {
        delete b;
        err = std::string("no sound system for capture (") + ma_result_description(cr) + ")";
        return false;
    }
    b->haveCtx = true;
    ma_device_config dc = ma_device_config_init(ma_device_type_capture);
    dc.capture.format = ma_format_f32;
    dc.capture.channels = 2;
    dc.sampleRate = (ma_uint32)rate;
    dc.dataCallback = dataCb;
    dc.pUserData = I;
    ma_device_id chosen{};
    if (!deviceId.empty()) {
        const auto mics = listCapture(&b->ctx);
        const Mic* m = findMic(mics, deviceId);
        if (!m) {
            ma_context_uninit(&b->ctx);
            delete b;
            err = "the chosen microphone (" + deviceId + ") is not connected: choose another one or the default input";
            return false;
        }
        chosen = m->dev;
        dc.capture.pDeviceID = &chosen;
    }
    ma_result r = ma_device_init(&b->ctx, &dc, &b->dev);
    if (r == MA_SUCCESS) {
        b->haveDev = true;
        r = ma_device_start(&b->dev);
    }
    if (r != MA_SUCCESS) {
        if (b->haveDev) ma_device_uninit(&b->dev);
        const char* backend = ma_get_backend_name(b->ctx.backend);
        ma_context_uninit(&b->ctx);
        delete b;
        err = std::string("the microphone could not be started (") + backend + ": " + ma_result_description(r) + ")";
#ifdef _WIN32
        err += winPrivacyMicHint();
#endif
        return false;
    }
    I->backend = b;
    return true;
}

void audioInMicStop(AudioIn::Impl* I) {
    auto* b = static_cast<Cap*>(I->backend);
    if (!b) return;
    if (b->haveDev) ma_device_uninit(&b->dev);
    if (b->haveCtx) ma_context_uninit(&b->ctx);
    delete b;
    I->backend = nullptr;
}

// The transmitter panel lists every frame and opening the sound system takes milliseconds (WASAPI: tens): the list is kept for a few seconds.
std::vector<AudioInputDevice> audioInListMic() {
    static std::mutex mu;
    static std::vector<AudioInputDevice> cache;
    static std::chrono::steady_clock::time_point at;
    std::lock_guard<std::mutex> lk(mu);
    const auto now = std::chrono::steady_clock::now();
    if (at != std::chrono::steady_clock::time_point() && now - at < std::chrono::seconds(3)) return cache;
    at = now;
    cache.clear();
    ma_context ctx;
    ma_context_config cc = ma_context_config_init();
    if (ma_context_init(nullptr, 0, &cc, &ctx) != MA_SUCCESS) return cache;
    for (const Mic& m : listCapture(&ctx)) cache.push_back({m.id, m.name});
    ma_context_uninit(&ctx);
    return cache;
}

} // namespace dect2
