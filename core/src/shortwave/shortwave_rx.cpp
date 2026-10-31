// Shortwave receiver: the general receiver plus one-click decoders on the same channel (see shortwave_rx.h).
#include "dect2/shortwave/shortwave_rx.h"
#include "dect2/shortwave/shortwave_tel.h"
#include "dect2/general/general_tel.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/hfdig/hfdig_tel.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/cw/cw_rx.h"
#include "dect2/cw/cw_tel.h"
#include "dect2/psk/psk_rx.h"
#include "dect2/psk/psk_tel.h"
#include "dect2/ale/ale_rx.h"
#include "dect2/ale/ale_tel.h"
#include "dect2/js8/js8_rx.h"
#include "dect2/js8/js8_tel.h"
#include "dect2/ardop/ardop_rx.h"
#include "dect2/ardop/ardop_tel.h"
#include "dect2/pactor/pactor_rx.h"
#include "dect2/pactor/pactor_tel.h"
#include "dect2/mt63/mt63_rx.h"
#include "dect2/mt63/mt63_tel.h"
#include "dect2/fsq/fsq_rx.h"
#include "dect2/ifkp/ifkp_rx.h"
#include "dect2/marine/marine_rx.h"
#include "dect2/marine/marine_tel.h"
#include "dect2/time/time_rx.h"
#include "dect2/time/time_tel.h"
#include "dect2/milhf/milhf_engine.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>

