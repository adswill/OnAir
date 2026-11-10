// HF ALE receiver (see ale_rx.h).
#include "dect2/ale/ale_rx.h"
#include "dect2/automation/engine.h"
#include "dect2/ale/ale_lqa.h"
#include "dect2/results/results_io.h"
#include "dect2/ale/ale_tel.h"
#include "dect2/ale/ale_proto.h"
#include "dect2/marine/marine_dsp.h"
#include "../audio/ham_listen.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"
#include "dect2/dsp/dsp_compat.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kFs = 8000;                   // audio
constexpr int kSps = 64;                       // 8 ms
constexpr int kHop = 2;                        // tones measured every 2 samples
constexpr int kHps = kSps / kHop;              // 32 instants a symbol
constexpr int kWordHops = ale::kWordSymbols * kHps;   // 1568: one Tw
constexpr int kTrack = 4;                      // in sync: the next word within +-4 instants
constexpr int kAudioN = 8192, kHopN = 4096;    // rings (powers of two)
constexpr int kAcqVote = 10, kAcqGolay = 2;    // to find the word clock: few errors of either kind
constexpr int kTrackVote = 24;                 // in sync
// acquisition (3.9 sensitivity pass D): the tuning error from the averaged audio spectrum, the 8 tone bands against the bands just outside
// them, while no word is in sync; up to +-kSearchHz (a 20 ppm radio at 14 MHz is 280 Hz off: the decoder, whose tone filters are 125 Hz
// wide and whose AFC moves only after a decoded word, found nothing beyond about 100 Hz)
constexpr int kSpecLog2 = 10, kSpecN = 1 << kSpecLog2;   // 7.8 Hz bins
constexpr double kSearchHz = 400;

double nowUtc() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

bool plausible(const ale::Word& w) {   // characters a word of its type can carry
    if (w.type == ale::Cmd) return (unsigned char)w.c[0] >= 0x20;   // an AMD text or a command letter, then anything
    for (char c : w.c) {
        if (w.type == ale::Data || w.type == ale::Rep) { if (!ale::asciiChar64(c)) return false; }
        else if (!ale::basicChar(c)) return false;
    }
    return true;
}

std::string printable(const std::string& s) {
    std::string o = s;
    for (char& c : o) if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) c = '.';
    return o;
}
} // namespace

struct AleReceiver::Impl {
    std::mutex mu;                       // pub, settings
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    std::atomic<double> dialHz{0};
    double startReq = -1;
    std::vector<std::string> nets;
    AleTelemetry pub;
    std::atomic<bool> resetReq{true}, offsetReq{false}, clearReq{false};
    // feed thread
    double curRate = 0;
    bool running = false;
    marine::Front front;
    hfdig::UsbAudio usb;
    std::atomic<int> demodSet{HfDemodUsb};   // setDemod()
    hfdig::Listen listen;
    std::vector<cf32> base;
    std::vector<float> aud;
    float ring[kAudioN] = {};
    int64_t na = 0;                      // audio samples so far
    double t0 = 0;
    // the tones of every instant
    uint8_t hsym[kHopN] = {};
    float hq[kHopN] = {}, hbest[kHopN] = {}, hother[kHopN] = {};
    int64_t nh = 0;                      // instants so far
    double afc = 0, twAfc = 1e9;
    std::vector<float> spRe, spIm, spPow;   // the averaged audio spectrum (acquisition)
    int64_t spNext = kSpecN, spBlocks = 0, spLastWord = 0;
    double spCand = 1e9;
    float twc[8][kSps], tws[8][kSps];
    // word sync
    bool locked = false, acq = false;
    int64_t acqEnd = 0, next = 0, bestH = -1;
    ale::Word best;
    float bestM = -1e9f;
    int misses = 0;
    // the transmission in progress
    ale::Assembler as;
    double txUtc = 0;
    int txVote = 0, txWords = 0;
    double txSnr = 0;
    // results
    std::vector<AleEvent> events;
    std::vector<AleStation> stations;
    std::vector<AleWordLog> words;
    std::vector<std::string> learnedNets;
    bool listLoad = false;               // a loaded file (mu): the feed thread takes it into the lists above
    AleTelemetry listIn;
    uint64_t wordsOk = 0, wordsMissed = 0;
    float lastSnr = 0, lastBer = 0;
    double lastWordUtc = -1e9;
    bool changed = true;
    int64_t nIn = 0;
    double nextReport = 0, power = 0;
    int64_t nPower = 0;

