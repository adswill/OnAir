#include "dect2/source/diversity_source.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>

namespace dect2 {

namespace {
int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::mutex gMu;
std::map<std::string, DiversitySource*> gLive;
constexpr size_t kAlignN = 1 << 17;          // samples of each radio the start-up alignment looks at
}

DiversitySource::DiversitySource(SynthGroupDef def, std::vector<DeviceInfo> members, ChildFactory factory)
    : def_(std::move(def)), factory_(std::move(factory)) {
    for (auto& m : members) { Child c; c.dev = m; ch_.push_back(std::move(c)); }
    mode_ = def_.diversityMode == 0 ? 0 : 1;
    rate_ = groupChildRate(members, def_.childRateHz);
    usbWarn_ = usbLoadWarning(members, rate_);
}

DiversitySource::~DiversitySource() { stop(); }

TuneSettings DiversitySource::childTune() const {
    TuneSettings t = tune_;
    t.sampleRate = rate_;
    return t;
}

bool DiversitySource::startChild(int i, std::string& err) {
    Child& c = ch_[(size_t)i];
    c.src = factory_ ? factory_(c.dev) : makeSource(c.dev);
    if (!c.src) { c.error = "no driver for " + c.dev.name; err = c.error; return false; }
    if (!c.ring) c.ring = std::make_unique<IqRing>(1u << 22);   // 4 M samples: the start-up alignment needs a second or so of both radios in hand
    c.ring->clear();
    std::string e;
    if (!c.src->start(childTune(), *c.ring, e)) { c.error = e.empty() ? "did not start" : e; c.src.reset(); err = c.error; return false; }
    const double r = c.src->sampleRate();
    // a driver rounds the rate a little (an RTL-SDR's divider): up to 50 ppm is carried by the drift tracking, more is a different rate
    if (std::fabs(r - rate_) > 50e-6 * rate_) {
        char b[200];
        snprintf(b, sizeof b, "runs at %.4f Msps instead of %.4f: left out", r / 1e6, rate_ / 1e6);
        c.error = b; c.src->stop(); c.src.reset(); err = c.error; return false;
    }
    c.error.clear();
    c.running = true;
    c.dc = cf32(0, 0); c.q.clear();
    c.lastNs = nowNs(); c.rateT0 = c.lastNs; c.rateN0 = 0; c.got = 0; c.deliveredPct = 100;
    return true;
}

void DiversitySource::stopChild(int i) {
    Child& c = ch_[(size_t)i];
    if (c.src) { c.src->stop(); c.src.reset(); }
    c.running = false;
}

bool DiversitySource::start(const TuneSettings& s, IqRing& ring, std::string& err) {
    stop();
    if (ch_.size() != 2 || !(rate_ > 0)) { err = "diversity pair " + def_.name + ": needs two radios with a common sample rate"; return false; }
    if (s.sampleRate > 0) rate_ = s.sampleRate;   // the mode asks for its own rate; both radios run it
    out_ = &ring;
    tune_ = s;
    int ok = 0;
    std::string notes;
    for (int i = 0; i < 2; i++) {
        std::string e;
        if (startChild(i, e)) ok++;
        else addNote(notes, ch_[(size_t)i].dev.name + ": " + ch_[(size_t)i].error);
    }
    if (ok == 0) { err = "diversity pair " + def_.name + ": neither radio started" + (notes.empty() ? "" : " (" + notes + ")"); return false; }
    if (ok == 1) addNote(notes, "only one radio of the pair runs: no diversity, the other one passes through unchanged");
    err = notes;
    if (!usbWarn_.empty()) addNote(err, usbWarn_);
    DiversityConfig cfg;
    cfg.rateHz = rate_;
    cfg.mode = mode_ == 0 ? DiversityConfig::Selection : DiversityConfig::Mrc;
    comb_.configure(cfg);
    aligned_ = false; combining_ = false; realign_ = true;
    state_ = ok == 2 ? "starting: waiting for both radios" : "one radio only";
    run_ = true;
    th_ = std::thread([this] { worker(); });
    { std::lock_guard<std::mutex> lk(gMu); gLive[def_.name] = this; }
    return true;
}

void DiversitySource::stop() {
    {
        std::lock_guard<std::mutex> lk(gMu);
        auto it = gLive.find(def_.name);
        if (it != gLive.end() && it->second == this) gLive.erase(it);
    }
    run_ = false;
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < 2; i++) stopChild(i);
}

