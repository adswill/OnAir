// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// QO-100 narrowband: the decoders that run on the signals the tracker found (qo100_auto.h). One decoder channel per cluster of signals that fit in
// 3.4 kHz of audio: the ordinary FT8 / FT4 / SSTV / RTTY (HF digital), JS8Call, PSK and CW receivers, each fed the one decimated copy of the
// narrowband channel with its own offset, so a whole transponder costs one capture and a few receivers.
#include "dect2/qo100/qo100_auto.h"
#include "dect2/qo100/qo100_dsp.h"
#include "dect2/common/threadpool.h"
#include "dect2/cw/cw_rx.h"
#include "dect2/cw/cw_tel.h"
#include "dect2/engine/sig_ident.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/hfdig/hfdig_tel.h"
#include "dect2/js8/js8_rx.h"
#include "dect2/js8/js8_tel.h"
#include "dect2/psk/psk_rx.h"
#include "dect2/psk/psk_tel.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <map>

namespace dect2 {

namespace {
using clk = std::chrono::steady_clock;
constexpr int kKinds[] = {kQoDecFt8, kQoDecFt4, kQoDecJs8, kQoDecPsk, kQoDecCw, kQoDecSstv, kQoDecRtty};
constexpr size_t kKeepDecodes = 500;
constexpr double kWindowLo = 120, kWindowHi = 3700;     // audio passband of a channel that holds signals (Hz above its dial)

double marginOf(int kind) { return kind == kQoDecSstv ? 1200 : kind == kQoDecRtty ? 500 : 300; }
}

struct Qo100AutoDecoder::Impl {
    struct Slot {
        int id = 0, kind = 0;
        double dial = 0, offset = std::numeric_limits<double>::quiet_NaN(), lastNeeded = 0;
        std::unique_ptr<HfdigReceiver> hf;
        std::unique_ptr<PskReceiver> psk;
        std::unique_ptr<CwReceiver> cw;
        std::unique_ptr<Js8Receiver> js8;
        std::vector<int> tracks;
        double busy = 0, busyWin = 0;   // seconds spent feeding: since created, and since the last load figure
        float load = 0;
        uint64_t telSeq = 0, seen = 0, results = 0, imgSeq = 0;
        double snr = 0;
        void feed(const cf32* x, size_t n) {
            const auto t0 = clk::now();
            if (hf) hf->feed(x, n); else if (psk) psk->feed(x, n); else if (cw) cw->feed(x, n); else if (js8) js8->feed(x, n);
            const double d = std::chrono::duration<double>(clk::now() - t0).count();
            busy += d; busyWin += d;
        }
        void setOffset(double hz) {
            if (hf) hf->setSignalOffset(hz); else if (psk) psk->setSignalOffset(hz); else if (cw) cw->setSignalOffset(hz); else if (js8) js8->setSignalOffset(hz);
        }
    };
    std::vector<std::unique_ptr<Slot>> slots;
    double rate = 0;
    unsigned enabled = 0;
    int maxSlots = 6, nextSlot = 1, dropped = 0;
    float cpuLimit = 0.7f, cpuLoad = 0;
    double startUtc = std::numeric_limits<double>::quiet_NaN();
    double fedSec = 0, lastPlan = -1e9, lastCollect = -1e9, winSec = 0, lastCentre = 0, lastCorr = 0;
    std::map<int, double> dropUntil;       // track id -> receiver seconds
    // results (kept when a channel goes)
    std::vector<Qo100Decode> decodes;
    uint64_t decodeTotal = 0, decodeSeq = 0;
    struct TextKey { int slot, sig; bool operator<(const TextKey& o) const { return slot != o.slot ? slot < o.slot : sig < o.sig; } };
    std::map<TextKey, Qo100Text> texts;
    std::map<int, std::vector<std::shared_ptr<const HfdigSstvImage>>> images;   // per channel
    uint64_t imageSeq = 0;
    // the signal identifier
    std::vector<cf32> ring;
    size_t ringPos = 0, ringFill = 0;
    std::future<sigid::Result> job;
    int jobTrack = 0;
    std::map<int, double> askedAt;
    std::string doneName; int doneCls = 0, doneTrack = 0; bool doneReady = false;