namespace dect2 {

using swr::Dec;

namespace {

constexpr double kPollSec = 0.15;          // how often the decoders' results are collected (signal time)
constexpr double kResetHz = 120.0;         // a retune this far means another signal: the decoder forgets the old one; less is tracking
constexpr double kAutoMaxSec = 40.0;       // Auto gives up after this much signal time
constexpr double kAutoGraceSec = 4.0;      // after the first decoder has valid text the others get this long to show a better one
constexpr size_t kTextKeep = 6000;

std::string clip(const std::string& s, size_t n = kTextKeep) { return s.size() <= n ? s : s.substr(s.size() - n); }

// ---------------------------------------------------------------- the decoders behind the buttons

class Slot {
public:
    virtual ~Slot() = default;
    virtual void configure(double rate) = 0;
    virtual void setOffset(double hz) = 0;
    virtual void setDial(double) {}
    virtual void reset() = 0;
    virtual void feed(const cf32* x, size_t n) = 0;
    virtual void poll(SwDecoderStatus& st) = 0;       // fills st from the newest telemetry
    virtual void clear() = 0;
    virtual void apply(const swr::Opts&) {}
};

// A receiver of the usual shape and the function that turns its telemetry into a status
template <class Rx, class Tel>
class Plain : public Slot {
public:
    using Fill = std::function<void(const Tel&, SwDecoderStatus&)>;
    Plain(Fill f, std::function<void(Rx&)> setup = nullptr) : fill_(std::move(f)) {
        if constexpr (requires(Rx& r) { r.setSilent(true); }) rx.setSilent(true);   // only the general receiver makes sound
        if (setup) setup(rx);
    }
    void configure(double r) override { rx.configure(r); }
    void setOffset(double hz) override { rx.setSignalOffset(hz); }
    void reset() override { rx.reset(); }
    void feed(const cf32* x, size_t n) override { rx.feed(x, n); }
    void poll(SwDecoderStatus& st) override {
        if (rx.telemetry(t_, seq_)) { seq_ = t_.seq; have_ = true; fill_(t_, last_); }
        if (have_) st = last_;
    }
    void clear() override {
        if constexpr (requires(Rx& r) { r.clearResults(); }) rx.clearResults();
        last_ = SwDecoderStatus(); have_ = false;
    }
    Rx rx;
protected:
    Fill fill_;
    Tel t_;
    uint64_t seq_ = 0;
    bool have_ = false;
    SwDecoderStatus last_;
};

// HF digital: one HfdigReceiver per button with only that decoder on
class HfdigSlot : public Slot {
public:
    explicit HfdigSlot(Dec d) : dec_(d) {
        rx_.setSilent(true);
        switch (d) {
        case Dec::Rtty: rx_.setDecoders(1); break;
        case Dec::Sstv: rx_.setDecoders(2); break;
        case Dec::FreeDv: rx_.setDecoders(4); break;
        case Dec::Ft8: case Dec::Ft4: case Dec::Wspr: {
            rx_.setDecoders(8);
            const int sel = d == Dec::Ft8 ? 0 : d == Dec::Ft4 ? 1 : 3;
            for (int m = 0; m < kFtxModes; m++) rx_.ftx().setEnabled(m, m == sel);
            break;
        }
        case Dec::HamDrm: rx_.setDecoders(16); break;
        default: {   // the keyboard modes
            rx_.setDecoders(32);
            KbdConfig k = rx_.kbd().config();
            k.family = d == Dec::Olivia ? KbdOlivia : d == Dec::Contestia ? KbdContestia : d == Dec::Mfsk ? KbdMfsk : d == Dec::Thor ? KbdThor : d == Dec::DominoEx ? KbdDominoex : KbdHell;
            rx_.kbd().setConfig(k);
        }
        }
    }
    void configure(double r) override { rx_.configure(r); }
    void setOffset(double hz) override { rx_.setSignalOffset(hz); }
    void setDial(double hz) override { if (dec_ == Dec::Ft8 || dec_ == Dec::Ft4 || dec_ == Dec::Wspr) rx_.ftx().setDialHz(hz); }
    void reset() override { rx_.reset(); }
    void feed(const cf32* x, size_t n) override { rx_.feed(x, n); }
    void clear() override { rx_.clearResults(); last_ = SwDecoderStatus(); have_ = false; }
    void apply(const swr::Opts& o) override {
        if (dec_ == Dec::Rtty) {
            const bool autoRtty = o.rttyBaud < 0 && o.rttyShift < 0;
            rx_.rtty().setBaudIndex(std::max(0, o.rttyBaud));
            rx_.rtty().setShiftIndex(std::max(0, o.rttyShift));
            rx_.rtty().setReverse(o.rttyReverse);
            // automatic speed / shift / polarity: the whole-span RTTY bank (a pair of tones is tracked with every combination, the best kept)
            rx_.setWide(autoRtty, 8000);
            auto_ = autoRtty;
        } else if (dec_ == Dec::Mfsk || dec_ == Dec::Thor || dec_ == Dec::DominoEx || dec_ == Dec::Hell) {
            KbdConfig k = rx_.kbd().config();
            k.variant = o.variant;
            rx_.kbd().setConfig(k);
        }
    }
    void poll(SwDecoderStatus& st) override {
        if (rx_.telemetry(t_, seq_)) { seq_ = t_.seq; have_ = true; fillFrom(t_, last_); }
        if (have_) st = last_;
    }
private:
    void fillFrom(const HfdigTelemetry& t, SwDecoderStatus& st) {
        char b[160];
        st.quality = 0;
        switch (dec_) {
        case Dec::Rtty: {
            const auto& r = t.rtty;
            std::string text = r.text;
            double baud = r.baud, shift = r.shiftHz, q = r.quality;
            uint64_t chars = r.chars;
            int state = r.state;
            if (auto_ && !t.rttyWide.empty()) {   // the best signal of the bank: resolved first, then the most characters
                const RttyWideSignal* best = nullptr;
                for (const auto& s : t.rttyWide) {
                    if (!best || (s.resolved && !best->resolved) || (s.resolved == best->resolved && s.chars > best->chars)) best = &s;
                }
                if (best && best->chars > chars) { text = best->text; baud = best->baud; shift = best->shiftHz; chars = best->chars; q = best->quality; state = best->active ? 2 : 1; }
            }
            st.text = clip(text); st.chars = chars; st.quality = q; st.state = state;
            st.valid = chars >= 10 && q > 0.4f;
            if (state == 0) snprintf(b, sizeof b, "RTTY: searching for a pair of tones");
            else snprintf(b, sizeof b, "RTTY %.2f baud / %.0f Hz shift, %s, %llu characters", baud, shift, state == 2 ? "receiving" : "locked", (unsigned long long)chars);
            st.status = b;
            break;
        }
        case Dec::Sstv: {
            const auto& s = t.sstv;
            st.state = s.state == 1 ? 2 : s.state == 2 ? 1 : 0;
            st.chars = s.picturesDone;
            st.valid = s.picturesDone > 0 || s.lines > 20;
            if (s.state == 1) snprintf(b, sizeof b, "SSTV %s: line %d of %d", s.mode.c_str(), s.lines, s.height);
            else if (s.picturesDone) snprintf(b, sizeof b, "SSTV: %llu pictures", (unsigned long long)s.picturesDone);
            else snprintf(b, sizeof b, "SSTV: waiting for a VIS header");
            st.status = b;
            if (s.image && s.imageSeq != st.imageSeq && s.image->width > 0) {
                auto im = std::make_shared<SwImage>();
                im->w = s.image->width; im->h = s.image->height; im->px = s.image->rgb;
                st.image = im; st.imageSeq = s.imageSeq;
            }
            break;
        }
        case Dec::FreeDv: {
            const auto& f = t.freedv;
            st.state = f.mode >= 0 ? 2 : 0;
            st.valid = f.mode >= 0;
            if (!f.libFound) st.note = "needs the Codec2 library (libcodec2), which OnAir does not ship";
            else st.note.clear();
            snprintf(b, sizeof b, "FreeDV: %s", f.mode >= 0 ? "sync" : (f.libFound ? "no sync" : "no library"));
            st.status = b;
            break;
        }
        case Dec::Ft8: case Dec::Ft4: case Dec::Wspr: {
            const int mode = dec_ == Dec::Ft8 ? 0 : dec_ == Dec::Ft4 ? 1 : 3;
            std::string text;
            uint64_t n = 0;
            for (const auto& d : t.ftx.decodes) {
                if (d.mode != mode) continue;
                n++;
                const std::time_t tt = (std::time_t)d.slotUtc;
                std::tm tmv{};
                gmtime_r(&tt, &tmv);
                snprintf(b, sizeof b, "%02d%02d%02d %+3.0f %+4.1f %4.0f ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (double)d.snrDb, (double)d.dt, (double)d.hz);
                text += b; text += d.msg; text += '\n';
            }
            st.text = clip(text, 4000); st.chars = n; st.valid = n > 0; st.state = n ? 2 : t.ftx.audioSamples ? 1 : 0;
            snprintf(b, sizeof b, "%s: %llu decodes (%d in the last slot)", ftxModeName(mode), (unsigned long long)n, t.ftx.lastSlotCount[mode]);
            st.status = b;
            break;
        }
        case Dec::HamDrm: {
            st.state = t.hamdrm.state == 1 ? 2 : 0;
            st.valid = t.hamdrm.state == 1 && t.hamdrm.mode >= 0;
            const std::string s = hfdigSummary(t);
            st.status = s == "listening" ? "HamDRM: listening" : s;
            break;
        }
        default: {   // Olivia .. Hell
            const auto& k = t.kbd;
            st.state = k.state == 1 ? 2 : 0;
            st.text = clip(k.text); st.chars = k.chars; st.valid = k.state == 1 && k.chars >= 8;
            if (k.state == 1) snprintf(b, sizeof b, "%s locked, %llu characters", swr::decoderInfo(dec_).name, (unsigned long long)k.chars);
            else snprintf(b, sizeof b, "%s: searching", swr::decoderInfo(dec_).name);
            st.status = b;
            if (dec_ == Dec::Hell && k.hellCols > 0) {
                auto im = std::make_shared<SwImage>();
                im->w = k.hellCols; im->h = (int)(k.hell.size() / (size_t)k.hellCols); im->gray = true; im->px = k.hell;
                st.image = im; st.imageSeq = k.hellTotal;
                st.valid = true;
            }
        }
        }
    }
    Dec dec_;
    HfdigReceiver rx_;
    HfdigTelemetry t_;
    uint64_t seq_ = 0;
    bool have_ = false, auto_ = false;
    SwDecoderStatus last_;
};

// ---- fills for the other receivers
void fillCw(const CwTelemetry& t, SwDecoderStatus& st) {
    // Morse at 4 .. 45 WPM; keyed FSK (RTTY at 45 baud keys a tone at 54 WPM and more) is not Morse. Of those that fit, the one nearest the marker
    const CwSignal* best = nullptr;
    for (const auto& s : t.signals) {
        const bool plausible = s.locked && s.wpm >= 4.f && s.wpm <= 45.f;
        if (!plausible) continue;
        if (!best || std::fabs(s.offsetHz) + 400.0 / (1.0 + (double)s.chars) < std::fabs(best->offsetHz) + 400.0 / (1.0 + (double)best->chars)) best = &s;
    }
    st.text = best ? clip(best->text) : std::string();
    st.chars = best ? best->chars : 0;
    st.state = t.state;
    st.valid = best && best->chars >= 8;
    st.quality = best ? std::min(1.f, best->snrDb / 30.f) : 0.f;
    st.status = cwSummary(t);
}
void fillPsk(const PskTelemetry& t, SwDecoderStatus& st) {
    std::string text;
    uint64_t chars = 0;
    const PskSignal* best = nullptr;
    for (const auto& s : t.signals) if (!best || s.chars > best->chars) best = &s;
    if (best) { text = best->text; chars = best->chars; }
    st.text = clip(text); st.chars = chars;
    st.state = t.state;
    st.valid = best && best->dcd && chars >= 8;
    st.quality = best ? best->quality : 0.f;
    st.status = pskSummary(t);
}
void fillAle(const AleTelemetry& t, SwDecoderStatus& st) {
    std::string text;
    char b[200];
    const size_t from = t.events.size() > 60 ? t.events.size() - 60 : 0;
    for (size_t i = from; i < t.events.size(); i++) {
        const auto& e = t.events[i];
        const std::time_t tt = (std::time_t)e.utc;
        std::tm tm{};
        gmtime_r(&tt, &tm);
        snprintf(b, sizeof b, "%02d:%02d:%02d %s %s%s%s%s%s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, e.type.c_str(), e.from.c_str(), e.to.empty() ? "" : " -> ", e.to.c_str(), e.amd.empty() ? "" : "  AMD: ", e.amd.c_str());
        text += b;
    }
    st.text = clip(text); st.chars = t.events.size(); st.state = t.state; st.valid = !t.events.empty();
    st.status = aleSummary(t);
}
void fillJs8(const Js8Telemetry& t, SwDecoderStatus& st) {
    std::string text;
    const size_t from = t.decodes.size() > 60 ? t.decodes.size() - 60 : 0;
    for (size_t i = from; i < t.decodes.size(); i++) text += t.decodes[i].text + "\n";
    st.text = clip(text); st.chars = t.decodes.size(); st.state = t.state; st.valid = !t.decodes.empty();
    st.status = js8Summary(t);
}
void fillKbd33(const Kbd33Telemetry& t, SwDecoderStatus& st, const std::string& summary) {
    st.text = clip(t.text); st.chars = t.chars; st.state = t.state; st.valid = t.dataValid && t.chars >= 8; st.quality = std::min(1.f, t.quality * 2);
    st.status = summary;
}
void fillMt63(const Mt63Telemetry& t, SwDecoderStatus& st) {
    st.text = clip(t.text); st.chars = t.chars; st.state = t.state; st.valid = t.dataValid && t.chars >= 8;
    st.status = mt63Summary(t);
}
template <class Tel>
void fillCounts(const Tel& t, SwDecoderStatus& st, const std::string& summary) {
    st.state = t.state; st.chars = t.blocksOk; st.valid = t.blocksOk > 0; st.status = summary;
}

// Marine receiver: NAVTEX or weather fax
class MarineSlot : public Slot {
public:
    explicit MarineSlot(Dec d) : dec_(d) { rx_.setSilent(true); rx_.setService(d == Dec::Navtex ? 1 : 3); }
    void configure(double r) override { rx_.configure(r); }
    void setOffset(double hz) override { rx_.setSignalOffset(hz); }
    void setDial(double hz) override { rx_.setFrequencyHz(hz); }
    void reset() override { rx_.reset(); }
    void feed(const cf32* x, size_t n) override { rx_.feed(x, n); }
    void clear() override { rx_.clearResults(); last_ = SwDecoderStatus(); have_ = false; }
    void poll(SwDecoderStatus& st) override {
        if (rx_.telemetry(t_, seq_)) {
            seq_ = t_.seq; have_ = true;
            char b[160];
            if (dec_ == Dec::Navtex) {
                std::string text;
                const size_t from = t_.navtex.size() > 20 ? t_.navtex.size() - 20 : 0;
                for (size_t i = from; i < t_.navtex.size(); i++) text += t_.navtex[i].header + "  " + t_.navtex[i].subjectName + "\n" + t_.navtex[i].text + "\n\n";
                last_.text = clip(text); last_.chars = t_.navtexCount; last_.valid = t_.navtexCount > 0;
                last_.state = t_.navtexLocked ? 2 : 0;
                snprintf(b, sizeof b, "NAVTEX: %s, %llu messages", t_.navtexLocked ? "locked" : "searching", (unsigned long long)t_.navtexCount);
            } else {
                const FaxStatus fs = rx_.faxStatus();
                last_.state = fs.state == 2 ? 2 : fs.state == 1 ? 1 : 0;
                last_.chars = (uint64_t)fs.lines; last_.valid = fs.lines > 20;
                static const char* const kSt[] = {"waiting for the start tone", "phasing", "receiving", "stopped"};
                snprintf(b, sizeof b, "Weather fax: %s, %d lines, %d lpm", kSt[std::clamp(fs.state, 0, 3)], fs.lines, fs.lpm);
                FaxImage fi;
                if (rx_.latestImage(fi, imgSeq_) && fi.width > 0 && fi.height > 0) {
                    auto im = std::make_shared<SwImage>();
                    im->w = fi.width; im->h = fi.height; im->gray = true; im->px = fi.pix;
                    last_.image = im; last_.imageSeq = imgSeq_;
                }
            }
            last_.status = b;
        }
        if (have_) st = last_;
    }
private:
    Dec dec_;
    MarineReceiver rx_;
    MarineTelemetry t_;
    uint64_t seq_ = 0, imgSeq_ = 0;
    bool have_ = false;
    SwDecoderStatus last_;
};

std::unique_ptr<Slot> makeSlot(Dec d) {
    switch (d) {
    case Dec::Rtty: case Dec::Sstv: case Dec::FreeDv: case Dec::Ft8: case Dec::Ft4: case Dec::Wspr: case Dec::HamDrm:
    case Dec::Olivia: case Dec::Contestia: case Dec::Mfsk: case Dec::Thor: case Dec::DominoEx: case Dec::Hell:
        return std::make_unique<HfdigSlot>(d);
    case Dec::Cw: { auto s = std::make_unique<Plain<CwReceiver, CwTelemetry>>(fillCw, [](CwReceiver& r) { r.setSilent(true); }); return s; }
    case Dec::Psk: {
        struct PskSlot : Plain<PskReceiver, PskTelemetry> {
            PskSlot() : Plain(fillPsk, [](PskReceiver& r) { r.setSilent(true); }) {}
            void apply(const swr::Opts& o) override { if (o.pskMode != mode_) { mode_ = o.pskMode; rx.setMode(std::clamp(mode_, 0, kPskModes - 1)); } }
            int mode_ = 0;
        };
        return std::make_unique<PskSlot>();
    }
    case Dec::Js8: return std::make_unique<Plain<Js8Receiver, Js8Telemetry>>(fillJs8, [](Js8Receiver& r) { r.setSilent(true); });
    case Dec::Ale: {
        struct AleSlot : Plain<AleReceiver, AleTelemetry> {
            AleSlot() : Plain(fillAle) {}
            void setDial(double hz) override { rx.setDialHz(hz); }
        };
        return std::make_unique<AleSlot>();
    }
    case Dec::Ardop: return std::make_unique<Plain<ArdopReceiver, ArdopTelemetry>>([](const ArdopTelemetry& t, SwDecoderStatus& s) { fillCounts(t, s, ardopSummary(t)); });
    case Dec::Pactor: return std::make_unique<Plain<PactorReceiver, PactorTelemetry>>([](const PactorTelemetry& t, SwDecoderStatus& s) { fillCounts(t, s, pactorSummary(t)); });
    case Dec::Mt63: return std::make_unique<Plain<Mt63Receiver, Mt63Telemetry>>(fillMt63, [](Mt63Receiver& r) { r.setSilent(true); });
    case Dec::Fsq: return std::make_unique<Plain<FsqReceiver, FsqTelemetry>>([](const FsqTelemetry& t, SwDecoderStatus& s) { fillKbd33(t, s, fsqSummary(t)); });
    case Dec::Ifkp: return std::make_unique<Plain<IfkpReceiver, IfkpTelemetry>>([](const IfkpTelemetry& t, SwDecoderStatus& s) { fillKbd33(t, s, ifkpSummary(t)); });
    case Dec::Navtex: case Dec::Wefax: return std::make_unique<MarineSlot>(d);
    case Dec::TimeSig: {
        struct TimeSlot : Plain<TimeReceiver, TimeTelemetry> {
            TimeSlot() : Plain([](const TimeTelemetry& t, SwDecoderStatus& s) { s.state = t.state; s.chars = t.blocksOk; s.valid = t.state >= 2; s.status = timeSummary(t); }) {}
            void setDial(double hz) override { rx.setDialHz(hz); }
        };
        return std::make_unique<TimeSlot>();
    }
    case Dec::HfId: {
        struct IdSlot : Plain<MilhfReceiver, MilhfTelemetry> {
            IdSlot() : Plain([](const MilhfTelemetry& t, SwDecoderStatus& s) { s.state = t.state; s.valid = t.analysed; s.status = milhfSummary(t); }) {}
            void clear() override { last_ = SwDecoderStatus(); have_ = false; }
        };
        return std::make_unique<IdSlot>();
    }
    default: return nullptr;
    }
}

}  // namespace

// ---------------------------------------------------------------- the receiver

struct ShortwaveReceiver::Impl {
    struct Run {
        std::unique_ptr<Slot> slot;
        double lastOff = 0;
        bool placed = false;
        bool mirror = false;               // fed the conjugate of the capture (a lower sideband decoder)
        swr::Opts opts;
        bool optsSet = false;
        SwDecoderStatus st;
        // log
        bool logOn = false;
        std::string logDir, logPath, logPrev;
    };

