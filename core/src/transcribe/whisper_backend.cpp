// whisper.cpp behind the SpeechBackend interface (transcriber.h). Compiled only with DECT2_WITH_WHISPER. CPU only.
#include "dect2/transcribe/transcriber.h"
#include "whisper.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {

class WhisperBackend : public SpeechBackend {
public:
    ~WhisperBackend() override {
        for (auto* s : free_) whisper_free_state(s);
        if (ctx_) whisper_free(ctx_);
    }
    bool load(const std::string& path, int workers, std::string* err) {
        whisper_log_set([](ggml_log_level, const char*, void*) {}, nullptr);   // nothing on the console
        whisper_context_params cp = whisper_context_default_params();
        cp.use_gpu = false;
        cp.flash_attn = false;
        ctx_ = whisper_init_from_file_with_params_no_state(path.c_str(), cp);
        if (!ctx_) { if (err) *err = "cannot load the speech model " + path + " (not a ggml whisper model, or damaged)"; return false; }
        multi_ = whisper_is_multilingual(ctx_) != 0;
        for (int i = 0; i < std::max(1, workers); i++) {
            whisper_state* s = whisper_init_state(ctx_);
            if (!s) { if (err) *err = "out of memory for the speech model"; return false; }
            free_.push_back(s);
        }
        return true;
    }
    const char* name() const override { return "whisper.cpp"; }
    void abort() override { abort_.store(true); }
    bool englishOnly() const override { return !multi_; }

    SpeechResult transcribe(const float* pcm, size_t n, const SpeechOptions& o) override {
        SpeechResult r;
        whisper_state* st = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!free_.empty()) { st = free_.back(); free_.pop_back(); }
        }
        if (!st) { r.error = "no free speech state"; return r; }
        whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        p.n_threads = std::max(1, o.threads);
        p.print_progress = p.print_realtime = p.print_timestamps = p.print_special = false;
        p.no_context = true;
        p.no_timestamps = true;
        p.single_segment = false;
        p.suppress_nst = true;
        p.temperature = 0.f;
        p.temperature_inc = 0.2f;
        p.no_speech_thold = 0.6f;
        p.abort_callback = [](void* u) { return static_cast<std::atomic<bool>*>(u)->load(); };
        p.abort_callback_user_data = &abort_;
        p.translate = multi_ && o.translate;
        p.initial_prompt = o.prompt.empty() ? nullptr : o.prompt.c_str();
        const std::string lang = multi_ ? o.language : std::string("en");
        // "auto": language "auto" makes whisper detect it and then transcribe. detect_language = true would only detect and return NO text.
        p.language = lang.c_str();
        p.detect_language = false;
        // at least a second of audio: the engine pads short input, but it hallucinates on less
        std::vector<float> padded;
        if (n < 16000) { padded.assign(pcm, pcm + n); padded.resize(16000, 0.f); pcm = padded.data(); n = padded.size(); }
        const int rc = whisper_full_with_state(ctx_, st, p, pcm, (int)n);
        if (rc != 0) r.error = "the speech engine failed (" + std::to_string(rc) + ")";
        else {
            r.ok = true;
            const int ns = whisper_full_n_segments_from_state(st);
            double sumP = 0, sumLp = 0;
            int nt = 0;
            float nsp = 0;
            for (int i = 0; i < ns; i++) {
                const char* t = whisper_full_get_segment_text_from_state(st, i);
                if (t) r.text += t;
                nsp = std::max(nsp, whisper_full_get_segment_no_speech_prob_from_state(st, i));
                const int ntok = whisper_full_n_tokens_from_state(st, i);
                for (int k = 0; k < ntok; k++) {
                    const whisper_token_data d = whisper_full_get_token_data_from_state(st, i, k);
                    if (d.id >= whisper_token_eot(ctx_)) continue;   // special tokens
                    sumP += d.p; sumLp += std::log(std::max(1e-6f, d.p)); nt++;
                }
            }
            { const int li = whisper_full_lang_id_from_state(st); if (li >= 0) r.lang = whisper_lang_str(li); }
            r.noSpeechProb = nsp;
            r.confidence = nt ? (float)(sumP / nt) : 0.f;
            r.avgLogprob = nt ? (float)(sumLp / nt) : 0.f;
            size_t a = r.text.find_first_not_of(" \t\r\n");
            r.text = a == std::string::npos ? std::string() : r.text.substr(a);
        }
        std::lock_guard<std::mutex> lk(mu_);
        free_.push_back(st);
        return r;
    }

private:
    whisper_context* ctx_ = nullptr;
    bool multi_ = false;
    std::atomic<bool> abort_{false};
    std::mutex mu_;
    std::vector<whisper_state*> free_;
};

}  // namespace

std::shared_ptr<SpeechBackend> makeWhisperBackend(const std::string& modelPath, int workers, std::string* err) {
    auto b = std::make_shared<WhisperBackend>();
    if (!b->load(modelPath, workers, err)) return nullptr;
    return b;
}

bool whisperAvailable() { return true; }

}  // namespace dect2