    Slot* make(int kind, double dial, double now) {
        auto s = std::make_unique<Slot>();
        s->id = nextSlot++; s->kind = kind; s->dial = dial; s->lastNeeded = now;
        if (kind == kQoDecFt8 || kind == kQoDecFt4 || kind == kQoDecSstv || kind == kQoDecRtty) {
            s->hf = std::make_unique<HfdigReceiver>();
            s->hf->configure(rate);
            s->hf->setSilent(true);
            s->hf->setDecoders(kind == kQoDecSstv ? 2 : kind == kQoDecRtty ? 1 : 8);
            if (kind == kQoDecFt8 || kind == kQoDecFt4) {
                for (int k = 0; k < kFtxModes; k++) s->hf->ftx().setEnabled(k, k == (kind == kQoDecFt8 ? 0 : 1));
                s->hf->ftx().setMirrored(false);
                if (!std::isnan(startUtc)) s->hf->ftx().setStartTime(startUtc + fedSec);
            }
        } else if (kind == kQoDecPsk) {
            s->psk = std::make_unique<PskReceiver>();
            s->psk->configure(rate); s->psk->setSilent(true);
        } else if (kind == kQoDecCw) {
            s->cw = std::make_unique<CwReceiver>();
            s->cw->configure(rate);
        } else if (kind == kQoDecJs8) {
            s->js8 = std::make_unique<Js8Receiver>();
            s->js8->configure(rate); s->js8->setSilent(true);
            if (!std::isnan(startUtc)) s->js8->setStartTime(startUtc + fedSec);
        }
        slots.push_back(std::move(s));
        return slots.back().get();
    }

    void dropWeakest(double now) {
        if (slots.empty()) return;
        auto it = std::min_element(slots.begin(), slots.end(), [](const auto& a, const auto& b) { return a->snr < b->snr; });
        for (int id : (*it)->tracks) dropUntil[id] = now + 60;
        slots.erase(it);
        dropped++;
    }