    GeneralReceiver gen;
    mutable std::mutex mu;                 // router, flags, the snapshot
    swr::Router router;
    std::map<int, Run> runs;               // by Dec, feed thread only
    uint64_t appliedRev = 0;
    double rate = 0, offset = 0, centre = 0, dial = 0, userChan = 0;
    int demod = GenNfm;
    double acc = 0;                        // signal seconds since the last poll
    std::vector<cf32> mirror;
    std::function<void(const std::string&)> logCb;
    // snapshot for telemetry()
    uint64_t snapSeq = 0, genSeq = 0;
    GeneralTelemetry genTel;
    std::vector<SwDecoderStatus> snapDec;
    uint64_t clearMask = 0;                // decoders to clear on the next feed (bit per Dec)
    std::map<int, std::string> logPaths;
    std::map<int, std::pair<bool, std::string>> logReq;   // requested log state per Dec: (on, dir)
    // Auto
    bool autoReq = false, autoOn = false;
    int autoPhase = 0, autoPick = -1;
    double autoSec = 0, autoValidSince = -1;
    std::vector<int> autoStarted;
    int detected = -1;
    float detectedConf = 0;
    double chanShiftApplied = 1e30;
    bool silent = false;

    void say(const std::string& s) { if (logCb) logCb(s); }
    void pushGeneralChannel() {   // the listening channel = the marker plus the decoder's audio shift
        const double shift = router.audioShiftHz(demod == GenUsb, demod == GenLsb);
        gen.setChannelOffsetHz(userChan + shift);
    }
    void sync();
    void pollAll(double dt);
    void doLog(Run& r, int id);
};

ShortwaveReceiver::ShortwaveReceiver() : p_(std::make_unique<Impl>()) {}
ShortwaveReceiver::~ShortwaveReceiver() = default;

GeneralReceiver& ShortwaveReceiver::general() { return p_->gen; }

void ShortwaveReceiver::configure(double r) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.mu);
    m.rate = r;
    m.gen.configure(r);
    for (auto& kv : m.runs) kv.second.slot->configure(r);
    m.appliedRev = 0;   // the decoders that need the rate are checked again
}
void ShortwaveReceiver::setSignalOffset(double hz) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.mu);
    m.offset = hz;
    m.gen.setSignalOffset(hz);
    m.router.setChannel(hz + m.userChan);
}
void ShortwaveReceiver::setCenterHz(double hz) { p_->centre = hz; }
void ShortwaveReceiver::setDialHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->dial = hz; }
bool ShortwaveReceiver::ready() const { return p_->gen.ready(); }
void ShortwaveReceiver::reset() {
    Impl& m = *p_;
    m.gen.reset();
    std::lock_guard<std::mutex> lk(m.mu);
    for (auto& kv : m.runs) kv.second.slot->reset();
}
void ShortwaveReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    p_->logCb = cb;
    p_->gen.setLogCallback(cb);
}
void ShortwaveReceiver::setSilent(bool s) { p_->silent = s; p_->gen.setSilent(s); }

