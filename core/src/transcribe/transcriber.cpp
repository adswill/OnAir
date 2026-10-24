#include "dect2/common/thread_name.h"
#include "dect2/transcribe/transcriber.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

namespace dect2 {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string lettersOnly(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c)) o += (char)std::tolower(c);
        else if (std::isspace(c) && !o.empty() && o.back() != ' ') o += ' ';
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

// Removes [BLANK_AUDIO], (music), *sigh* and the note signs the speech engine writes for sounds.
std::string stripSoundTags(const std::string& s) {
    std::string o;
    int depthB = 0, depthP = 0;
    bool star = false;
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '[') { depthB++; continue; }
        if (c == ']') { if (depthB) depthB--; continue; }
        if (c == '(') { depthP++; continue; }
        if (c == ')') { if (depthP) depthP--; continue; }
        if (c == '*') { star = !star; continue; }
        if (depthB || depthP || star) continue;
        if (c == 0xE2 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x99 && ((unsigned char)s[i + 2] == 0xAA || (unsigned char)s[i + 2] == 0xAB)) { i += 2; continue; }   // music notes
        o += (char)c;
    }
    return trim(o);
}

const std::set<std::string>& junkPhrases() {
    static const std::set<std::string> s = {
        "thank you", "thanks", "thank you very much", "thanks for watching", "thank you for watching", "thank you so much", "you", "bye", "bye bye",
        "goodbye", "the end", "please subscribe", "like and subscribe", "subscribe", "see you next time", "see you in the next video", "i", "uh", "um",
        "hmm", "mm", "mmm", "oh", "ah", "so", "yeah", "you you", "thank you thank you", "subtitles by the amaraorg community", "transcribed by",
        "www", "music", "applause", "silence", "blank audio",
    };
    return s;
}

bool isNumberWord(const std::string& w) {
    static const std::set<std::string> n = {"zero", "one", "two", "three", "tree", "four", "fower", "five", "fife", "six", "seven", "eight", "nine", "niner"};
    return n.count(w) > 0 || std::all_of(w.begin(), w.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// A phrase (1 to 8 words) repeated back to back: a loop of the decoder. Marks the repeats (all but the first) in `drop`; returns how many
// words they cover.
size_t markRepeats(const std::vector<std::string>& w, std::vector<bool>& drop) {
    size_t covered = 0;
    drop.assign(w.size(), false);
    for (size_t n = 1; n <= 8; n++) {
        for (size_t i = 0; i + n * 2 <= w.size();) {
            size_t k = 1;
            while (i + (k + 1) * n <= w.size() && std::equal(w.begin() + (long)i, w.begin() + (long)(i + n), w.begin() + (long)(i + k * n))) k++;
            bool nums = true;
            for (size_t j = 0; j < n; j++) nums &= isNumberWord(w[i + j]);
            const size_t need = n == 1 ? 6 : 3;   // one word repeated is common speech ("go go go"), a phrase three times is not
            if (k >= need && !nums) {
                for (size_t x = i + n; x < i + k * n; x++) if (!drop[x]) { drop[x] = true; covered++; }
                i += k * n;
            } else i++;
        }
    }
    return covered;
}

}  // namespace

std::string filterHallucination(std::string& text, const SpeechResult& r, double durSec, const FilterConfig& c) {
    std::string t = stripSoundTags(trim(text));
    if (t.empty()) return text.empty() ? "empty" : "junk";
    const std::string norm = lettersOnly(t);
    if (norm.empty()) return "junk";
    if (junkPhrases().count(norm)) return "junk";
    if (r.avgLogprob < -1.5f && r.avgLogprob != 0) return "low confidence";
    if (r.noSpeechProb > c.noSpeechMax && r.avgLogprob < c.minLogprob) return "no speech";
    std::vector<std::string> w;
    {
        std::istringstream in(t);
        std::string x;
        while (in >> x) w.push_back(x);
    }
    std::vector<std::string> wl;
    for (const auto& x : w) wl.push_back(lettersOnly(x));
    // a loop: the same phrase over and over
    std::vector<bool> drop;
    const size_t covered = markRepeats(wl, drop);
    if (covered) {
        if (covered * 10 >= wl.size() * 6 || wl.size() - covered <= 2) return "repeat";   // mostly a loop
        std::string rebuilt;
        for (size_t i = 0; i < w.size(); i++) if (!drop[i]) { if (!rebuilt.empty()) rebuilt += ' '; rebuilt += w[i]; }
        t = rebuilt;
    }
    if (durSec > 0 && (double)norm.size() / durSec > 30.0 && norm.size() > 40) return "too fast";
    text = t;
    return "";
}

// --------------------------------------------------------------------------------------------------------------------------------------

Transcriber::Transcriber(TranscriptStore& store) : store_(store) {}

Transcriber::~Transcriber() { stopWorkers(); }

void Transcriber::configure(const TranscribeConfig& c) {
    bool restart = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        restart = c.workers != cfg_.workers && !th_.empty();
        cfg_ = c;
        cfg_.workers = std::max(1, std::min(4, cfg_.workers));
        cfg_.threads = std::max(1, std::min(16, cfg_.threads));
        cfg_.queueMax = std::max<size_t>(1, cfg_.queueMax);
        for (auto& kv : src_) kv.second.seg.reset();   // new segmenter options apply from the next audio
        if (!cfg_.enabled) { q_.clear(); }
    }
    if (restart) { stopWorkers(); startWorkers(); }
    else if (c.enabled) startWorkers();
}

