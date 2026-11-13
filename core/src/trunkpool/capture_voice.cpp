// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// See capture_voice.h.
#include "dect2/trunkpool/capture_voice.h"
#include "dect2/dmr/dmr_rx.h"
#include "dect2/dsp/dc_notch.h"
#include "dect2/dsp/fm_front.h"
#include "dect2/nxdn/nxdn_rx.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/p25p2/p25p2_rx.h"
#include "dect2/trunkpool/slice_ddc.h"
#include "dect2/voice/voice_monitor.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>

namespace dect2 {
namespace tpool {

namespace {
constexpr double kWork = 48000.0;
constexpr int kNoiseBlock = 480;            // 10 ms
constexpr double kNoiseRmsHz = 3500.0;      // above the speech band: wide FM noise from an empty channel is far above this
constexpr int kHangBlocks = 30;             // 300 ms
constexpr size_t kMaxQueued = 48000 * 5;
constexpr double kVoiceHoldSec = 0.5;       // a digital slice is hearing voice for this long after its last voice frame
constexpr double kDcGuardHz = 20000;        // automatic DC notch: a slice this close to the centre
constexpr int kMaxSlots = 16;

enum class Dec { Analog, P25, P25p2, Dmr, Nxdn };
Dec decoderOf(Proto p) {
    switch (p) {
    case Proto::P25p1: return Dec::P25;
    case Proto::P25p2: return Dec::P25p2;
    case Proto::DmrTier3: case Proto::DmrCapPlus: case Proto::DmrConnectPlus: return Dec::Dmr;
    case Proto::Nxdn: return Dec::Nxdn;
    default: return Dec::Analog;   // Motorola Type II analog, EDACS, MPT 1327, LTR, other
    }
}
double nowCpu() { using namespace std::chrono; return duration<double>(steady_clock::now().time_since_epoch()).count(); }
}

struct CaptureVoice::Dc { DcNotch n; };

struct CaptureVoice::Slice {
    bool open = false;
    Dec dec = Dec::Analog;
    Proto proto = Proto::Other;
    double hz = 0;
    int slot = -1;
    SliceDdc ddc;
    FmFront fm;
    FmBiquad lp, nlp;
    double dc = 0;
    std::vector<float> disc, block;
    std::deque<float> queue;
    int rate = 48000;
    double sum = 0;
    int count = 0, hang = 0, noisy = 0;   // noisy: consecutive 10 ms blocks of channel noise
    bool hearing = false;                 // analog: the squelch; digital: see lastVoice
    double lastVoice = -1e9;              // signal time of the last digital voice frame
    uint64_t frames = 0;
    double cpu = 0;                       // seconds of processing
    std::unique_ptr<P25Receiver> p25;
    std::unique_ptr<P25p2Receiver> p2;
    std::unique_ptr<DmrReceiver> dmr;
    std::unique_ptr<NxdnReceiver> nxdn;
};

CaptureVoice::CaptureVoice(int slices) : dc_(new Dc) {
    for (int i = 0; i < std::min(kMaxSlots, std::max(1, slices)); i++) s_.push_back(std::make_unique<Slice>());
}
CaptureVoice::~CaptureVoice() = default;

void CaptureVoice::resetSlice(Slice& x) {
    x.open = false; x.p25.reset(); x.p2.reset(); x.dmr.reset(); x.nxdn.reset(); x.queue.clear(); x.hearing = false;
    x.frames = 0; x.lastVoice = -1e9; x.cpu = 0;
}

void CaptureVoice::configure(double rateHz) {
    fs_ = rateHz; fed_ = 0;
    dc_->n.set(rateHz > 0 ? rateHz : 1, 400.0); dc_->n.reset();
    for (auto& p : s_) resetSlice(*p);
}

void CaptureVoice::setSlots(int n) {
    n = std::min(kMaxSlots, std::max(1, n));
    while ((int)s_.size() > n) s_.pop_back();   // the pool sees the slice closed (sliceHearing false) and ends its call
    while ((int)s_.size() < n) s_.push_back(std::make_unique<Slice>());
}

bool CaptureVoice::openSlice(const TuneRequest& r, int* slice) {
    if (!(fs_ > 0) || !(centre_ > 0)) return false;
    for (size_t i = 0; i < s_.size(); i++) {
        Slice& x = *s_[i];
        if (x.open) continue;
        resetSlice(x);
        x.hz = r.freqHz; x.proto = r.proto; x.dec = decoderOf(r.proto); x.slot = r.slot;
        if (!x.ddc.configure(fs_, r.freqHz - centre_, 9000, 48000)) return false;
        const double nr = x.ddc.outRate();
        x.open = true;
        x.fm.configure(nr, kWork, 7500.0); x.fm.setOffset(0); x.fm.reset();
        x.lp.lowpass(3400.0, kWork, 0.7071); x.nlp.lowpass(5000.0, kWork, 0.7071);
        x.dc = 0; x.sum = 0; x.count = 0; x.hang = 0; x.noisy = 0;
        Slice* sp = &x;
        const int idx = (int)i;
        voice::Monitor* mon = monPrefix_.empty() ? nullptr : &voice::monitor(monPrefix_ + std::to_string(i + 1));
        auto queueAudio = [this, sp, idx](const float* a, size_t n, int rate) {
            sp->queue.insert(sp->queue.end(), a, a + n);
            while (sp->queue.size() > kMaxQueued) sp->queue.pop_front();
            if (sink_) sink_(idx, a, n, rate);
        };
        auto heard = [this, sp]() { sp->frames++; sp->lastVoice = nowSec(); };
        switch (x.dec) {
        case Dec::P25:
            x.rate = 8000;
            x.p25 = std::make_unique<P25Receiver>();
            x.p25->setSilent(true);
            if (mon) x.p25->setVoiceMonitor(mon);
            x.p25->configure(nr);
            x.p25->setSignalOffset(0);
            x.p25->reset();
            x.p25->setAudioTap([queueAudio](const float* a, size_t n) { queueAudio(a, n, 8000); });
            x.p25->setVoiceTap([heard](const uint8_t*, bool) { heard(); });
            break;
        case Dec::P25p2: {
            x.rate = 8000;
            x.p2 = std::make_unique<P25p2Receiver>();
            x.p2->setSilent(true);
            x.p2->configure(nr);
            x.p2->setSignalOffset(0);
            x.p2->setSystem(p2Seed_[0], p2Seed_[1], p2Seed_[2]);
            const int ls = r.slot >= 0 ? (r.slot & 1) + 1 : 0;
            x.p2->setListenSlot(ls);
            x.p2->startFollow((uint64_t)i + 1);
            if (r.tg >= 0 && ls) x.p2->setCallHint(ls, r.src, r.tg, true, r.freqHz);
            x.p2->setAudioTap([queueAudio](const float* a, size_t n) { queueAudio(a, n, 8000); });
            x.p2->setVoiceTap([heard, ls](int s, const uint8_t*, bool) { if (!ls || s == ls) heard(); });
            break;
        }
        case Dec::Dmr: {
            x.rate = 48000;
            x.dmr = std::make_unique<DmrReceiver>();
            x.dmr->setSilent(true);
            if (mon) x.dmr->setVoiceMonitor(mon);
            x.dmr->configureChannel(nr);
            const int ls = r.slot >= 0 ? (r.slot & 1) + 1 : 0;
            x.dmr->setListenSlot(ls);
            x.dmr->setAudioTap([queueAudio](const float* l, const float*, size_t n) { queueAudio(l, n, 48000); });
            x.dmr->setVoiceCallback([heard, ls](int s, int, const uint8_t*) { if (!ls || s == ls) heard(); });
            break;
        }
        case Dec::Nxdn:
            x.rate = 8000;
            x.nxdn = std::make_unique<NxdnReceiver>();
            x.nxdn->setSilent(true);
            if (mon) x.nxdn->setVoiceMonitor(mon);
            x.nxdn->configure(nr);
            x.nxdn->setSignalOffset(0);
            x.nxdn->reset();
            x.nxdn->setAudioTap([queueAudio](const float* a, size_t n) { queueAudio(a, n, 8000); });
            x.nxdn->setVoiceTap([heard](const uint8_t*) { heard(); });
            break;
        default: x.rate = 48000; break;
        }
        *slice = (int)i;
        return true;
    }
    return false;
}

void CaptureVoice::closeSlice(int slice) {
    if (slice < 0 || (size_t)slice >= s_.size()) return;
    resetSlice(*s_[(size_t)slice]);
}

bool CaptureVoice::sliceOpen(int i) const { return i >= 0 && (size_t)i < s_.size() && s_[(size_t)i]->open; }
bool CaptureVoice::sliceDigital(int i) const { return sliceOpen(i) && s_[(size_t)i]->dec != Dec::Analog; }
double CaptureVoice::sliceHz(int i) const { return sliceOpen(i) ? s_[(size_t)i]->hz : 0; }
Proto CaptureVoice::sliceProto(int i) const { return sliceOpen(i) ? s_[(size_t)i]->proto : Proto::Other; }
const P25Receiver* CaptureVoice::sliceP25(int i) const { return sliceOpen(i) ? s_[(size_t)i]->p25.get() : nullptr; }
uint64_t CaptureVoice::sliceVoiceFrames(int i) const { return sliceOpen(i) ? s_[(size_t)i]->frames : 0; }
double CaptureVoice::sliceRateHz(int i) const { return sliceOpen(i) ? s_[(size_t)i]->ddc.outRate() : 0; }
double CaptureVoice::cpuShare(int i) const {
    if (i < 0 || (size_t)i >= s_.size()) return 0;
    const double t = nowSec();
    return t > 0 ? s_[(size_t)i]->cpu / t : 0;
}
double CaptureVoice::cpuShareAll() const { double c = 0; for (size_t i = 0; i < s_.size(); i++) c += cpuShare((int)i); return c; }

bool CaptureVoice::sliceHearing(int i) const {
    if (!sliceOpen(i)) return false;
    const Slice& x = *s_[(size_t)i];
    if (x.dec == Dec::Analog) return x.hearing;
    return nowSec() - x.lastVoice < kVoiceHoldSec;
}

int CaptureVoice::pullAudio(int i, float* out, int maxFrames, int* rate) {
    if (!sliceOpen(i) || maxFrames <= 0) return 0;
    Slice& x = *s_[(size_t)i];
    const int n = std::min(maxFrames, (int)x.queue.size());
    for (int k = 0; k < n; k++) { out[k] = x.queue.front(); x.queue.pop_front(); }
    if (rate) *rate = x.rate;
    return n;
}

void CaptureVoice::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    // the DC spike: a narrow notch on the capture while a slice sits near the centre (automatic), or always / never
    bool nearDc = false;
    for (const auto& p : s_) if (p->open && std::fabs(p->hz - centre_) < kDcGuardHz) nearDc = true;
    dcOn_ = dcMode_ == 2 || (dcMode_ == 0 && nearDc);
    const cf32* in = x;
    if (dcOn_) {
        work_.assign(x, x + n);
        dc_->n.process(work_.data(), n);
        in = work_.data();
    }
    fed_ += n;
    for (size_t i = 0; i < s_.size(); i++) {
        Slice& v = *s_[i];
        if (!v.open) continue;
        const double t0 = nowCpu();
        const std::vector<cf32>& c = v.ddc.process(in, n);
        if (!c.empty()) {
            switch (v.dec) {
            case Dec::P25: if (v.p25) v.p25->feed(c.data(), c.size()); break;
            case Dec::P25p2: if (v.p2) v.p2->feed(c.data(), c.size()); break;
            case Dec::Dmr: if (v.dmr) v.dmr->feed(c.data(), c.size()); break;
            case Dec::Nxdn: if (v.nxdn) v.nxdn->feed(c.data(), c.size()); break;
            default: {
                v.disc.clear();
                v.fm.feed(c.data(), c.size(), v.disc);
                v.block.clear();
                for (float d : v.disc) {
                    // noise squelch: what is left of the discriminator above the speech band, over 10 ms. An empty channel gives wide FM noise, a carrier does not.
                    const double e = (double)d - v.nlp.run((double)d);
                    v.sum += e * e;
                    if (++v.count == kNoiseBlock) {
                        const double rms = std::sqrt(v.sum / kNoiseBlock);
                        v.sum = 0; v.count = 0;
                        if (rms < kNoiseRmsHz) { v.hang = kHangBlocks; v.hearing = true; v.noisy = 0; v.frames++; }
                        else { v.noisy++; if (v.hang > 0 && --v.hang == 0) v.hearing = false; }
                    }
                    const double a = (double)d / 5000.0;
                    v.dc += 0.03 * (a - v.dc);                  // about 240 Hz: takes the subaudible signalling out of the speech
                    // the hang keeps the call active (the pool) over a short fade, but the sound mutes after 20 ms of noise: else every call
                    // ended in 300 ms of full-scale noise once the carrier dropped (the squelch tail)
                    const float o = (float)(0.9 * v.lp.run(a - v.dc));
                    v.block.push_back(v.hearing && v.noisy < 2 ? o : 0.f);
                }
                if (!v.block.empty()) {
                    v.queue.insert(v.queue.end(), v.block.begin(), v.block.end());
                    while (v.queue.size() > kMaxQueued) v.queue.pop_front();
                    if (sink_) sink_((int)i, v.block.data(), v.block.size(), 48000);
                }
                break;
            }
            }
        }
        v.cpu += nowCpu() - t0;
    }
}

} // namespace tpool
} // namespace dect2