void ShortwaveReceiver::setChannelOffsetHz(double hz) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.mu);
    m.userChan = hz;
    m.router.setChannel(m.offset + hz);
    m.pushGeneralChannel();
}
void ShortwaveReceiver::setDemod(int d) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.mu);
    m.demod = d;
    m.gen.setDemod(d);
    m.pushGeneralChannel();
}

bool ShortwaveReceiver::startDecoder(Dec d) { std::lock_guard<std::mutex> lk(p_->mu); const bool r = p_->router.start(d); p_->pushGeneralChannel(); return r; }
bool ShortwaveReceiver::stopDecoder(Dec d) { std::lock_guard<std::mutex> lk(p_->mu); const bool r = p_->router.stop(d); p_->pushGeneralChannel(); return r; }
bool ShortwaveReceiver::decoderActive(Dec d) const { std::lock_guard<std::mutex> lk(p_->mu); return p_->router.active(d); }
void ShortwaveReceiver::stopAllDecoders() { std::lock_guard<std::mutex> lk(p_->mu); p_->router.stopAll(); p_->autoReq = false; p_->pushGeneralChannel(); }
void ShortwaveReceiver::setDecoderOpts(Dec d, const swr::Opts& o) { std::lock_guard<std::mutex> lk(p_->mu); p_->router.setOpts(d, o); p_->pushGeneralChannel(); }
swr::Opts ShortwaveReceiver::decoderOpts(Dec d) const { std::lock_guard<std::mutex> lk(p_->mu); return p_->router.opts(d); }
void ShortwaveReceiver::clearDecoder(Dec d) { std::lock_guard<std::mutex> lk(p_->mu); p_->clearMask |= 1ull << (int)d; }
std::vector<Dec> ShortwaveReceiver::running() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->router.running(); }
swr::Router ShortwaveReceiver::routerCopy() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->router; }
void ShortwaveReceiver::setDetected(Dec d, float c) { std::lock_guard<std::mutex> lk(p_->mu); p_->router.setDetected(d, c); p_->detected = d == Dec::Count ? -1 : (int)d; p_->detectedConf = c; }
void ShortwaveReceiver::setAuto(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->autoReq = on; }
bool ShortwaveReceiver::autoOn() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->autoReq; }