    void askIdentifier(std::vector<Qo100Track>& tracks, double centreRf, double corr, double now) {
        if (job.valid()) {
            if (job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            const sigid::Result r = job.get();
            doneTrack = jobTrack;
            if (!r.cands.empty() && r.cands[0].confidence >= 0.4) {
                const sigid::Candidate& c = r.cands[0];
                int cls = 0;
                if (c.modeId == "hfdig" && c.subId == "ft8") cls = kQoSlot15;
                else if (c.modeId == "hfdig" && c.subId == "rtty") cls = kQoRtty;
                else if (c.modeId == "hfdig" && c.subId == "sstv") cls = kQoSstv;
                else if (c.modeId == "psk") cls = kQoPsk;
                else if (c.modeId == "cw") cls = kQoCw;
                else if (c.modeId == "js8") cls = kQoSlot15;
                doneName = c.name; doneCls = cls; doneReady = true;
            }
            return;
        }
        if (ringFill < (size_t)(0.4 * rate)) return;
        for (Qo100Track& t : tracks) {
            if (t.cls != kQoUnknown || !t.active || now - t.firstSec < 8 || t.snrDb < 12 || t.widthHz > 4000) continue;
            auto a = askedAt.find(t.id);
            if (a != askedAt.end() && now - a->second < 60) continue;
            askedAt[t.id] = now;
            const size_t n = (size_t)(0.4 * rate);
            std::vector<cf32> x(n);
            for (size_t i = 0; i < n; i++) x[i] = ring[(ringPos + ring.size() - n + i) % ring.size()];
            sigid::Options o;
            o.rfHz = centreRf + corr; o.usePriors = false;
            o.loHz = t.loHz - corr - centreRf - 150; o.hiHz = t.hiHz - corr - centreRf + 150;
            const double r = rate;
            jobTrack = t.id;
            job = std::async(std::launch::async, [x = std::move(x), r, o]() { return sigid::identify(x.data(), x.size(), r, o); });
            return;
        }
    }
};

Qo100AutoDecoder::Qo100AutoDecoder() : p_(new Impl) {}
Qo100AutoDecoder::~Qo100AutoDecoder() { if (p_->job.valid()) p_->job.wait(); }

void Qo100AutoDecoder::configure(double rateHz) {
    Impl& m = *p_;
    if (m.job.valid()) m.job.wait();
    m.job = std::future<sigid::Result>();
    m.rate = rateHz;
    m.slots.clear();
    m.ring.assign((size_t)(0.6 * rateHz) + 8, cf32(0, 0)); m.ringPos = m.ringFill = 0;
}
void Qo100AutoDecoder::reset() {
    Impl& m = *p_;
    if (m.job.valid()) m.job.wait();
    m.job = std::future<sigid::Result>();
    m.slots.clear(); m.fedSec = 0; m.lastPlan = m.lastCollect = -1e9; m.winSec = 0; m.cpuLoad = 0; m.dropUntil.clear(); m.askedAt.clear(); m.ringFill = 0;
}
void Qo100AutoDecoder::clearResults() {
    Impl& m = *p_;
    m.decodes.clear(); m.decodeTotal = 0; m.texts.clear(); m.images.clear(); m.imageSeq++;
}
void Qo100AutoDecoder::setEnabled(unsigned mask) { p_->enabled = mask; }
void Qo100AutoDecoder::setMaxSlots(int n) { p_->maxSlots = std::clamp(n, 1, 24); }
void Qo100AutoDecoder::setCpuLimit(float c) { p_->cpuLimit = std::clamp(c, 0.002f, 16.f); }
void Qo100AutoDecoder::setStartTime(double utc) { p_->startUtc = utc; }

bool Qo100AutoDecoder::takeIdentified(int& id, std::string& name, int& cls) {
    Impl& m = *p_;
    if (!m.doneReady) return false;
    m.doneReady = false;
    id = m.doneTrack; name = m.doneName; cls = m.doneCls;
    return true;
}

void Qo100AutoDecoder::plan(std::vector<Qo100Track>& tracks, double centreRf, double corr, double now) {
    Impl& m = *p_;
    if (m.rate <= 0) return;
    m.lastCentre = centreRf; m.lastCorr = corr;
    // follow the LNB correction: the offset of every channel
    for (auto& s : m.slots) {
        const double off = s->dial - corr - centreRf;
        if (std::isnan(s->offset) || std::fabs(off - s->offset) > 1.0) { s->offset = off; s->setOffset(off); }
    }
    if (now - m.lastPlan < 1.0) return;
    m.lastPlan = now;
    for (Qo100Track& t : tracks) t.decMask = 0;
    m.askIdentifier(tracks, centreRf, corr, now);
    for (auto it = m.dropUntil.begin(); it != m.dropUntil.end();) it = it->second < now ? m.dropUntil.erase(it) : std::next(it);
    for (int bit : kKinds) {
        // channels of a kind that was unticked go
        if (!(m.enabled & (unsigned)bit)) {
            m.slots.erase(std::remove_if(m.slots.begin(), m.slots.end(), [&](const auto& s) { return s->kind == bit; }), m.slots.end());
            continue;
        }
        const double hold = (bit == kQoDecFt8 || bit == kQoDecFt4 || bit == kQoDecJs8) ? 45 : 15;
        std::vector<Qo100Track*> el;
        for (Qo100Track& t : tracks) {
            if (!(qo100ClassDecoders(t.cls) & bit)) continue;
            if (t.snrDb < 6 || m.dropUntil.count(t.id)) continue;
            if (!t.active && now - t.lastSec > hold) continue;
            el.push_back(&t);
        }
        std::sort(el.begin(), el.end(), [](const Qo100Track* a, const Qo100Track* b) { return a->loHz < b->loHz; });
        std::vector<Impl::Slot*> mine;
        for (auto& s : m.slots) if (s->kind == bit) { mine.push_back(s.get()); s->tracks.clear(); s->snr = -1e9; }
        std::vector<Qo100Track*> rest;
        for (Qo100Track* t : el) {
            Impl::Slot* home = nullptr;
            for (Impl::Slot* s : mine) {
                const double w0 = s->dial + (bit == kQoDecSstv ? 0 : kWindowLo), w1 = s->dial + kWindowHi;
                if (t->loHz >= w0 - 100 && t->hiHz <= w1) { home = s; break; }
            }
            if (home) {
                home->tracks.push_back(t->id); home->lastNeeded = now; home->snr = std::max(home->snr, (double)t->snrDb);
                t->decMask |= bit;
            } else rest.push_back(t);
        }
        // new channels for the signals no channel covers: lowest first, everything within the window joins
        struct Cl { double dial; std::vector<Qo100Track*> mem; double snr = -1e9; };
        std::vector<Cl> cls;
        for (size_t i = 0; i < rest.size();) {
            Cl c; c.dial = rest[i]->loHz - marginOf(bit);
            size_t j = i;
            while (j < rest.size() && (j == i || (bit != kQoDecSstv && rest[j]->hiHz <= c.dial + kWindowHi))) {
                c.mem.push_back(rest[j]); c.snr = std::max(c.snr, (double)rest[j]->snrDb); j++;
            }
            cls.push_back(std::move(c));
            i = j;
        }
        std::sort(cls.begin(), cls.end(), [](const Cl& a, const Cl& b) { return a.snr > b.snr; });
        for (Cl& c : cls) {
            if ((int)m.slots.size() >= m.maxSlots) { m.dropped++; break; }
            Impl::Slot* s = m.make(bit, std::round(c.dial / 10) * 10, now);
            for (Qo100Track* t : c.mem) { s->tracks.push_back(t->id); t->decMask |= bit; }
            s->snr = c.snr;
            s->offset = s->dial - corr - centreRf; s->setOffset(s->offset);
        }
    }
    // idle channels go
    m.slots.erase(std::remove_if(m.slots.begin(), m.slots.end(), [&](const auto& s) { return s->tracks.empty() && now - s->lastNeeded > 20; }), m.slots.end());
}

void Qo100AutoDecoder::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.rate <= 0 || n == 0) return;
    // the identifier's window
    {
        const size_t R = m.ring.size();
        for (size_t i = 0; i < n; i++) { m.ring[m.ringPos] = x[i]; if (++m.ringPos == R) m.ringPos = 0; }
        m.ringFill = std::min(R, m.ringFill + n);
    }
    const double sec = (double)n / m.rate;
    m.fedSec += sec; m.winSec += sec;
    if (!m.slots.empty()) {
        if (m.slots.size() >= 2) ThreadPool::shared().parallelFor(m.slots.size(), [&](size_t i) { m.slots[i]->feed(x, n); });
        else m.slots[0]->feed(x, n);
    }
    if (m.winSec >= 2.0) {
        double tot = 0;
        for (auto& s : m.slots) { s->load = (float)(s->busyWin / m.winSec); tot += s->busyWin; s->busyWin = 0; }
        m.cpuLoad = (float)(tot / m.winSec);
        m.winSec = 0;
        if (m.cpuLoad > m.cpuLimit && !m.slots.empty()) m.dropWeakest(m.fedSec);
    }
}

