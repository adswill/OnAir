// Voice mixer and call recorder of the Inmarsat all-carriers mode (inmsat_voice.h).
#include "dect2/inmsat/inmsat_voice.h"
#include "dect2/codec/codec_registry.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <filesystem>
#include <mutex>

namespace dect2::inmsat {

namespace {
constexpr double kIdleSec = 3.0;
constexpr size_t kFifoMax = 16000;   // 2 s of 8 kHz audio per channel

void wr32(std::FILE* f, uint32_t v) { for (int i = 0; i < 4; i++) std::fputc((int)((v >> (8 * i)) & 0xFF), f); }
void wr16(std::FILE* f, uint16_t v) { std::fputc(v & 0xFF, f); std::fputc(v >> 8, f); }

struct Wav {
    std::FILE* f = nullptr;
    uint32_t bytes = 0;
    bool open(const std::string& path, int rate) {
        f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        std::fwrite("RIFF", 1, 4, f); wr32(f, 0); std::fwrite("WAVEfmt ", 1, 8, f);
        wr32(f, 16); wr16(f, 1); wr16(f, 1); wr32(f, (uint32_t)rate); wr32(f, (uint32_t)rate * 2); wr16(f, 2); wr16(f, 16);
        std::fwrite("data", 1, 4, f); wr32(f, 0);
        return true;
    }
    void write(const int16_t* p, size_t n) {
        if (!f) return;
        for (size_t i = 0; i < n; i++) wr16(f, (uint16_t)p[i]);
        bytes += (uint32_t)(n * 2);
    }
    void close() {
        if (!f) return;
        std::fseek(f, 4, SEEK_SET); wr32(f, 36 + bytes);
        std::fseek(f, 40, SEEK_SET); wr32(f, bytes);
        std::fclose(f);
        f = nullptr;
    }
};
}

struct VoiceMixer::Impl {
    mutable std::mutex mu;
    DecoderFactory factory;
    std::string folder = "the codecs folder";
    std::string recDir;
    int callCounter = 0;
    struct Ch {
        VoiceChannelInfo info;
        std::unique_ptr<codec::SpeechCodec> dec;
        bool triedDec = false;
        std::deque<int16_t> fifo;
        Wav wav;
        std::FILE* raw = nullptr;
    };
    std::map<int, Ch> chans;
    bool codecReady = false;

    DecoderFactory fac() const { return factory ? factory : DecoderFactory([] { return codec::pluginVocoder("aeroambe"); }); }
    bool recDirEmptyImpl() const { return recDir.empty(); }

    void closeCall(Ch& c) {
        c.wav.close();
        if (c.raw) { std::fclose(c.raw); c.raw = nullptr; }
        c.info.active = false;
        c.info.recording.clear(); c.info.rawFile.clear();
    }
    void openCall(Ch& c, double timeSec) {
        c.info.active = true;
        c.info.callStart = timeSec;
        if (recDir.empty()) return;
        char nm[96];
        std::snprintf(nm, sizeof nm, "inmarsat_%ld_%d", (long)std::lround(c.info.freqHz / 1000), ++callCounter);
        const std::string base = recDir + "/" + nm;
        c.raw = std::fopen((base + ".bits").c_str(), "wb");
        if (c.raw) c.info.rawFile = base + ".bits";
        if (c.dec && c.wav.open(base + ".wav", c.dec->sampleRate())) c.info.recording = base + ".wav";
    }
};

VoiceMixer::VoiceMixer() : p_(std::make_unique<Impl>()) {}
VoiceMixer::~VoiceMixer() { std::lock_guard<std::mutex> lk(p_->mu); for (auto& kv : p_->chans) p_->closeCall(kv.second); }
void VoiceMixer::setDecoderFactory(DecoderFactory f) { std::lock_guard<std::mutex> lk(p_->mu); p_->factory = std::move(f); p_->chans.clear(); }
void VoiceMixer::setCodecFolder(const std::string& folder) { std::lock_guard<std::mutex> lk(p_->mu); p_->folder = folder; }
void VoiceMixer::setRecording(const std::string& dir) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->recDir = dir;
    if (!dir.empty()) { std::error_code ec; std::filesystem::create_directories(std::filesystem::path(dir), ec); }   // the pictures folder's Inmarsat subfolder may not exist yet
}
void VoiceMixer::setMute(int id, bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->chans[id].info.id = id; p_->chans[id].info.mute = on; }
void VoiceMixer::setSolo(int id, bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->chans[id].info.id = id; p_->chans[id].info.solo = on; }

