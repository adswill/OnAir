// SPDX-License-Identifier: GPL-3.0-or-later
// Moonshine Voice backend for OnAir. Uses OnAir's existing transmission/segment queue,
// exactly like whisper_backend.cpp; native library is loaded on demand on Linux.
// Moonshine C API version 3: https://moonshine-voice.readthedocs.io/en/latest/api/c-api/
// There is deliberately no pipe, service, or background inference protocol here.
#include "dect2/transcribe/transcriber.h"
#include "moonshine_runtime_loader.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <vector>
#include <dlfcn.h>

namespace dect2 {
namespace {
std::mutex gEngineInfoMutex;
MoonshineEngineInfo gEngineInfo;
void setEngineInfo(const MoonshineEngineInfo& info) {
    std::lock_guard<std::mutex> lock(gEngineInfoMutex);
    gEngineInfo = info;
}
// ABI-compatible prefix of moonshine's transcript_line_t. Only 'text' is read.
// The full layout is not needed because we never access fields past 'text'.
struct MoonshineLine { const char* text; };
// ABI definition from Moonshine Voice v0.1.6 moonshine-c-api.h.
struct MoonshineTranscript { const MoonshineLine* lines; uint64_t line_count; };

using LoadFn = int32_t (*)(const char*, uint32_t, const void*, uint64_t, int32_t);
using FreeFn = void (*)(int32_t);
using TranscribeFn = int32_t (*)(int32_t, float*, uint64_t, int32_t, uint32_t, MoonshineTranscript**);
using ErrorFn = const char* (*)(int32_t);

// Moonshine's model architecture IDs from its v3 public C API.
constexpr uint32_t kTiny = 0, kBase = 1, kTinyStream = 2, kBaseStream = 3,
                   kSmallStream = 4, kMediumStream = 5;
constexpr int32_t kHeaderVersion = 30000;

uint32_t architectureFor(const std::string& dir, std::string* error) {
    const auto name = std::filesystem::path(dir).filename().string();
    // Deliberate explicit names: avoids mistakenly loading a different architecture.
    for (const auto& pair : std::vector<std::pair<std::string, uint32_t>>{
        {"moonshine-tiny-streaming-", kTinyStream}, {"moonshine-base-streaming-", kBaseStream},
        {"moonshine-small-streaming-", kSmallStream}, {"moonshine-medium-streaming-", kMediumStream},
        {"moonshine-tiny-", kTiny}, {"moonshine-base-", kBase}}) {
        if (name.rfind(pair.first, 0) == 0 && name.size() > pair.first.size()) return pair.second;
    }
    if (error) *error = "Moonshine folder name must be moonshine-{tiny|base|tiny-streaming|base-streaming|small-streaming|medium-streaming}-{language}, e.g. moonshine-small-streaming-de";
    return std::numeric_limits<uint32_t>::max();
}

std::string languageFor(const std::string& dir) {
    const auto name = std::filesystem::path(dir).filename().string();
    const auto pos = name.find_last_of('-');
    return pos == std::string::npos ? std::string() : name.substr(pos + 1);
}

class MoonshineBackend final : public SpeechBackend {
public:
    ~MoonshineBackend() override {
        // OnAir drops its shared_ptr only after its transcription workers have stopped.
        if (handle_ >= 0 && free_) free_(handle_);
        // The library handles are released, but RTLD_NODELETE keeps the native
        // runtimes mapped across a switch to Whisper.
    }
    bool load(const std::string& path, std::string* error) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::is_directory(path, ec)) {
            if (error) *error = "Moonshine model directory not found: " + path;
            return false;
        }
        const uint32_t arch = architectureFor(path, error);
        if (arch == std::numeric_limits<uint32_t>::max()) return false;
        // Moonshine uses different file layouts for streaming and non-streaming models.
        const bool streaming = arch >= kTinyStream;
        const char* required[] = {"tokenizer.bin", streaming ? "streaming_config.json" : "encoder_model.ort"};
        for (const char* file : required) if (!fs::is_regular_file(fs::path(path) / file, ec)) {
            if (error) *error = std::string("Moonshine model is incomplete: missing ") + file;
            return false;
        }
        if (!libraries_.open(std::filesystem::path(path).parent_path().string())) {
            MoonshineEngineInfo info;
            info.detail = "Moonshine/ONNX Runtime could not be loaded: " + libraries_.error;
            setEngineInfo(info);
            if (error) *error = info.detail;
            return false;
        }
        lib_ = libraries_.moonshine;
        load_ = reinterpret_cast<LoadFn>(dlsym(lib_, "moonshine_load_transcriber_from_files"));
        free_ = reinterpret_cast<FreeFn>(dlsym(lib_, "moonshine_free_transcriber"));
        transcribe_ = reinterpret_cast<TranscribeFn>(dlsym(lib_, "moonshine_transcribe_without_streaming"));
        error_ = reinterpret_cast<ErrorFn>(dlsym(lib_, "moonshine_error_to_string"));
        if (!load_ || !free_ || !transcribe_ || !error_) {
            if (error) *error = "libmoonshine.so does not provide the Moonshine Voice v3 C API";
            return false;
        }
        // Use Moonshine's default ONNX execution provider. No device override.
        handle_ = load_(path.c_str(), arch, nullptr, 0, kHeaderVersion);
        MoonshineEngineInfo info;
        info.moonshinePath = libraries_.moonshinePath;
        info.onnxPath = libraries_.onnxPath;
        info.moonshineVersion = libraries_.moonshineVersion;
        info.onnxVersion = libraries_.onnxVersion;
        if (handle_ < 0) {
            info.detail = std::string("Moonshine model load failed: ") + safeError(handle_);
            setEngineInfo(info);
            if (error) *error = info.detail;
            return false;
        }
        info.initialized = true;
        info.detail = "Library-default execution provider; actual device not verified";
        setEngineInfo(info);
        language_ = languageFor(path);
        return true;
    }
    const char* name() const override { return "moonshine-voice"; }
    bool englishOnly() const override { return language_ == "en"; }
    void abort() override { abort_.store(true); }
    SpeechResult transcribe(const float* pcm, size_t n, const SpeechOptions& options) override {
        (void)options; // Language is defined by the model; Moonshine doesn't implement Whisper translation.
        SpeechResult result;
        result.confidenceAvailable = false; // Moonshine C API does not provide token confidence
        if (abort_.load()) { result.error = "transcription cancelled"; return result; }
        if (!pcm || n == 0) { result.ok = true; return result; }
        // Moonshine accepts mutable float32 mono PCM; OnAir delivers immutable 16-kHz float samples.
        std::vector<float> data(pcm, pcm + n);
        std::lock_guard<std::mutex> lock(mu_); // also protects the transcript owned by this handle
        if (abort_.load()) { result.error = "transcription cancelled"; return result; }
        MoonshineTranscript* transcript = nullptr;
        const int32_t rc = transcribe_(handle_, data.data(), data.size(), 16000, 0, &transcript);
        if (rc != 0) { result.error = std::string("Moonshine recognition failed: ") + safeError(rc); return result; }
        if (transcript && transcript->lines) {
            // The v3 API exposes an array of transcript_line_t (not an array of pointers).
            // See the full structure layout below; stride must match the public header.
            const auto* lines = reinterpret_cast<const MoonshineLineFull*>(transcript->lines);
            for (uint64_t i = 0; i < transcript->line_count; ++i) {
                if (lines[i].text && *lines[i].text) {
                    if (!result.text.empty()) result.text += ' ';
                    result.text += lines[i].text;
                }
            }
        }
        result.ok = true;
        result.lang = language_;
        // Moonshine does not report Whisper's avg token log-probability/no-speech score.
        // Zero means 'unavailable' to OnAir's existing hallucination filters.
        return result;
    }
private:
    // Exact v3 transcript_line_t layout: required for safe indexing in transcript->lines.
    struct MoonshineLineFull {
        const char* text;
        const float* audio_data;
        size_t audio_data_count;
        float start_time;
        float duration;
        uint64_t id;
        int8_t is_complete, is_updated, is_new, has_text_changed, have_speakers_changed;
        const void* speaker_spans;
        uint64_t speaker_span_count;
        uint32_t last_transcription_latency_ms;
        const void* words;
        uint64_t word_count;
    };
    const char* safeError(int32_t code) const {
        const char* s = error_ ? error_(code) : nullptr;
        return s ? s : "unknown error";
    }
    MoonshineLibraries libraries_;
    void* lib_ = nullptr;
    int32_t handle_ = -1;
    LoadFn load_ = nullptr;
    FreeFn free_ = nullptr;
    TranscribeFn transcribe_ = nullptr;
    ErrorFn error_ = nullptr;
    std::atomic<bool> abort_{false};
    std::mutex mu_;
    std::string language_;
};
} // namespace

void initializeMoonshineEngineInfo(const std::string& modelsRoot) {
    // Only the first invocation loads libraries; this deliberately happens when
    // the Transcripts window is opened, never once per rendered frame.
    static std::once_flag once;
    std::call_once(once, [&] {
        static MoonshineLibraries libraries;
        MoonshineEngineInfo info;
        if (libraries.open(modelsRoot)) {
            info.moonshinePath = libraries.moonshinePath;
            info.onnxPath = libraries.onnxPath;
            info.moonshineVersion = libraries.moonshineVersion;
            info.onnxVersion = libraries.onnxVersion;
            info.detail = "Libraries loaded; model not initialized";
        } else info.detail = libraries.error;
        setEngineInfo(info);
    });
}

MoonshineEngineInfo moonshineEngineInfo() {
    std::lock_guard<std::mutex> lock(gEngineInfoMutex);
    return gEngineInfo;
}

std::shared_ptr<SpeechBackend> makeMoonshineBackend(const std::string& path, int, std::string* err) {
    auto backend = std::make_shared<MoonshineBackend>();
    return backend->load(path, err) ? backend : nullptr;
}
bool moonshineAvailable() { return true; } // runtime availability is reported when loading a model
} // namespace dect2
