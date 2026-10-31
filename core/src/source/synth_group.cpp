#include "dect2/source/synth_group.h"
#include "dect2/source/diversity_source.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>

namespace dect2 {

namespace {
int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
bool isRtl(const DeviceInfo& d) { const std::string b = lower(d.board + " " + d.name); return b.find("rtl") != std::string::npos; }
}

SlicePlan planSlices(int radios, double fs, double wantSpanHz, double usable) {
    SlicePlan p;
    if (radios < 2 || radios > synthwide::kSynthMaxSlices) { p.why = "a group needs 2 to 5 radios"; return p; }
    if (!(fs > 10e3)) { p.why = "the radios have no common sample rate"; return p; }
    // FFT size: a hop (n/2 samples) of about 0.2 ms, so the per-hop phase tracking follows free-running oscillators
    int n = 256;
    while (n < 16384 && (double)n * 1.41 < fs * 4.3e-4) n *= 2;   // the power of 2 nearest to 0.43 ms of samples (1024 at 2.4 Msps)
    const double df = fs / n;
    int U = (int)(usable * n) & ~1;
    const int minOv = std::max(64, n / 16);                  // the overlap the alignment needs (150 kHz at 2.4 Msps)
    const int maxStep = (U - minOv) & ~1;
    const double spanBins = wantSpanHz / df;
    int step = (int)std::floor((spanBins - U) / (radios - 1));
    step &= ~1;
    if (step > maxStep) {
        step = maxStep;
        char b[200];
        snprintf(b, sizeof b, "%d radios at %.3g Msps reach %.3g MHz with a safe overlap, not %.3g MHz", radios, fs / 1e6,
                 ((radios - 1) * step + U) * df / 1e6, wantSpanHz / 1e6);
        p.why = b;
    }
    step = std::max(step, std::max(2, U / 4) & ~1);           // asking for less than one radio covers: the slices still step apart
    synthwide::SynthCfg c;
    c.slices = radios; c.fs = fs; c.n = n; c.stepBins = step; c.usable = usable;
    c.alignFrames = std::max(24, (int)std::lround(0.03 * fs / (n / 2)));   // about 30 ms per alignment solve
    c.maxOffsetHz = std::min(60e3, 0.25 * (U - step) * df);
    synthwide::SynthWideband probe;
    if (!probe.configure(c)) { p.why = "no valid slice layout for this rate and span"; return p; }
    p.ok = true;
    p.cfg = c;
    for (int i = 0; i < radios; i++) p.sliceOffsetHz.push_back(probe.sliceCentreHz(i));
    p.overlapHz = probe.overlapHz();
    p.spanHz = probe.spanHz();
    p.maxSpanHz = ((radios - 1) * maxStep + U) * df;
    p.outRateHz = probe.outRate();
    return p;
}

// ---- saved groups ----
namespace {
std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '%' || c == ';' || c == '|' || c == ',' || c == '\n' || c == '=' || c < 32) { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
        else o += (char)c;
    }
    return o;
}
std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) { o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else o += s[i];
    }
    return o;
}
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v; std::string cur;
    for (char c : s) { if (c == sep) { v.push_back(cur); cur.clear(); } else cur += c; }
    v.push_back(cur);
    return v;
}
std::mutex gMu;
std::vector<SynthGroupDef> gGroups;
std::map<std::string, std::vector<DeviceInfo>> gFound;      // group name -> members as found by the last listing
std::map<std::string, SyntheticWideSource*> gLive;          // group name -> the running source
}

// name;center;span;rate;board,serial,name|board,serial,name...   one group per line
std::string synthGroupsToString(const std::vector<SynthGroupDef>& v) {
    std::string o;
    for (const auto& g : v) {
        char b[120];
        snprintf(b, sizeof b, ";%.0f;%.0f;%.0f;", g.centerHz, g.spanHz, g.childRateHz);
        o += esc(g.name) + b;
        for (size_t i = 0; i < g.members.size(); i++) {
            if (i) o += "|";
            o += esc(g.members[i].board) + "," + esc(g.members[i].serial) + "," + esc(g.members[i].name);
        }
        if (g.diversity) o += ";d" + std::to_string(g.diversityMode);   // optional 6th field, absent in groups saved before diversity pairs existed
        o += "\n";
    }
    return o;
}

