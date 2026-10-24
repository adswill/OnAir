// Live transcription: voice audio in, transcript entries out. Per source (a channel) a VoiceSegmenter cuts the audio into transmissions; each
// transmission goes to a bounded queue (the oldest is dropped when the engine cannot keep up); worker threads run the speech engine on it (a
// SpeechBackend: whisper.cpp in the program, a fake one in the tests), the result goes through the hallucination filters, the callsigns in it
// are read, and it is added to the TranscriptStore. The caller's thread (the radio's, or the audio callback's) only copies samples and
// cuts the audio: it never waits for the speech engine. While no transmission is going on nothing runs, so an idle channel costs no battery.
#pragma once
#include "dect2/transcribe/callsign.h"
#include "dect2/transcribe/segmenter.h"
#include "dect2/transcribe/transcript.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {

struct SpeechOptions {
    std::string language = "en";   // "auto" detects (multilingual models only)
    bool translate = false;        // whisper task "translate": the text comes out in English (multilingual models only)
    std::string prompt;            // biases the words (initial prompt)
    int threads = 2;
};

struct SpeechResult {
    bool ok = false;
    std::string text, error;
    float noSpeechProb = 0;        // the engine's own probability that there was no speech
    float avgLogprob = 0;          // mean log probability of the tokens (<= 0)
    float confidence = 0;          // mean probability of the tokens, 0..1
    std::string lang;              // the language the engine detected or was told ("de"), "" when not known
};

class SpeechBackend {
public:
    virtual ~SpeechBackend() = default;
    virtual SpeechResult transcribe(const float* pcm16k, size_t n, const SpeechOptions& o) = 0;   // may be called from several threads
    virtual const char* name() const { return "speech"; }
    virtual bool englishOnly() const { return true; }
    virtual void abort() {}   // the program is ending: a transcription in progress returns as soon as it can
};

// whisper.cpp behind SpeechBackend. nullptr and *err when the model cannot be loaded, or when this build has no whisper.cpp.
std::shared_ptr<SpeechBackend> makeWhisperBackend(const std::string& modelPath, int workers, std::string* err);
bool whisperAvailable();

// What the filters decide on
struct FilterConfig {
    float noSpeechMax = 0.6f;      // with a poor avgLogprob, above this the text is dropped
    float minLogprob = -1.0f;
};
// "" keeps the text; otherwise why it was dropped ("empty", "no speech", "junk", "repeat", "too fast", "low confidence").
// `text` may be shortened (a phrase repeated over and over is cut to one).
std::string filterHallucination(std::string& text, const SpeechResult& r, double durSec, const FilterConfig& c);

struct TranscribeConfig {
    bool enabled = false;
    int workers = 1;               // transmissions transcribed at once
    int threads = 2;               // CPU threads of the speech engine per transmission (the CPU budget)
    size_t queueMax = 6;           // transmissions waiting; the oldest is dropped beyond
    std::string language = "en";
    bool translate = false;        // also keep an English translation next to the original text (a second pass; ~1.5 x the cost); needs a multilingual model
    double translateMaxRtf = 0.8;  // performance guard: above this average speed (engine time / audio time) the translation pass is skipped
    std::string extraPrompt;       // added to the built-in prompt of the domain
    FilterConfig filter;
    SegmenterOptions segmenter;
};

struct SourceInfo {
    int64_t key = 0;               // identifies the channel (the segmenter state lives under it)
    double freqHz = 0;
    std::string mode;              // "airband", "marine", "audio"
    std::string channel;           // "118.700 Tower"
    std::string domain = "aviation";   // picks the prompt: "aviation", "marine", "general"
    bool rejectMusic = false;      // skip transmissions with no speech rhythm (music, tones): captions of broadcast sound
};

struct TranscriberStats {
    uint64_t segments = 0;         // transmissions cut
    uint64_t dropped = 0;          // thrown away because the queue was full
    uint64_t done = 0;             // transcribed (text kept or not)
    uint64_t kept = 0, filtered = 0, errors = 0;
    uint64_t musicSkipped = 0;     // transmissions left out because they were music or a tone, not speech
    uint64_t translated = 0, translateSkipped = 0;   // translation passes done / skipped by the performance guard
    size_t queued = 0;
    bool busy = false;
    double lastRtf = 0;            // speech engine time / audio time of the last transmission (below 1 = faster than real time)
    double avgRtf = 0;
    std::string lastFilter;        // why the last text was dropped
    std::string error;
};

class Transcriber {
public:
    explicit Transcriber(TranscriptStore& store);
    ~Transcriber();
    void configure(const TranscribeConfig& c);
    TranscribeConfig config() const;
    void setBackend(std::shared_ptr<SpeechBackend> b);
    bool hasBackend() const;
    // The audio of a source: mono floats at `rate`. Cheap; never waits for the speech engine.
    void feed(const SourceInfo& src, const float* mono, size_t n, int rate, int64_t wallMs);
    void endSource(int64_t key, int64_t wallMs);         // the channel went away: finish its transmission
    // Called on the worker thread for each kept entry (after it is in the store)
    void setOnEntry(std::function<void(const TranscriptEntry&)> cb);
    // Which aircraft on the screen a callsign belongs to: returns the ICAO address (hex) or the callsign, "" when none.
    void setResolver(std::function<std::string(const CallsignHit&)> cb);
    TranscriberStats stats() const;
    bool active() const;                                  // a transmission is being cut or transcribed now
    void waitIdle();                                      // tests: until the queue is empty and nothing runs
    void setSynchronous(bool on);                         // tests: the work runs in feed(), no threads
    void clearStats();
    void shutdown();                                      // the program is ending: aborts the running transcription, stops the workers and waits for them

private:
    struct Job { SourceInfo src; VoiceSegment seg; int turn = 0; };
    struct Src { std::unique_ptr<VoiceSegmenter> seg; int rate = 0; int turn = 0; SourceInfo info; };
    void submit(Job&& j);
    void process(Job& j);
    void workerLoop();
    void startWorkers();
    void stopWorkers();
    bool takeSegment(Src& s, VoiceSegment&& g, std::vector<Job>& ready);   // false: music, counted and dropped
    std::string promptFor(const SourceInfo& s) const;

    TranscriptStore& store_;
    mutable std::mutex mu_;               // config, backend, sources, queue
    std::condition_variable cv_, idleCv_;
    TranscribeConfig cfg_;
    std::shared_ptr<SpeechBackend> backend_;
    std::map<int64_t, Src> src_;
    std::deque<Job> q_;
    std::vector<std::thread> th_;
    bool stop_ = false, sync_ = false;
    int running_ = 0;
    TranscriberStats st_;
    mutable std::mutex cbMu_;
    std::function<void(const TranscriptEntry&)> onEntry_;
    std::function<std::string(const CallsignHit&)> resolver_;
};

}  // namespace dect2