void ShortwaveReceiver::setDecoderLog(Dec d, bool on, const std::string& dir) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->logReq[(int)d] = {on, dir};
}
bool ShortwaveReceiver::decoderLogging(Dec d) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->logReq.find((int)d);
    return it != p_->logReq.end() && it->second.first;
}
std::string ShortwaveReceiver::decoderLogPath(Dec d) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->logPaths.find((int)d);
    return it == p_->logPaths.end() ? std::string() : it->second;
}
bool ShortwaveReceiver::saveDecoderText(Dec d, const std::string& path, std::string* err) const {
    std::string text;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (const auto& s : p_->snapDec) if (s.id == (int)d) text = s.text;
    }
    if (text.empty()) { if (err) *err = "nothing decoded yet"; return false; }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    f << text;
    return (bool)f;
}

// takes up button presses, option changes, clears and log requests; runs on the feed thread
void ShortwaveReceiver::Impl::sync() {
    std::lock_guard<std::mutex> lk(mu);
    // clears
    if (clearMask) {
        for (auto& kv : runs) if (clearMask & (1ull << kv.first)) { kv.second.slot->clear(); kv.second.st = SwDecoderStatus(); kv.second.logPrev.clear(); }
        clearMask = 0;
    }
    // logs
    for (const auto& q : logReq) {
        auto it = runs.find(q.first);
        if (it == runs.end()) continue;
        it->second.logOn = q.second.first;
        it->second.logDir = q.second.second;
    }
    if (router.revision() != appliedRev || appliedRev == 0) {
        appliedRev = router.revision();
        // stop what went off
        for (auto it = runs.begin(); it != runs.end();) {
            if (!router.active((Dec)it->first)) it = runs.erase(it); else ++it;
        }
        // start what went on
        for (Dec d : router.running()) {
            const swr::DecoderInfo& info = swr::decoderInfo(d);
            auto it = runs.find((int)d);
            if (it == runs.end()) {
                if (rate <= 0 || rate < info.minRateHz) continue;   // the capture is too narrow: the status says so
                auto s = makeSlot(d);
                if (!s) continue;
                Run r;
                s->configure(rate);
                r.slot = std::move(s);
                r.st.id = (int)d;
                it = runs.emplace((int)d, std::move(r)).first;
                const auto lq = logReq.find((int)d);
                if (lq != logReq.end()) { it->second.logOn = lq->second.first; it->second.logDir = lq->second.second; }
                say(std::string("Shortwave: ") + info.name + " on");
            }
            Run& r = it->second;
            if (!r.optsSet || !(r.opts == router.opts(d))) { r.opts = router.opts(d); r.optsSet = true; r.slot->apply(r.opts); }
            r.slot->setDial(dial);
        }
        // Auto bookkeeping: a button the user pressed while Auto ran is theirs from then on
    }
    // offsets: every retune is a call here (cheap); a jump of more than kResetHz is another signal
    for (auto& kv : runs) {
        Run& r = kv.second;
        const swr::Plan p = router.plan((Dec)kv.first);
        r.mirror = p.mirror;
        if (!r.placed) { r.slot->setOffset(p.dialOffsetHz); r.slot->setDial(dial); r.lastOff = p.dialOffsetHz; r.placed = true; continue; }
        if (p.dialOffsetHz != r.lastOff) {
            if (std::fabs(p.dialOffsetHz - r.lastOff) > kResetHz) r.slot->reset();
            r.slot->setOffset(p.dialOffsetHz);
            r.lastOff = p.dialOffsetHz;
        }
    }
    pushGeneralChannel();
}