std::vector<SynthGroupDef> synthGroupsFromString(const std::string& s) {
    std::vector<SynthGroupDef> v;
    for (const auto& line : split(s, '\n')) {
        const auto f = split(line, ';');
        if (f.size() < 5 || f[0].empty()) continue;
        SynthGroupDef g;
        g.name = unesc(f[0]);
        g.centerHz = atof(f[1].c_str()); g.spanHz = atof(f[2].c_str()); g.childRateHz = atof(f[3].c_str());
        if (!f[4].empty())
            for (const auto& m : split(f[4], '|')) {
                const auto mf = split(m, ',');
                if (mf.size() != 3) continue;
                g.members.push_back({unesc(mf[0]), unesc(mf[1]), unesc(mf[2])});
            }
        if (f.size() >= 6 && f[5].size() >= 2 && f[5][0] == 'd') { g.diversity = true; g.diversityMode = atoi(f[5].c_str() + 1) == 0 ? 0 : 1; }
        if (g.diversity) { if (g.members.size() == 2) v.push_back(g); continue; }
        if (g.members.size() >= 2 && g.members.size() <= (size_t)synthwide::kSynthMaxSlices && g.spanHz > 0) v.push_back(g);
    }
    return v;
}

void setSynthGroups(const std::vector<SynthGroupDef>& v) { std::lock_guard<std::mutex> lk(gMu); gGroups = v; }
std::vector<SynthGroupDef> synthGroups() { std::lock_guard<std::mutex> lk(gMu); return gGroups; }

bool memberMatches(const SynthGroupMember& m, const DeviceInfo& d) {
    if (d.kind == DeviceInfo::Group) return false;
    if (!m.serial.empty()) return d.serial == m.serial && lower(d.board) == lower(m.board);
    return d.name == m.name;
}
SynthGroupMember memberOf(const DeviceInfo& d) { return {d.board, d.serial, d.name}; }

double groupChildRate(const std::vector<DeviceInfo>& members, double wanted) {
    if (members.empty()) return 0;
    double hi = 1e12, lo = 0;
    bool allRtl = true;
    for (const auto& d : members) {
        const double mx = d.steadyRateHz > 0 ? d.steadyRateHz : d.maxRateHz > 0 ? d.maxRateHz : isRtl(d) ? 2.4e6 : 2e6;
        hi = std::min(hi, mx);
        lo = std::max(lo, d.minRateHz);
        allRtl &= isRtl(d);
    }
    double r = wanted > 0 ? wanted : allRtl ? 2.4e6 : std::min(hi, 10e6);
    if (allRtl) r = std::min(r, 2.56e6);   // above 2.56 Msps an RTL-SDR loses samples on most hosts
    r = std::min(r, hi);
    if (r < lo) return 0;
    return r;
}

std::vector<DeviceInfo> synthGroupDevices(const std::vector<DeviceInfo>& found) {
    std::vector<DeviceInfo> out;
    std::lock_guard<std::mutex> lk(gMu);
    for (const auto& g : gGroups) {
        std::vector<DeviceInfo> mem;
        for (const auto& m : g.members)
            for (const auto& d : found) if (d.isRadio() && memberMatches(m, d)) { mem.push_back(d); break; }
        if (mem.size() < 2) continue;
        gFound[g.name] = mem;
        DeviceInfo d;
        d.kind = DeviceInfo::Group;
        if (g.diversity) {   // two radios on one frequency: the list entry runs at the members' common rate (diversity_source.h)
            if (mem.size() != 2) continue;
            const double fs = groupChildRate(mem, g.childRateHz);
            if (!(fs > 0)) continue;
            d.board = "diversitygroup";
            d.serial = "group:" + g.name;
            d.nativeArgs = g.name;
            d.name = "Diversity: " + g.name + " (2 radios)";
            double lo = 0;
            for (const auto& m : mem) lo = std::max(lo, m.minRateHz);
            if (lo <= 0) lo = 225e3;
            d.minRateHz = std::min(lo, fs); d.maxRateHz = fs;   // any rate both radios reach: the mode asks for its own
            d.rateRanges = {{d.minRateHz, d.maxRateHz}};
            d.gainMinDb = 1e9; d.gainMaxDb = -1e9;
            for (const auto& m : mem) {
                d.gainMinDb = std::min(d.gainMinDb, m.gainMinDb); d.gainMaxDb = std::max(d.gainMaxDb, m.gainMaxDb);
                d.minFreqHz = std::max(d.minFreqHz, m.minFreqHz);
                d.maxFreqHz = d.maxFreqHz == 0 ? m.maxFreqHz : m.maxFreqHz == 0 ? d.maxFreqHz : std::min(d.maxFreqHz, m.maxFreqHz);
            }
            if (d.gainMinDb > d.gainMaxDb) d.gainMinDb = d.gainMaxDb = 0;
            d.settings.push_back(ppmSetting(1.0));
            out.push_back(d);
            continue;
        }
        d.board = "synthgroup";
        d.serial = "group:" + g.name;
        d.nativeArgs = g.name;
        d.name = "Group: " + g.name + " (" + std::to_string(mem.size()) + (mem.size() < g.members.size() ? " of " + std::to_string(g.members.size()) : std::string()) + " radios)";
        const double fs = groupChildRate(mem, g.childRateHz);
        const SlicePlan p = planSlices((int)mem.size(), fs, g.spanHz);
        if (!p.ok) continue;
        d.maxRateHz = d.minRateHz = p.outRateHz;
        d.rateRanges = {{p.outRateHz, p.outRateHz}};
        d.gainMinDb = 1e9; d.gainMaxDb = -1e9;
        for (const auto& m : mem) {
            d.gainMinDb = std::min(d.gainMinDb, m.gainMinDb); d.gainMaxDb = std::max(d.gainMaxDb, m.gainMaxDb);
            d.minFreqHz = std::max(d.minFreqHz, m.minFreqHz);
            d.maxFreqHz = d.maxFreqHz == 0 ? m.maxFreqHz : m.maxFreqHz == 0 ? d.maxFreqHz : std::min(d.maxFreqHz, m.maxFreqHz);
        }
        if (d.gainMinDb > d.gainMaxDb) d.gainMinDb = d.gainMaxDb = 0;
        d.settings.push_back(ppmSetting(1.0));
        out.push_back(d);
    }
    return out;
}