    void setTwiddles() {
        twAfc = afc;
        for (int k = 0; k < 8; k++) {
            const double f = ale::kTone0 + ale::kToneStep * k + afc;
            for (int n = 0; n < kSps; n++) { twc[k][n] = (float)std::cos(2 * kPi * f * n / kFs); tws[k][n] = (float)std::sin(2 * kPi * f * n / kFs); }
        }
    }

    double utcOf(int64_t sample) const { return t0 + (double)sample / kFs; }

    void resetState() {
        std::function<void(const std::string&)> cb;
        double off, st;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; cb = log; st = startReq; }
        running = curRate >= aleTuning().minSampleRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        usb.reset(); listen.reset();
        std::fill(std::begin(ring), std::end(ring), 0.f);
        na = 0; nh = 0;
        locked = acq = false; misses = 0; bestH = -1;
        as.finish();
        txWords = 0; txVote = 0; txSnr = 0;
        afc = 0; setTwiddles();
        spPow.assign(kSpecN, 0.f); spNext = kSpecN; spBlocks = 0; spLastWord = 0; spCand = 1e9;
        nIn = 0; nextReport = 0; power = 0; nPower = 0;
        t0 = st >= 0 ? st : nowUtc();
        { std::lock_guard<std::mutex> lk(mu); pub.inputRate = curRate; pub.timeMode = st >= 0 ? 1 : 0; }
        changed = true;
        if (cb && curRate > 0) cb(running ? "ALE: 8-FSK 125 baud words on the upper sideband, 750 to 2500 Hz above the dial"
                                          : "ALE: the sample rate is too low (250 kHz at least)");
    }

    // the eight tones over the last 64 samples
    void measure() {
        if (std::fabs(afc - twAfc) > 1.0) setTwiddles();
        float p[8];
        const int64_t s0 = na - kSps;
        for (int k = 0; k < 8; k++) {
            float re = 0, im = 0;
            for (int n = 0; n < kSps; n++) { const float v = ring[(s0 + n) & (kAudioN - 1)]; re += v * twc[k][n]; im -= v * tws[k][n]; }
            p[k] = re * re + im * im;
        }
        int b = 0;
        float tot = 0;
        for (int k = 0; k < 8; k++) { tot += p[k]; if (p[k] > p[b]) b = k; }
        const int i = (int)(nh & (kHopN - 1));
        hsym[i] = (uint8_t)ale::tribit(b);
        hq[i] = tot > 0 ? p[b] / tot : 0;
        hbest[i] = p[b];
        hother[i] = (tot - p[b]) / 7;
    }

    ale::Word candidate(int64_t h, float& metric) const {
        uint8_t bits[147];
        float q = 0;
        for (int s = 0; s < ale::kWordSymbols; s++) {
            const int i = (int)((h - (int64_t)(ale::kWordSymbols - 1 - s) * kHps) & (kHopN - 1));
            const int tb = hsym[i];
            bits[3 * s] = (uint8_t)((tb >> 2) & 1); bits[3 * s + 1] = (uint8_t)((tb >> 1) & 1); bits[3 * s + 2] = (uint8_t)(tb & 1);
            q += hq[i];
        }
        ale::Word w = ale::decodeBits(bits);
        metric = q - 1.5f * (float)(w.voteErrs + w.golayErrs) + (w.stuffOk ? 0.5f : 0.f);
        return w;
    }

    // the tone offset of a word: power at its tones plus d, d = -60 .. 60 Hz
    double offsetOf(int64_t h) const {
        constexpr int kD = 13;
        double e[kD] = {};
        for (int s = 0; s < ale::kWordSymbols; s++) {
            const int64_t hh = h - (int64_t)(ale::kWordSymbols - 1 - s) * kHps;
            const int k = ale::tone(hsym[hh & (kHopN - 1)]);
            const int64_t s0 = kSps + kHop * hh - kSps;   // first sample of that window
            if (na - s0 > kAudioN - 8) return 0;
            for (int d = 0; d < kD; d++) {
                const double f = ale::kTone0 + ale::kToneStep * k + afc + 10.0 * (d - 6);
                const std::complex<double> st = std::polar(1.0, -2 * kPi * f / kFs);
                std::complex<double> r(1, 0), acc(0, 0);
                for (int n = 0; n < kSps; n++) { acc += (double)ring[(s0 + n) & (kAudioN - 1)] * r; r *= st; }
                e[d] += std::norm(acc);
            }
        }
        int b = 0;
        for (int d = 1; d < kD; d++) if (e[d] > e[b]) b = d;
        double frac = 0;
        if (b > 0 && b < kD - 1) { const double den = e[b - 1] - 2 * e[b] + e[b + 1]; if (den < 0) frac = 0.5 * (e[b - 1] - e[b + 1]) / den; }
        return 10.0 * (b - 6 + std::max(-0.5, std::min(0.5, frac)));
    }

    void emit(const ale::Word& w, int64_t h) {
        const int64_t endSample = kSps + kHop * h;
        const double utc = utcOf(endSample) - ale::kTw;
        double sb = 0, so = 0;
        for (int s = 0; s < ale::kWordSymbols; s++) { const int i = (int)((h - (int64_t)s * kHps) & (kHopN - 1)); sb += hbest[i]; so += hother[i]; }
        const double snr = so > 0 ? std::max(1e-3, (sb - so) / so) * kBaudOverBw() : 1e-3;
        const float snrDb = (float)(10 * std::log10(snr));
        const double d = offsetOf(h);
        afc = std::max(-kSearchHz, std::min(kSearchHz, afc + 0.5 * d));
        spLastWord = na;
        if (as.empty()) { txUtc = utc; txVote = 0; txWords = 0; txSnr = 0; }
        as.add(w);
        txVote += w.voteErrs; txWords++; txSnr += snrDb;
        wordsOk++;
        lastSnr = snrDb; lastBer = (float)w.voteErrs / 147.f; lastWordUtc = utc;
        AleWordLog l;
        l.utc = utc; l.type = w.type; l.text = printable(w.text()); l.voteErrs = w.voteErrs; l.golayErrs = w.golayErrs;
        words.push_back(l);
        if (words.size() > kAleWordKeep) words.erase(words.begin(), words.begin() + (words.size() - kAleWordKeep));
        changed = true;
    }
    static double kBaudOverBw() { return (double)ale::kBaud / 3000.0; }

    bool isNet(const std::string& a) const {
        for (const auto& n : nets) if (n == a) return true;
        for (const auto& n : learnedNets) if (n == a) return true;
        return false;
    }

    void endTx() {
        locked = false; acq = false; misses = 0;
        if (as.empty()) return;
        const ale::Call c = as.finish();
        if (c.kind == ale::Call::Unknown) return;
        AleEvent e;
        e.utc = txUtc; e.dialHz = dialHz.load();
        std::function<void(const std::string&)> log;
        { std::lock_guard<std::mutex> lk(mu); log = this->log; }
        e.from = c.from;
        for (size_t i = 0; i < c.to.size(); i++) e.to += (i ? ", " : "") + c.to[i];
        for (size_t i = 0; i < c.thru.size(); i++) e.to += (e.to.empty() ? "" : ", ") + c.thru[i];
        e.tis = c.conclusion == ale::Tis;
        e.amd = c.amd;
        for (size_t i = 0; i < c.cmds.size(); i++) e.cmd += (i ? "; " : "") + c.cmds[i];
        e.words = txWords;
        e.ber = txWords ? (float)txVote / (147.f * (float)txWords) : 0.f;
        e.snrDb = txWords ? (float)(txSnr / txWords) : 0.f;
        e.cfoHz = (float)afc;
        e.type = ale::kindName(c.kind);
        if (c.kind == ale::Call::Individual && c.to.size() == 1 && !c.from.empty()) {
            const std::string& to = c.to[0];
            if (isNet(to)) e.type = "Net";
            else
                for (auto it = events.rbegin(); it != events.rend(); ++it) {
                    if (e.utc - it->utc > 20) break;   // answers come within seconds
                    if (it->from != to) continue;      // the called station's last transmission decides
                    if ((it->type == "Individual" || it->type == "Net") && it->to == c.from) { e.type = "Response"; break; }
                    if (it->type == "Response" && it->to == c.from) { e.type = "Ack"; break; }
                    if ((it->type == "Individual" || it->type == "Net") && it->to != c.from && it->to.find(',') == std::string::npos) {
                        // a station other than the one called answers: the call went to a net
                        e.type = "Net response";
                        if (it->type != "Net") {
                            it->type = "Net";
                            if (!isNet(it->to)) learnedNets.push_back(it->to);
                            if (log) log("ALE: " + it->to + " is a net (" + c.from + " answered " + it->from + ")");
                        }
                        break;
                    }
                    break;
                }
        }
        events.push_back(e);
        if (!e.from.empty() && e.dialHz > 0 && automation::wants(automation::FMsg)) {   // rules, integrations and the learned sounding schedules (insight/schedule_watch.h)
            automation::Event ex;
            ex.str("station", e.from).str("to", e.to).str("call_type", e.type);
            automation::emitMessage("ale", "ALE", e.dialHz, e.from, e.type + " " + e.from + (e.to.empty() ? "" : " to " + e.to), e.snrDb, std::move(ex));
        }
        if (!e.from.empty() && e.dialHz > 0) {   // the link-quality memory (ale_lqa.h): every call heard is a measurement of the path from that station
            AleLqaObs q; q.utc = e.utc; q.station = e.from; q.hz = e.dialHz; q.snrDb = e.snrDb; q.ber = e.ber; q.sounding = e.type == "Sounding";
            AleLqa::global().add(q);
        }
        if (events.size() > kAleKeep) events.erase(events.begin(), events.begin() + (events.size() - kAleKeep));
        if (!e.from.empty()) {
            auto st = std::find_if(stations.begin(), stations.end(), [&](const AleStation& s) { return s.address == e.from; });
            if (st == stations.end()) { stations.push_back(AleStation()); st = stations.end() - 1; st->address = e.from; }
            st->count++; st->lastUtc = e.utc; st->lastDialHz = e.dialHz; st->snrDb = e.snrDb; st->ber = e.ber; st->lastType = e.type;
            std::stable_sort(stations.begin(), stations.end(), [](const AleStation& a, const AleStation& b) { return a.lastUtc > b.lastUtc; });
        }
        if (log) {
            char b[300];
            snprintf(b, sizeof b, "ALE: %s %s%s%s%s%s", e.type.c_str(), e.from.c_str(), e.to.empty() ? "" : " -> ", e.to.c_str(), e.amd.empty() ? "" : "  AMD: ", e.amd.c_str());
            log(b);
        }
        changed = true;
    }

    void spectrum() {
        if (spRe.empty()) { spRe.resize(kSpecN); spIm.resize(kSpecN); if (spPow.empty()) spPow.assign(kSpecN, 0.f); }
        const int64_t s0 = na - kSpecN;
        for (int n = 0; n < kSpecN; n++) {
            spRe[(size_t)n] = ring[(s0 + n) & (kAudioN - 1)] * (float)(0.5 - 0.5 * std::cos(2 * kPi * n / kSpecN));
            spIm[(size_t)n] = 0;
        }
        fftSplit(spRe.data(), spIm.data(), kSpecLog2, false);
        const float a = spBlocks < 8 ? 1.f / (float)(spBlocks + 1) : 0.125f;
        for (int k = 0; k < kSpecN / 2; k++) spPow[(size_t)k] += a * (spRe[(size_t)k] * spRe[(size_t)k] + spIm[(size_t)k] * spIm[(size_t)k] - spPow[(size_t)k]);
        spBlocks++;
        if (locked || acq || spBlocks < 4 || (spBlocks & 3)) return;
        const double bin = kFs / kSpecN;
        auto band = [&](double f) { double e = 0; const int c = (int)std::lround(f / bin); for (int k = c - 4; k <= c + 4; k++) if (k > 0 && k < kSpecN / 2) e += spPow[(size_t)k]; return e; };
        double best = -1e300, bestD = 0, bestOut = 0;
        for (double d = -kSearchHz; d <= kSearchHz + 1e-9; d += bin) {
            double in = 0;
            for (int k = 0; k < 8; k++) in += band(ale::kTone0 + ale::kToneStep * k + d);
            const double out = 0.5 * (band(ale::kTone0 - ale::kToneStep + d) + band(ale::kTone0 + ale::kToneStep * 8 + d));
            const double sc = in / 8 - out;
            if (sc > best) { best = sc; bestD = d; bestOut = out; }
        }
        // only a clear error, seen twice, with no word for 2.5 s: within +-60 Hz the words decode and the AFC after each word does better,
        // and acting on single noisy estimates cost 1.5 dB at the threshold
        const bool cand = best > 0.5 * bestOut && std::fabs(bestD - afc) > 60;
        const bool quiet = (double)(na - spLastWord) > 2.5 * kFs;
        if (cand && quiet && std::fabs(bestD - spCand) < 16) { afc = bestD; spCand = 1e9; }
        else spCand = cand ? bestD : 1e9;
    }

    void step() {   // one instant: the tones, then the word clock
        measure();
        const int64_t h = nh++;
        if (h < (int64_t)(ale::kWordSymbols - 1) * kHps) return;
        float m = 0;
        if (!locked) {
            const ale::Word w = candidate(h, m);
            const bool good = w.ok && w.voteErrs <= kAcqVote && w.golayErrs <= kAcqGolay && w.stuffOk && plausible(w);
            if (good && (!acq || m > bestM)) { if (!acq) acqEnd = h + kHps; acq = true; best = w; bestM = m; bestH = h; }
            if (acq && h >= acqEnd) {
                acq = false; locked = true; misses = 0;
                emit(best, bestH);
                next = bestH + kWordHops;
                bestH = -1;
            }
            return;
        }
        if (h >= next - kTrack && h <= next + kTrack) {
            const ale::Word w = candidate(h, m);
            if (w.ok && w.voteErrs <= kTrackVote && plausible(w) && (bestH < 0 || m > bestM)) { best = w; bestM = m; bestH = h; }
        }
        if (h == next + kTrack) {
            if (bestH >= 0) { emit(best, bestH); next = bestH + kWordHops; misses = 0; }
            else {
                wordsMissed++; changed = true;
                next += kWordHops;
                if (++misses >= 2) endTx();
            }
            bestH = -1;
        }
    }

    void process(const float* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            ring[na & (kAudioN - 1)] = x[i];
            na++;
            if (na >= spNext) { spNext += kSpecN / 2; spectrum(); }
            if (na >= kSps && (na & (kHop - 1)) == 0) step();
        }
    }

    void report() {
        const double secs = nIn / std::max(1.0, curRate);
        std::lock_guard<std::mutex> lk(mu);
        pub.seq++;
        pub.timeSec = secs;
        pub.nowUtc = utcOf(na);
        pub.levelDb = nPower ? (float)(10 * std::log10(power / nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        pub.state = !running ? 0 : locked ? 2 : 1;
        pub.cfoHz = afc;
        pub.snrDb = lastSnr; pub.ber = lastBer;
        pub.dataValid = pub.nowUtc - lastWordUtc < 60;
        pub.blocksOk = wordsOk; pub.blocksBad = wordsMissed;
        pub.dialHz = dialHz.load();
        if (changed) { pub.events = events; pub.stations = stations; pub.words = words; changed = false; }
    }
};

AleReceiver::AleReceiver() : p_(std::make_unique<Impl>()) {}
AleReceiver::~AleReceiver() = default;

void AleReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void AleReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
void AleReceiver::setDialHz(double hz) { p_->dialHz = hz; }
bool AleReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= aleTuning().minSampleRate - 1; }
void AleReceiver::reset() { p_->resetReq = true; }

void AleReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.resetState(); }
    if (m.offsetReq.exchange(false) && m.running) { std::lock_guard<std::mutex> lk(m.mu); m.front.setOffsetHz(m.offsetHz); }
    if (m.clearReq.exchange(false)) { m.events.clear(); m.stations.clear(); m.words.clear(); m.learnedNets.clear(); m.changed = true; }
    {
        std::lock_guard<std::mutex> lk(m.mu);
        if (m.listLoad) {
            m.events = std::move(m.listIn.events); m.stations = std::move(m.listIn.stations); m.words = std::move(m.listIn.words);
            m.listIn = AleTelemetry(); m.listLoad = false; m.changed = true;
        }
    }
    if (m.curRate <= 0) return;
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
    }
    m.nPower += (int64_t)n;
    m.nIn += (int64_t)n;
    if (m.running) {
        m.base.clear();
        m.front.process(x, n, m.base);
        for (cf32& v : m.base) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
        m.aud.clear();
        double pw = 0;
        if (const int d = hfDemodResolve(m.demodSet.load(), 0); d != m.usb.demod()) m.usb.setDemod(d);
        m.usb.process(m.base.data(), m.base.size(), m.aud, pw);
        m.listen.play(m.aud.data(), m.aud.size());   // a copy to the sound card only while listening
        m.process(m.aud.data(), m.aud.size());
    }
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        m.report();
    }
}