TranscribeConfig Transcriber::config() const { std::lock_guard<std::mutex> lk(mu_); return cfg_; }

void Transcriber::setBackend(std::shared_ptr<SpeechBackend> b) {
    std::lock_guard<std::mutex> lk(mu_);
    backend_ = std::move(b);
    if (!backend_) q_.clear();
}

bool Transcriber::hasBackend() const { std::lock_guard<std::mutex> lk(mu_); return backend_ != nullptr; }

void Transcriber::setOnEntry(std::function<void(const TranscriptEntry&)> cb) { std::lock_guard<std::mutex> lk(cbMu_); onEntry_ = std::move(cb); }
void Transcriber::setResolver(std::function<std::string(const CallsignHit&)> cb) { std::lock_guard<std::mutex> lk(cbMu_); resolver_ = std::move(cb); }

void Transcriber::setSynchronous(bool on) {
    if (on) stopWorkers();
    std::lock_guard<std::mutex> lk(mu_);
    sync_ = on;
}

TranscriberStats Transcriber::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    TranscriberStats s = st_;
    s.queued = q_.size();
    s.busy = running_ > 0;
    return s;
}

void Transcriber::shutdown() {
    std::shared_ptr<SpeechBackend> be;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cfg_.enabled = false;
        q_.clear();
        be = backend_;
    }
    if (be) be->abort();   // whisper keeps running its decoder otherwise, and the program's statics (ggml's mutex) are gone while it does: a crash at exit
    stopWorkers();
}

void Transcriber::clearStats() { std::lock_guard<std::mutex> lk(mu_); st_ = TranscriberStats{}; }

bool Transcriber::active() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!q_.empty() || running_ > 0) return true;
    for (const auto& kv : src_) if (kv.second.seg && kv.second.seg->active()) return true;
    return false;
}

void Transcriber::waitIdle() {
    std::unique_lock<std::mutex> lk(mu_);
    idleCv_.wait(lk, [&] { return q_.empty() && running_ == 0; });
}

void Transcriber::startWorkers() {
    std::lock_guard<std::mutex> lk(mu_);
    if (sync_ || !th_.empty()) return;
    stop_ = false;
    for (int i = 0; i < cfg_.workers; i++) th_.emplace_back(dect2::namedThread("onair-asr", [this] { workerLoop(); }));
}