void ShortwaveReceiver::Impl::doLog(Run& r, int id) {
    if (!r.logOn || r.logDir.empty()) return;
    const std::string delta = swr::textDelta(r.logPrev, r.st.text);
    r.logPrev = r.st.text;
    if (delta.empty()) return;
    if (r.logPath.empty()) {
        char b[64];
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_r(&t, &tm);
        strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
        r.logPath = r.logDir + "/" + swr::decoderInfo((Dec)id).key + "_" + b + ".txt";
    }
    logPaths[id] = r.logPath;
    std::ofstream f(r.logPath, std::ios::binary | std::ios::app);
    if (f) f << delta;
}

void ShortwaveReceiver::Impl::pollAll(double dt) {
    std::vector<SwDecoderStatus> out;
    std::vector<int> toStop;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& kv : runs) {
            Run& r = kv.second;
            r.slot->poll(r.st);
            r.st.id = kv.first;
            doLog(r, kv.first);
        }
        // Auto: start the likely text decoders together, keep the first that produces valid text
        if (autoReq && !autoOn) {
            autoOn = true; autoPhase = 1; autoPick = -1; autoSec = 0; autoValidSince = -1; autoStarted.clear();
            static const Dec kCand[] = {Dec::Rtty, Dec::Cw, Dec::Psk, Dec::Olivia, Dec::Mfsk, Dec::Ale};
            for (Dec d : kCand) if (!router.active(d)) { router.start(d); autoStarted.push_back((int)d); }
        } else if (!autoReq && autoOn) {
            autoOn = false; autoPhase = 0;
            for (int d : autoStarted) if (d != autoPick) router.stop((Dec)d);
            autoStarted.clear();
        }
        if (autoOn && autoPhase == 1) {
            autoSec += dt;
            // the first decoder with valid text starts a short grace period (a false friend may validate first); then the best score wins
            int firstValid = -1;
            for (int d : autoStarted) { auto it = runs.find(d); if (it != runs.end() && it->second.st.valid) { firstValid = d; break; } }
            if (firstValid >= 0 && autoValidSince < 0) autoValidSince = autoSec;
            if (autoValidSince >= 0 && autoSec - autoValidSince >= kAutoGraceSec) {
                int pick = -1;
                double bestScore = -1;
                for (int d : autoStarted) {
                    auto it = runs.find(d);
                    if (it == runs.end() || !it->second.st.valid) continue;
                    const double score = (0.2 + (double)it->second.st.quality) * std::log2(2.0 + (double)it->second.st.chars);
                    if (score > bestScore) { bestScore = score; pick = d; }
                }
                if (pick < 0) autoValidSince = -1;   // it fell back to invalid: keep looking
                else {
                    autoPick = pick; autoPhase = 2;
                    for (int d : autoStarted) if (d != pick) router.stop((Dec)d);
                    autoStarted.assign(1, pick);
                    say(std::string("Shortwave Auto: ") + swr::decoderInfo((Dec)pick).name + " produces valid text");
                }
            } else if (autoSec > kAutoMaxSec) {
                autoPhase = 2;
                for (int d : autoStarted) router.stop((Dec)d);
                autoStarted.clear();
                say("Shortwave Auto: nothing produced valid text");
            }
        }
        for (auto& kv : runs) out.push_back(kv.second.st);
        // a stopped decoder disappears from the list at once
        snapDec = out;
        snapSeq++;
    }
}

void ShortwaveReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (n == 0) return;
    m.sync();
    m.gen.feed(x, n);
    bool mirrored = false;
    for (auto& kv : m.runs) {
        Impl::Run& r = kv.second;
        if (r.mirror) {
            if (m.mirror.size() < n) m.mirror.resize(n);
            if (!mirrored) { for (size_t i = 0; i < n; i++) m.mirror[i] = std::conj(x[i]); mirrored = true; }
            r.slot->feed(m.mirror.data(), n);
        } else r.slot->feed(x, n);
    }
    if (m.rate > 0) m.acc += (double)n / m.rate;
    if (m.acc >= kPollSec) {
        const double dt = m.acc;
        m.acc = 0;
        GeneralTelemetry g;
        uint64_t gs = m.genSeq;
        const bool got = m.gen.telemetry(g, gs);
        m.pollAll(dt);
        if (got) { std::lock_guard<std::mutex> lk(m.mu); m.genTel = g; m.genSeq = g.seq; m.snapSeq++; }
    }
}

bool ShortwaveReceiver::telemetry(ShortwaveTelemetry& out, uint64_t lastSeq) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.mu);
    if (m.snapSeq == 0 || m.snapSeq <= lastSeq) return false;
    out.seq = m.snapSeq;
    out.general = m.genTel;
    out.state = m.genTel.state; out.cfoHz = m.genTel.cfoHz; out.snrDb = m.genTel.snrDb; out.dataValid = m.genTel.dataValid;
    out.blocksOk = m.genTel.blocksOk; out.blocksBad = m.genTel.blocksBad; out.levelDb = m.genTel.levelDb;
    out.decoders = m.snapDec;
    // a decoder that is on but could not be made (the capture is too narrow): say so
    for (Dec d : m.router.running()) {
        bool found = false;
        for (const auto& s : out.decoders) if (s.id == (int)d) found = true;
        if (!found) {
            SwDecoderStatus s;
            s.id = (int)d;
            const swr::DecoderInfo& info = swr::decoderInfo(d);
            if (m.rate > 0 && m.rate < info.minRateHz) s.note = "the capture is too narrow for this decoder (needs " + std::to_string((int)(info.minRateHz / 1000)) + " ksps)";
            s.status = std::string(info.name) + ": starting";
            out.decoders.push_back(s);
        }
    }
    std::sort(out.decoders.begin(), out.decoders.end(), [](const SwDecoderStatus& a, const SwDecoderStatus& b) { return a.id < b.id; });
    out.channelOffsetHz = m.router.channel();
    out.inputRate = m.rate;
    out.detected = m.detected; out.detectedConfidence = m.detectedConf;
    out.autoPhase = m.autoReq ? (m.autoPhase ? m.autoPhase : 1) : 0;
    out.autoPick = m.autoPick; out.autoSec = m.autoSec;
    return true;
}

ModeTuning shortwaveTuning() {
    ModeTuning t;
    t.stdMode = 213;                 // family 211 + 2
    t.id = "shortwave";
    t.name = "Shortwave";
    t.minMhz = 0.1; t.maxMhz = 30; t.defMhz = 10.000;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 1.6;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;          // as HF digital: the radio sits 20 kHz above the dial, away from its DC spike
    return t;
}

std::string shortwaveSummary(const ShortwaveTelemetry& t) {
    std::string s = generalSummary(t.general);
    for (const auto& d : t.decoders) {
        if (!d.status.empty()) s += std::string(" \xC2\xB7 ") + d.status;
    }
    return s;
}

}  // namespace dect2