std::vector<DeviceInfo> synthGroupMembers(const DeviceInfo& group) {
    std::lock_guard<std::mutex> lk(gMu);
    auto it = gFound.find(group.nativeArgs);
    return it == gFound.end() ? std::vector<DeviceInfo>() : it->second;
}

double usbBytesPerSec(const DeviceInfo& d, double rate) {
    if (d.isNetwork()) return 0;
    const std::string b = lower(d.board);
    const double bytes = isRtl(d) || d.kind == DeviceInfo::HackRF || b.find("hackrf") != std::string::npos ? 2 : b.find("airspy") != std::string::npos ? 4 : 4;
    return rate * bytes;
}

std::string usbLoadWarning(const std::vector<DeviceInfo>& members, double rate) {
    double tot = 0; int usb = 0, rtl = 0;
    for (const auto& d : members) { const double b = usbBytesPerSec(d, rate); tot += b; if (b > 0) usb++; if (isRtl(d)) rtl++; }
    std::string w;
    char buf[300];
    if (tot > 30e6) {
        snprintf(buf, sizeof buf, "the radios send %.0f MB/s together: more than one USB 2 hub carries reliably (about 30-35 MB/s). Spread them over several USB ports/controllers or lower the rate.", tot / 1e6);
        w = buf;
    }
    if (rtl >= 3) {
        if (!w.empty()) w += " ";
        w += "Several dongles on one hub: use a powered hub (each RTL-SDR draws up to about 300 mA and gets hot), and short USB extension cables so the dongles do not touch.";
    }
    (void)usb;
    return w;
}

// ---- the source ----

SyntheticWideSource::SyntheticWideSource(SynthGroupDef def, std::vector<DeviceInfo> members, ChildFactory factory)
    : def_(std::move(def)), factory_(std::move(factory)) {
    for (auto& m : members) { Child c; c.dev = m; ch_.push_back(std::move(c)); }
    const double fs = groupChildRate(members, def_.childRateHz);
    plan_ = planSlices((int)ch_.size(), fs, def_.spanHz);
    usbWarn_ = usbLoadWarning(members, fs);
}

SyntheticWideSource::~SyntheticWideSource() { stop(); }

TuneSettings SyntheticWideSource::childTune(int i) const {
    TuneSettings t = tune_;
    t.centerHz = tune_.centerHz + plan_.sliceOffsetHz[(size_t)i];
    t.sampleRate = plan_.cfg.fs;
    t.basebandFilterHz = 0;
    t.bandwidthMhz = plan_.cfg.fs / 1e6;
    return t;
}