void Transcriber::stopWorkers() {
    std::vector<std::thread> t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        t.swap(th_);
    }
    cv_.notify_all();
    for (auto& x : t) if (x.joinable()) x.join();
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = false;
}

std::string Transcriber::promptFor(const SourceInfo& s) const {
    std::string p;
    if (s.domain == "aviation")
        p = "Air traffic control radio. Lufthansa, Speedbird, Ryanair, Wizz, Emirates, Delta, November, alpha bravo charlie, cleared to land, runway two five left, "
            "heading, flight level, QNH, squawk, taxi, hold short, line up and wait, wilco, roger, niner, tree, fife.";
    else if (s.domain == "marine")
        p = "Marine VHF radio. Mayday, Pan-pan, Securite, channel sixteen, Coast Guard, vessel, position, over, out.";
    if (!cfg_.extraPrompt.empty()) p += (p.empty() ? "" : " ") + cfg_.extraPrompt;
    return p;
}

bool Transcriber::takeSegment(Src& s, VoiceSegment&& g, std::vector<Job>& ready) {   // mu_ is held
    if (std::getenv("ONAIR_TX_DEBUG")) fprintf(stderr, "tx-debug: piece %.1f s level %.1f dB syllabic %.2f stdDb %.1f music %d\n", g.durSec, g.levelDb, g.syllabic, g.stdDb, g.music ? 1 : 0);
    if (g.music) { st_.musicSkipped++; return false; }
    Job j;
    j.src = s.info; j.seg = std::move(g); j.turn = ++s.turn;
    ready.push_back(std::move(j));
    return true;
}

void Transcriber::feed(const SourceInfo& src, const float* mono, size_t n, int rate, int64_t wallMs) {
    std::vector<Job> ready;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!cfg_.enabled || !backend_ || !n) return;
        if (src_.size() > 64)
            for (auto it = src_.begin(); it != src_.end();) it = (it->first != src.key && (!it->second.seg || !it->second.seg->active())) ? src_.erase(it) : std::next(it);
        Src& s = src_[src.key];
        if (!s.seg || s.rate != rate) {
            SegmenterOptions so = cfg_.segmenter;
            so.rejectMusic = so.rejectMusic || src.rejectMusic;
            s.seg = std::make_unique<VoiceSegmenter>(rate, so);
            s.rate = rate;
        }
        s.info = src;
        s.seg->push(mono, n, wallMs, [&](VoiceSegment&& g) { takeSegment(s, std::move(g), ready); });
    }
    for (auto& j : ready) submit(std::move(j));
}

void Transcriber::endSource(int64_t key, int64_t wallMs) {
    std::vector<Job> ready;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = src_.find(key);
        if (it == src_.end() || !it->second.seg) return;
        Src& s = it->second;
        s.seg->flush(wallMs, [&](VoiceSegment&& g) { takeSegment(s, std::move(g), ready); });
    }
    for (auto& j : ready) submit(std::move(j));
}

void Transcriber::submit(Job&& j) {
    bool sync = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.segments++;
        if (sync_) sync = true;
        else {
            while (q_.size() >= cfg_.queueMax) { q_.pop_front(); st_.dropped++; }   // the oldest is the least useful now
            q_.push_back(std::move(j));
        }
    }
    if (sync) { process(j); return; }
    cv_.notify_one();
}

void Transcriber::workerLoop() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
            if (stop_) return;
            j = std::move(q_.front());
            q_.pop_front();
            running_++;
        }
        process(j);
        {
            std::lock_guard<std::mutex> lk(mu_);
            running_--;
        }
        idleCv_.notify_all();
    }
}

