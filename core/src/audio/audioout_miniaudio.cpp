// The fallback sound backend for Windows and Linux: miniaudio (ALSA / PulseAudio / PipeWire on Linux, WASAPI on Windows), used when our own
// backend (audioout_native.cpp) cannot open the sound device, or when DECT2_AUDIO=miniaudio is set. If no sound device can be opened (a
// headless machine, CI), a null device with correct timing is used so that playback and A/V sync still behave.
#include "audioout_impl.h"
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

namespace dect2 {

namespace {
struct Backend {
    ma_context ctx{};
    ma_device dev{};
    bool haveCtx = false, haveDev = false;
};

void dataCb(ma_device* d, void* out, const void*, ma_uint32 frames) {
    static_cast<AudioOut::Impl*>(d->pUserData)->render(static_cast<float*>(out), frames);
}

bool open(Backend* b, AudioOut::Impl* I, int rate, const ma_backend* backends, ma_uint32 nBackends) {
    ma_context_config cc = ma_context_config_init();
    if (ma_context_init(backends, nBackends, &cc, &b->ctx) != MA_SUCCESS) return false;
    b->haveCtx = true;
    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.format = ma_format_f32;
    dc.playback.channels = (ma_uint32)I->channels;   // miniaudio's default map for 6 / 8 is the WAVE order AudioOut uses
    dc.sampleRate = (ma_uint32)rate;
    ma_device_id chosen{};
    if (!I->device.empty()) {   // a named output device; not found: the default
        ma_device_info* pb = nullptr; ma_uint32 npb = 0;
        if (ma_context_get_devices(&b->ctx, &pb, &npb, nullptr, nullptr) == MA_SUCCESS)
            for (ma_uint32 i = 0; i < npb; i++) if (I->device == pb[i].name) { chosen = pb[i].id; dc.playback.pDeviceID = &chosen; break; }
    }
    dc.dataCallback = dataCb;
    dc.pUserData = I;
    if (ma_device_init(&b->ctx, &dc, &b->dev) != MA_SUCCESS) return false;
    b->haveDev = true;
    return ma_device_start(&b->dev) == MA_SUCCESS;
}

void close(Backend* b) {
    if (b->haveDev) ma_device_uninit(&b->dev);
    if (b->haveCtx) ma_context_uninit(&b->ctx);
    b->haveDev = b->haveCtx = false;
}
}

std::vector<std::string> audioListOutputs() {
    std::vector<std::string> v;
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return v;
    ma_device_info* pb = nullptr; ma_uint32 npb = 0;
    if (ma_context_get_devices(&ctx, &pb, &npb, nullptr, nullptr) == MA_SUCCESS) for (ma_uint32 i = 0; i < npb; i++) v.push_back(pb[i].name);
    ma_context_uninit(&ctx);
    return v;
}

bool audioMiniaudioStart(AudioOut::Impl* I, int rate) {
    auto* b = new Backend;
    if (!open(b, I, rate, nullptr, 0)) {
        close(b);
        *b = Backend{};
        const ma_backend nullOnly[] = {ma_backend_null};
        if (!open(b, I, rate, nullOnly, 1)) { close(b); delete b; return false; }
    }
    I->backend = b;
    return true;
}

void audioMiniaudioStop(AudioOut::Impl* I) {
    auto* b = static_cast<Backend*>(I->backend);
    if (!b) return;
    close(b);
    delete b;
    I->backend = nullptr;
}

} // namespace dect2