bool SyntheticWideSource::startChild(int i, std::string& err) {
    Child& c = ch_[(size_t)i];
    c.src = factory_ ? factory_(c.dev) : makeSource(c.dev);
    if (!c.src) { c.error = "no driver for " + c.dev.name; err = c.error; return false; }
    if (!c.ring) c.ring = std::make_unique<IqRing>(1u << 20);
    c.ring->clear();
    std::string e;
    if (!c.src->start(childTune(i), *c.ring, e)) { c.error = e.empty() ? "did not start" : e; c.src.reset(); err = c.error; return false; }
    const double r = c.src->sampleRate();
    // drivers round the rate a little (an RTL-SDR's divider): up to 50 ppm the drift correction takes it; more would be a different rate
    if (std::fabs(r - plan_.cfg.fs) > 50e-6 * plan_.cfg.fs) {
        char b[200];
        snprintf(b, sizeof b, "runs at %.4f Msps instead of %.4f: left out", r / 1e6, plan_.cfg.fs / 1e6);
        c.error = b; c.src->stop(); c.src.reset(); err = c.error; return false;
    }
    c.error.clear();
    c.running = true;
    c.dc = cf32(0, 0);
    c.lastNs = nowNs(); c.rateT0 = c.lastNs; c.rateN0 = 0; c.got = 0; c.deliveredPct = 100;
    return true;
}

void SyntheticWideSource::stopChild(int i) {
    Child& c = ch_[(size_t)i];
    if (c.src) { c.src->stop(); c.src.reset(); }
    c.running = false;
}

bool SyntheticWideSource::start(const TuneSettings& s, IqRing& ring, std::string& err) {
    stop();
    if (!plan_.ok) { err = "radio group " + def_.name + ": " + plan_.why; return false; }
    if (!sw_.configure(plan_.cfg)) { err = "radio group " + def_.name + ": bad slice layout"; return false; }
    out_ = &ring;
    tune_ = s;
    int ok = 0;
    std::string notes;
    for (int i = 0; i < (int)ch_.size(); i++) {
        std::string e;
        if (ch_[(size_t)i].enabled && startChild(i, e)) ok++;
        else {
            if (!ch_[(size_t)i].enabled) ch_[(size_t)i].error = "switched off";
            sw_.setAlive(i, false);
            addNote(notes, ch_[(size_t)i].dev.name + ": " + ch_[(size_t)i].error);
        }
    }
    if (ok < 2) {
        for (int i = 0; i < (int)ch_.size(); i++) stopChild(i);
        err = "radio group " + def_.name + ": fewer than 2 radios started" + (notes.empty() ? "" : " (" + notes + ")");
        return false;
    }
    err = notes;
    if (!usbWarn_.empty()) addNote(err, usbWarn_);
    run_ = true;
    th_ = std::thread([this] { worker(); });
    { std::lock_guard<std::mutex> lk(gMu); gLive[def_.name] = this; }
    return true;
}

void SyntheticWideSource::stop() {
    {
        std::lock_guard<std::mutex> lk(gMu);
        auto it = gLive.find(def_.name);
        if (it != gLive.end() && it->second == this) gLive.erase(it);
    }
    run_ = false;
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < (int)ch_.size(); i++) stopChild(i);
}

bool SyntheticWideSource::retune(const TuneSettings& s, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    const bool moved = std::fabs(s.centerHz - tune_.centerHz) > 1.0;
    tune_ = s;
    bool all = true;
    for (int i = 0; i < (int)ch_.size(); i++) {
        Child& c = ch_[(size_t)i];
        if (!c.running || !c.src) continue;
        std::string e;
        if (!c.src->retune(childTune(i), e)) { all = false; addNote(err, c.dev.name + ": " + e); }
    }
    // a new frequency: the frequency errors (ppm x frequency) and the oscillator phases are new, the alignment starts again
    if (moved) sw_.recalibrate();
    return all;
}

void SyntheticWideSource::recalibrate() { std::lock_guard<std::mutex> lk(mu_); sw_.recalibrate(); }

bool SyntheticWideSource::setMemberEnabled(int i, bool enabled, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (i < 0 || i >= (int)ch_.size()) return false;
    Child& c = ch_[(size_t)i];
    c.enabled = enabled;
    if (!enabled) {
        stopChild(i);
        c.error = "switched off";
        sw_.setAlive(i, false);
        return true;
    }
    if (c.running) return true;
    if (!out_) return true;   // not started: takes effect at start
    std::string e;
    if (!startChild(i, e)) { if (err) *err = e; return false; }
    sw_.setAlive(i, true);
    return true;
}