void Transcriber::process(Job& j) {
    std::shared_ptr<SpeechBackend> be;
    TranscribeConfig cfg;
    SpeechOptions so;
    {
        std::lock_guard<std::mutex> lk(mu_);
        be = backend_;
        cfg = cfg_;
        so.prompt = promptFor(j.src);
    }
    if (!be) return;
    so.threads = cfg.threads;
    so.language = (cfg.language == "auto" && be->englishOnly()) ? "en" : cfg.language;
    if (be->englishOnly()) so.language = "en";
    const auto t0 = std::chrono::steady_clock::now();
    SpeechResult r = be->transcribe(j.seg.pcm16k.data(), j.seg.pcm16k.size(), so);
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();   // the guard looks at the first pass only
    // live translation: a second pass in the "translate" task, only for foreign speech, and not when the engine is already too slow
    std::string translation;
    bool wantTr = false, trSkipped = false;
    if (cfg.translate && !be->englishOnly() && r.ok && so.language != "en" && r.lang != "en") {
        double rtf;
        { std::lock_guard<std::mutex> lk(mu_); rtf = st_.avgRtf; }
        if (rtf > cfg.translateMaxRtf) trSkipped = true;
        else wantTr = true;
    }
    const double audioSec = (double)j.seg.pcm16k.size() / 16000.0;
    std::string text = r.text;
    std::string why = r.ok ? filterHallucination(text, r, j.seg.durSec, cfg.filter) : "error";
    if (wantTr && why.empty()) {
        SpeechOptions to = so;
        to.translate = true;
        if (to.language == "auto" && !r.lang.empty()) to.language = r.lang;   // no second detection
        SpeechResult tr = be->transcribe(j.seg.pcm16k.data(), j.seg.pcm16k.size(), to);
        if (tr.ok) {
            std::string t2 = tr.text;
            if (filterHallucination(t2, tr, j.seg.durSec, cfg.filter).empty()) translation = t2;
        }
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.done++;
        if (!translation.empty()) st_.translated++;
        if (trSkipped) st_.translateSkipped++;
        if (audioSec > 0) {
            st_.lastRtf = el / audioSec;
            st_.avgRtf = st_.avgRtf == 0 ? st_.lastRtf : st_.avgRtf * 0.8 + st_.lastRtf * 0.2;
        }
        if (!r.ok) { st_.errors++; st_.error = r.error; }
        else if (!why.empty()) { st_.filtered++; st_.lastFilter = why; }
        else st_.kept++;
    }
    idleCv_.notify_all();
    if (!r.ok || !why.empty()) return;
    TranscriptEntry e;
    e.wall = j.seg.startMs / 1000;
    e.startMs = j.seg.startMs;
    e.freqHz = j.src.freqHz;
    e.mode = j.src.mode;
    e.channel = j.src.channel;
    e.durSec = j.seg.durSec;
    e.text = text;
    e.lang = r.lang.empty() && so.language != "auto" ? so.language : r.lang;
    e.translation = translation;
    e.confidence = r.confidence;
    e.turn = j.turn;
    std::function<std::string(const CallsignHit&)> res;
    std::function<void(const TranscriptEntry&)> cb;
    { std::lock_guard<std::mutex> lk(cbMu_); res = resolver_; cb = onEntry_; }
    if (j.src.domain == "aviation") {
        const std::vector<CallsignHit> hits = findCallsigns(text);
        for (const auto& h : hits) {
            if (h.kind == CallsignHit::Flight && e.callsign.empty()) e.callsign = h.key;
            if (h.kind == CallsignHit::Reg && e.reg.empty()) e.reg = h.key;
        }
        e.speaker = guessSpeaker(hits);
        if (res) for (const auto& h : hits) { const std::string a = res(h); if (!a.empty()) { e.aircraft = a; break; } }
    }
    e.id = store_.add(e);
    if (cb) cb(e);
}

}  // namespace dect2

#ifndef DECT2_HAVE_WHISPER
namespace dect2 {
std::shared_ptr<SpeechBackend> makeWhisperBackend(const std::string&, int, std::string* err) {
    if (err) *err = "this build has no speech engine (built without DECT2_WITH_WHISPER)";
    return nullptr;
}
bool whisperAvailable() { return false; }
}  // namespace dect2
#endif