bool DiversitySource::retune(const TuneSettings& s, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    const bool moved = std::fabs(s.centerHz - tune_.centerHz) > 1.0 || std::fabs(s.sampleRate - tune_.sampleRate) > 1.0;
    tune_ = s;
    bool all = true;
    for (int i = 0; i < 2; i++) {
        Child& c = ch_[(size_t)i];
        if (!c.running || !c.src) continue;
        std::string e;
        if (!c.src->retune(childTune(), e)) { all = false; addNote(err, c.dev.name + ": " + e); }
    }
    // a new frequency: the radios' frequency errors (ppm x frequency) and phases are new, the alignment starts again
    if (moved) realign_ = true;
    return all;
}

void DiversitySource::setMode(int mode) { mode_ = mode == 0 ? 0 : 1; }
void DiversitySource::realign() { realign_ = true; }

std::string DiversitySource::statusNote() const {
    std::lock_guard<std::mutex> lk(mu_);
    return combining_ ? "" : state_;
}

void DiversitySource::worker() {
    std::vector<cf32> buf(1 << 15), outv;
    int alignTries = 0;
    int64_t warmUntil = nowNs() + 300000000LL, lastLockNs = nowNs();
    while (run_) {
        bool any = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const int64_t now = nowNs();
            for (int i = 0; i < 2; i++) {
                Child& c = ch_[(size_t)i];
                if (!c.running || !c.ring) continue;
                for (int guard = 0; guard < 8; guard++) {
                    const size_t n = c.ring->read(buf.data(), buf.size());
                    if (n == 0) break;
                    any = true;
                    // the radio's DC spike (an RTL-SDR's sits in the middle of the band): a slow running mean taken out
                    cf32 dc = c.dc;
                    for (size_t k = 0; k < n; k++) { dc += (buf[k] - dc) * 2e-5f; buf[k] -= dc; }
                    c.dc = dc;
                    c.q.insert(c.q.end(), buf.begin(), buf.begin() + (long)n);
                    c.got += n; c.lastNs = now;
                    if (n < buf.size()) break;
                }
                if (now - c.rateT0 > 2000000000LL) {
                    c.deliveredPct = 100.0 * (double)(c.got - c.rateN0) / ((now - c.rateT0) * 1e-9 * rate_);
                    c.rateT0 = now; c.rateN0 = c.got;
                }
                // a radio that has stopped delivering for 2 s is left out until it comes back
                if (c.running && now - c.lastNs > 2000000000LL && c.q.empty()) { c.error = "no samples for 2 s"; }
            }
            const bool aOn = ch_[0].running && now - ch_[0].lastNs < 2000000000LL, bOn = ch_[1].running && now - ch_[1].lastNs < 2000000000LL;
            outv.clear();
            if (aOn && bOn) {
                if (realign_) {
                    // start-up alignment: wait out the radios' settling, then look at the heads of both queues
                    aligned_ = false; combining_ = false;
                    comb_.reset();
                    state_ = "measuring the delay between the two radios";
                    if (now > warmUntil && ch_[0].q.size() >= kAlignN && ch_[1].q.size() >= kAlignN) {
                        const int maxSkew = (int)std::min<size_t>(kAlignN / 2 - 1, (size_t)(0.1 * rate_) + 1000);
                        const DiversityAlign al = diversityCoarseAlign(ch_[0].q.data(), ch_[1].q.data(), kAlignN, rate_, 40e3, maxSkew);
                        if (al.ok) {
                            // B lags A by skew: drop that many samples from the head of B (A when negative)
                            Child& lead = al.skewSamples >= 0 ? ch_[1] : ch_[0];
                            const size_t drop = (size_t)std::abs(al.skewSamples);
                            lead.q.erase(lead.q.begin(), lead.q.begin() + (long)std::min(drop, lead.q.size()));
                            aligned_ = true; realign_ = false; alignTries = 0;
                            state_ = "aligned, locking on the signal";
                        } else if (++alignTries >= 8) {
                            // no signal both radios hear (yet): the combiner starts anyway; it passes radio A until it finds one
                            realign_ = false; alignTries = 0;
                            state_ = "no common signal found yet: radio A passes through";
                        } else {
                            // try another stretch of the stream
                            for (int i = 0; i < 2; i++) ch_[(size_t)i].q.erase(ch_[(size_t)i].q.begin(), ch_[(size_t)i].q.begin() + (long)(kAlignN / 2));
                        }
                    }
                    // while waiting, keep the queues bounded (the radios deliver continuously)
                    for (int i = 0; i < 2; i++) {
                        auto& q = ch_[(size_t)i].q;
                        if (q.size() > 3 * kAlignN) q.erase(q.begin(), q.begin() + (long)(q.size() - 2 * kAlignN));
                    }
                }
                if (!realign_) {
                    const size_t n = std::min(ch_[0].q.size(), ch_[1].q.size());
                    if (n > 0) {
                        comb_.setMode(mode_ == 0 ? DiversityConfig::Selection : DiversityConfig::Mrc);
                        comb_.push(ch_[0].q.data(), ch_[1].q.data(), n);
                        comb_.pull(outv);
                        for (int i = 0; i < 2; i++) ch_[(size_t)i].q.erase(ch_[(size_t)i].q.begin(), ch_[(size_t)i].q.begin() + (long)n);
                        const DiversityStatus ds = comb_.status();
                        combining_ = ds.locked;
                        state_ = ds.locked ? "combining" : (aligned_ ? "aligned, locking on the signal" : "no common signal found yet: radio A passes through");
                    }
                    // one queue runs far ahead of the other (a stalled USB link): the pairing of samples is no longer right, measure it again
                    for (int i = 0; i < 2; i++) {
                        auto& q = ch_[(size_t)i].q;
                        if (q.size() > 4 * kAlignN) { q.erase(q.begin(), q.begin() + (long)(q.size() - kAlignN)); realign_ = true; alignTries = 0; }
                    }
                    // not locked for a while (a new signal, a retune the radios did not report, lost samples): look again, at most every 5 s
                    if (combining_) lastLockNs = now;
                    else if (now - lastLockNs > 5000000000LL) { realign_ = true; alignTries = 0; lastLockNs = now; }
                }
            } else if (aOn || bOn) {
                // one radio only: its samples pass through unchanged
                combining_ = false; aligned_ = false; realign_ = true; alignTries = 0; warmUntil = now + 300000000LL;
                Child& c = ch_[aOn ? 0 : 1];
                outv.swap(c.q);
                state_ = "one radio only";
            }
        }
        if (!outv.empty() && out_) out_->write(outv.data(), outv.size());
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

DiversitySourceStatus DiversitySource::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    DiversitySourceStatus s;
    s.running = run_;
    s.name = def_.name;
    s.combining = combining_;
    s.aligned = aligned_;
    s.state = state_;
    s.mode = mode_;
    s.rateHz = rate_;
    s.dsp = comb_.status();
    s.usbWarning = usbWarn_;
    const int64_t now = nowNs();
    for (const auto& c : ch_) {
        DiversityMemberStatus m;
        m.name = c.dev.name;
        m.running = c.running && now - c.lastNs < 1000000000LL;
        m.deliveredPct = c.deliveredPct;
        m.error = c.error;
        s.members.push_back(m);
    }
    return s;
}

bool diversityStatus(const std::string& name, DiversitySourceStatus& out) {
    std::lock_guard<std::mutex> lk(gMu);
    auto it = gLive.find(name);
    if (it == gLive.end()) return false;
    out = it->second->status();
    return true;
}
bool diversitySetMode(const std::string& name, int mode) {
    std::lock_guard<std::mutex> lk(gMu);
    auto it = gLive.find(name);
    if (it == gLive.end()) return false;
    it->second->setMode(mode);
    return true;
}
bool diversityRealign(const std::string& name) {
    std::lock_guard<std::mutex> lk(gMu);
    auto it = gLive.find(name);
    if (it == gLive.end()) return false;
    it->second->realign();
    return true;
}

std::unique_ptr<IqSource> makeDiversitySource(const SynthGroupDef& def, const std::vector<DeviceInfo>& members) {
    if (members.size() != 2) return nullptr;
    return std::make_unique<DiversitySource>(def, members);
}

} // namespace dect2