void Qo100AutoDecoder::fill(Qo100AutoTelemetry& t) {
    Impl& m = *p_;
    if (m.fedSec - m.lastCollect >= 0.5) {
        m.lastCollect = m.fedSec;
        for (auto& sp : m.slots) {
            Impl::Slot& s = *sp;
            if (s.hf) {
                HfdigTelemetry tel;
                if (!s.hf->telemetry(tel, s.telSeq)) continue;
                s.telSeq = tel.seq;
                if (s.kind == kQoDecFt8 || s.kind == kQoDecFt4) {
                    const uint64_t fresh = tel.ftx.total > s.seen ? tel.ftx.total - s.seen : 0;
                    s.seen = tel.ftx.total;
                    const size_t take = (size_t)std::min<uint64_t>(fresh, tel.ftx.decodes.size());
                    for (size_t i = tel.ftx.decodes.size() - take; i < tel.ftx.decodes.size(); i++) {
                        const FtxDecode& d = tel.ftx.decodes[i];
                        Qo100Decode q;
                        q.seq = ++m.decodeSeq; q.utc = d.slotUtc; q.kind = s.kind; q.rfHz = s.dial + d.hz; q.snrDb = d.snrDb;
                        q.text = d.msg; q.call = d.call; q.grid = d.grid;
                        m.decodes.push_back(std::move(q)); m.decodeTotal++; s.results++;
                    }
                } else if (s.kind == kQoDecSstv) {
                    if (tel.sstv.imageSeq != s.imgSeq || m.images[s.id].empty()) {
                        s.imgSeq = tel.sstv.imageSeq;
                        std::vector<std::shared_ptr<const HfdigSstvImage>> v;
                        if (tel.sstv.image) v.push_back(tel.sstv.image);
                        for (const auto& h : tel.sstv.history) if (!tel.sstv.image || h->id != tel.sstv.image->id) v.push_back(h);
                        s.results = tel.sstv.picturesDone;
                        m.images[s.id] = std::move(v);
                        m.imageSeq++;
                    }
                } else if (s.kind == kQoDecRtty && !tel.rtty.text.empty()) {
                    Qo100Text& q = m.texts[{s.id, 0}];
                    q.kind = kQoDecRtty; q.rfHz = s.dial + tel.rtty.markHz; q.text = tel.rtty.text; q.snrDb = tel.rtty.quality * 20.f; q.lastSec = m.fedSec;
                    q.calls = qo100CallsFromText(q.text);
                    s.results = tel.rtty.chars;
                }
            } else if (s.js8) {
                Js8Telemetry tel;
                if (!s.js8->telemetry(tel, s.telSeq)) continue;
                s.telSeq = tel.seq;
                const uint64_t fresh = tel.blocksOk > s.seen ? tel.blocksOk - s.seen : 0;
                s.seen = tel.blocksOk;
                const size_t take = (size_t)std::min<uint64_t>(fresh, tel.decodes.size());
                for (size_t i = tel.decodes.size() - take; i < tel.decodes.size(); i++) {
                    const Js8Decode& d = tel.decodes[i];
                    Qo100Decode q;
                    q.seq = ++m.decodeSeq; q.utc = d.slotUtc; q.kind = kQoDecJs8; q.rfHz = s.dial + d.hz; q.snrDb = d.snrDb;
                    q.text = d.text; q.call = d.from; q.grid = d.grid;
                    m.decodes.push_back(std::move(q)); m.decodeTotal++; s.results++;
                }
            } else if (s.psk) {
                PskTelemetry tel;
                if (!s.psk->telemetry(tel, s.telSeq)) continue;
                s.telSeq = tel.seq;
                for (const PskSignal& g : tel.signals) {
                    if (g.text.empty()) continue;
                    Qo100Text& q = m.texts[{s.id, g.id}];
                    q.kind = kQoDecPsk; q.rfHz = s.dial + g.offsetHz; q.text = g.text; q.snrDb = g.snrDb; q.lastSec = m.fedSec; q.calls = g.calls;
                    s.results = std::max<uint64_t>(s.results, g.chars);
                }
            } else if (s.cw) {
                CwTelemetry tel;
                if (!s.cw->telemetry(tel, s.telSeq)) continue;
                s.telSeq = tel.seq;
                for (const CwSignal& g : tel.signals) {
                    if (g.text.empty()) continue;
                    Qo100Text& q = m.texts[{s.id, g.id}];
                    q.kind = kQoDecCw; q.rfHz = s.dial + g.offsetHz; q.text = g.text; q.snrDb = g.snrDb; q.lastSec = m.fedSec; q.calls = g.calls;
                    s.results = std::max<uint64_t>(s.results, g.chars);
                }
            }
        }
        if (m.decodes.size() > kKeepDecodes) m.decodes.erase(m.decodes.begin(), m.decodes.end() - (long)kKeepDecodes);
        while (m.texts.size() > 60) m.texts.erase(m.texts.begin());
    }
    t.decoders = m.enabled; t.maxSlots = m.maxSlots; t.cpuLoad = m.cpuLoad; t.cpuLimit = m.cpuLimit; t.dropped = m.dropped;
    t.slots.clear();
    for (auto& sp : m.slots) {
        Qo100Slot q;
        q.id = sp->id; q.kind = sp->kind; q.dialHz = sp->dial; q.tracks = (int)sp->tracks.size(); q.load = sp->load; q.results = sp->results;
        t.slots.push_back(q);
    }
    t.decodes = m.decodes; t.decodeTotal = m.decodeTotal;
    t.texts.clear();
    for (const auto& kv : m.texts) t.texts.push_back(kv.second);
    t.images.clear();
    for (const auto& kv : m.images) for (const auto& im : kv.second) t.images.push_back(im);
    std::sort(t.images.begin(), t.images.end(), [](const auto& a, const auto& b) { return a->unixTime > b->unixTime; });
    if (t.images.size() > 16) t.images.resize(16);
    t.imageSeq = m.imageSeq;
}

} // namespace dect2