void SyntheticWideSource::worker() {
    std::vector<cf32> buf(1 << 15), wide;
    while (run_) {
        bool any = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const int64_t now = nowNs();
            for (int i = 0; i < (int)ch_.size(); i++) {
                Child& c = ch_[(size_t)i];
                if (!c.running || !c.ring) continue;
                // a queue more than half full: the stitcher cannot keep up; drop the backlog in one piece (the alignment re-finds the delay)
                if (c.ring->available() > c.ring->capacity() / 2) c.dropped += c.ring->skipToNewest(c.ring->capacity() / 16);
                for (int guard = 0; guard < 8; guard++) {
                    const size_t n = c.ring->read(buf.data(), buf.size());
                    if (n == 0) break;
                    any = true;
                    // the radio's DC spike (an RTL-SDR's sits in the middle of every slice): a slow running mean taken out
                    cf32 dc = c.dc;
                    for (size_t k = 0; k < n; k++) { dc += (buf[k] - dc) * 2e-5f; buf[k] -= dc; }
                    c.dc = dc;
                    c.got += n; c.lastNs = now;
                    sw_.push(i, buf.data(), n);
                    if (n < buf.size()) break;
                }
                if (now - c.rateT0 > 2000000000LL) {
                    c.deliveredPct = 100.0 * (double)(c.got - c.rateN0) / ((now - c.rateT0) * 1e-9 * plan_.cfg.fs);
                    c.rateT0 = now; c.rateN0 = c.got;
                }
            }
            wide.clear();
            sw_.pull(wide);
        }
        if (!wide.empty() && out_) out_->write(wide.data(), wide.size());
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

SynthGroupStatus SyntheticWideSource::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    SynthGroupStatus s;
    s.running = run_;
    s.name = def_.name;
    s.outRateHz = plan_.outRateHz; s.spanHz = plan_.spanHz; s.overlapHz = plan_.overlapHz; s.childRateHz = plan_.cfg.fs;
    s.usbWarning = usbWarn_;
    const int64_t now = nowNs();
    for (int i = 0; i < (int)ch_.size(); i++) {
        const Child& c = ch_[(size_t)i];
        SynthMemberStatus m;
        m.name = c.dev.name;
        m.centerHz = tune_.centerHz + plan_.sliceOffsetHz[(size_t)i];
        m.enabled = c.enabled;
        m.running = c.running && now - c.lastNs < 1000000000LL;
        const synthwide::SliceStatus ss = sw_.slice(i);
        m.alive = ss.alive && c.running;
        m.locked = i == 0 ? m.alive : ss.locked;
        m.offsetHz = ss.offsetHz; m.driftPpm = ss.driftPpm; m.dropouts = ss.dropouts;
        m.deliveredPct = c.deliveredPct;
        m.queueDropped = c.dropped;
        m.error = c.error;
        if (c.running && !m.running) m.error = "no samples for " + std::to_string((now - c.lastNs) / 1000000000LL) + " s";
        s.members.push_back(m);
    }
    for (int p = 0; p + 1 < (int)ch_.size(); p++) s.seams.push_back(sw_.pair(p));
    return s;
}

bool synthGroupStatus(const std::string& name, SynthGroupStatus& out) {
    std::lock_guard<std::mutex> lk(gMu);
    auto it = gLive.find(name);
    if (it == gLive.end()) return false;
    out = it->second->status();
    return true;
}
bool synthGroupRecalibrate(const std::string& name) {
    std::lock_guard<std::mutex> lk(gMu);
    auto it = gLive.find(name);
    if (it == gLive.end()) return false;
    it->second->recalibrate();
    return true;
}
bool synthGroupSetMember(const std::string& name, int member, bool enabled) {
    std::lock_guard<std::mutex> lk(gMu);
    auto it = gLive.find(name);
    if (it == gLive.end()) return false;
    return it->second->setMemberEnabled(member, enabled);
}

std::unique_ptr<IqSource> makeSynthGroupSource(const DeviceInfo& d) {
    SynthGroupDef def;
    bool have = false;
    for (const auto& g : synthGroups()) if (g.name == d.nativeArgs) { def = g; have = true; }
    if (!have) return nullptr;
    auto mem = synthGroupMembers(d);
    if (mem.size() < 2) return nullptr;
    if (def.diversity) return makeDiversitySource(def, mem);
    return std::make_unique<SyntheticWideSource>(def, mem);
}

} // namespace dect2