bool AleReceiver::telemetry(AleTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void AleReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void AleReceiver::setStartTime(double utc) { std::lock_guard<std::mutex> lk(p_->mu); p_->startReq = utc; p_->resetReq = true; }
void AleReceiver::setNetAddresses(const std::vector<std::string>& nets) { std::lock_guard<std::mutex> lk(p_->mu); p_->nets = nets; }
void AleReceiver::clearLog() {
    p_->clearReq = true;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->listLoad = false;
    p_->pub.events.clear(); p_->pub.stations.clear(); p_->pub.words.clear();
    p_->pub.seq++;
}
// the words, events and stations already stay through reset() (their times are UTC): only Clear empties them
void AleReceiver::clearResults() { clearLog(); }
std::string AleReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.words, p_->pub.events, p_->pub.stations);
}
bool AleReceiver::loadResults(const std::string& blob) {
    AleTelemetry l;
    if (!results::unpack(blob, l.words, l.events, l.stations)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pub.words = l.words; p_->pub.events = l.events; p_->pub.stations = l.stations;
    p_->listIn = std::move(l); p_->listLoad = true;
    p_->pub.seq++;
    return true;
}
void AleReceiver::flush() {
    Impl& m = *p_;
    if (!m.running) return;
    m.endTx();
    m.report();
}
void AleReceiver::setListen(bool on) { p_->listen.on = on; }
void AleReceiver::setDemod(int d) { p_->demodSet = d; }
int AleReceiver::demod() const { return p_->demodSet.load(); }
bool AleReceiver::listening() const { return p_->listen.on.load(); }
void AleReceiver::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void AleReceiver::setMuted(bool m) { p_->listen.muted = m; }
void AleReceiver::setSilent(bool s) { p_->listen.silent = s; }
void AleReceiver::setListenTap(std::function<void(const float* x, size_t n)> cb) { p_->listen.setTap(std::move(cb)); }

ModeTuning aleTuning() {
    ModeTuning t;
    t.stdMode = 51; t.id = "ale"; t.name = "HF ALE";
    t.minMhz = 1.5; t.maxMhz = 30; t.defMhz = 14.109;   // the 20 m HFN / amateur ALE channel (upper sideband dial)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.003;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;                              // as HF digital: the radio sits 20 kHz above the dial, away from its DC spike
    return t;
}

const AlePreset* alePresets(int& count) {
    static const AlePreset kP[] = {{"80 m", 3.596}, {"40 m", 7.102}, {"30 m", 10.145}, {"20 m", 14.109}, {"17 m", 18.106}, {"15 m", 21.096}, {"12 m", 24.926}, {"10 m", 28.146}};
    count = (int)(sizeof kP / sizeof *kP);
    return kP;
}

} // namespace dect2