void VoiceMixer::pushFrame(int id, double freqHz, const std::vector<uint8_t>& coded, double timeSec) {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto& c = p_->chans[id];
    c.info.id = id; c.info.freqHz = freqHz;
    auto recDirEmpty = [&] { return p_->recDirEmptyImpl(); };
    // one decoder per channel; the factory returns nullptr while the codec is not installed (or not loaded yet: the app loads the plug-in a
    // moment after the start), so a channel without one asks again at every new call and every 5 s of frames
    if (!c.dec && (!c.triedDec || !c.info.active || c.info.frames % 250 == 0)) {
        c.triedDec = true;
        c.dec = p_->fac()();
        if (c.dec && c.info.active && !recDirEmpty() && c.info.recording.empty()) {   // a call that began without the codec: start its WAV now
            const std::string base = c.info.rawFile.size() > 5 ? c.info.rawFile.substr(0, c.info.rawFile.size() - 5) : std::string();
            if (!base.empty() && c.wav.open(base + ".wav", c.dec->sampleRate())) c.info.recording = base + ".wav";
        }
    }
    if (!c.info.active) p_->openCall(c, timeSec);
    c.info.frames++;
    c.info.lastFrame = timeSec;
    if (c.raw) std::fwrite(coded.data(), 1, coded.size(), c.raw);
    if (!c.dec) return;
    const std::vector<int16_t> pcm = c.dec->decode(coded);
    if (pcm.empty()) return;
    c.info.decoded++;
    p_->codecReady = true;
    if (c.fifo.size() + pcm.size() <= kFifoMax) c.fifo.insert(c.fifo.end(), pcm.begin(), pcm.end());
    c.wav.write(pcm.data(), pcm.size());
}

void VoiceMixer::endIdle(double timeSec) {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& kv : p_->chans) if (kv.second.info.active && timeSec - kv.second.info.lastFrame > kIdleSec) p_->closeCall(kv.second);
}

size_t VoiceMixer::mix(int16_t* out, size_t n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<int32_t> acc(n, 0);
    bool anySolo = false;
    for (auto& kv : p_->chans) anySolo |= kv.second.info.solo;
    for (auto& kv : p_->chans) {
        Impl::Ch& c = kv.second;
        const bool play = anySolo ? c.info.solo : !c.info.mute;
        float peak = 0;
        for (size_t i = 0; i < n && !c.fifo.empty(); i++) {     // a muted channel still runs, so unmuting does not play old audio
            const int16_t v = c.fifo.front(); c.fifo.pop_front();
            peak = std::max(peak, std::fabs((float)v) / 32768.f);
            if (play) acc[i] += v;
        }
        if (peak > 0) c.info.level = peak;
    }
    for (size_t i = 0; i < n; i++) out[i] = (int16_t)std::max(-32768, std::min(32767, acc[i]));
    return n;
}

VoiceStatus VoiceMixer::status() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    VoiceStatus s;
    s.codecReady = p_->codecReady;
    for (const auto& kv : p_->chans) {
        s.channels.push_back(kv.second.info);
        s.activeCalls += kv.second.info.active;
    }
    if (!s.codecReady) {
        bool loaded = codec::pluginLoaded("aeroambe");
        if (p_->factory) loaded = true;        // a test or the app supplied its own decoder: it works or says so when asked to decode
        if (!loaded)
            s.message = "Voice needs the Aero AMBE codec (4800 bit/s), which OnAir does not include. Install it yourself: " + codec::pluginStatusLine("aeroambe", p_->folder) +
                        " See docs/CODECS.md. Until then the voice stays silent; calls are still counted and their coded frames are saved.";
    }
    return s;
}

} // namespace dect2::inmsat
