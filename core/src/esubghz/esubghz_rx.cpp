// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Flipper Zero Sub-GHz chat receiver, see esubghz_rx.h
#include "dect2/esubghz/esubghz_rx.h"
#include "dect2/esubghz/esubghz_tel.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>

namespace dect2 {

using math::kPi;

namespace {

constexpr double kChanRate = 200e3;     // the channel stream, about
constexpr double kChanCutHz = 65e3;     // its low-pass: the signal (+-25 kHz) and +-15 kHz of carrier offset pass
constexpr double kMaxBurstSec = 0.25;
constexpr size_t kMinPreambleBits = 8;  // alternating bits right before the sync word

int64_t wallNow() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string hexOf(const uint8_t* p, size_t n) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < n; i++) { if (i) s += ' '; s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

struct Found {
    std::vector<uint8_t> payload;
    bool crcOk = false, inverted = false;
    size_t endBit = 0;
};

// Packets in one stream of bit decisions: sync word after alternating preamble bits, a length that fits, the CRC
std::vector<Found> findPackets(const std::vector<uint8_t>& bits) {
    std::vector<Found> out;
    const uint16_t sync = (uint16_t)(esubghz::kSync0 << 8 | esubghz::kSync1);
    for (size_t p = kMinPreambleBits; p + 16 + 8 * 3 <= bits.size(); p++) {
        uint16_t v = 0;
        for (size_t i = 0; i < 16; i++) v = (uint16_t)(v << 1 | bits[p + i]);
        const bool inv = v == (uint16_t)~sync;
        if (v != sync && !inv) continue;
        int flaws = 0;
        for (size_t i = p - kMinPreambleBits + 1; i < p; i++) if (bits[i] == bits[i - 1]) flaws++;
        if (flaws > 1) continue;
        const size_t q = p + 16;
        auto octet = [&](size_t k) { int b = 0; for (size_t i = 0; i < 8; i++) b = b << 1 | (bits[q + k * 8 + i] ^ (inv ? 1 : 0)); return (uint8_t)b; };
        const uint8_t len = octet(0);
        if (len == 0 || len > 61 || q + (size_t)(len + 3) * 8 > bits.size()) continue;
        std::vector<uint8_t> body((size_t)len + 1);
        for (size_t k = 0; k <= len; k++) body[k] = octet(k);
        const uint16_t crc = (uint16_t)(octet((size_t)len + 1) << 8 | octet((size_t)len + 2));
        Found f;
        f.crcOk = esubghz::crc16(body.data(), body.size()) == crc;
        f.inverted = inv;
        f.payload.assign(body.begin() + 1, body.end());
        f.endBit = q + (size_t)(len + 3) * 8;
        out.push_back(std::move(f));
        if (out.back().crcOk) p = out.back().endBit - 1;   // the next packet of the burst (if any) starts after this one
    }
    return out;
}

} // namespace

struct EsubghzReceiver::Impl {
    std::mutex mu;
    std::atomic<double> rate{0}, offset{0};
    std::atomic<bool> resetReq{true};
    std::function<void(const std::string&)> log;
    // front end (analysis thread)
    cf32 rot{1, 0}, step{1, 0};
    int D = 1;
    double chanRate = kChanRate;
    std::vector<float> taps;
    std::vector<cf32> ring;
    size_t rpos = 0;
    int dcount = 0;
    float pe = 0, nz = 0;
    int nzInit = 0, above = 0, below = 0;
    float nzMin = 0;
    int initRise = 0;
    bool inBurst = false;
    std::vector<cf32> hist;
    size_t hpos = 0, hfill = 0;
    std::vector<cf32> cur;
    size_t s0 = 0;
    float nzAtStart = 0;
    std::atomic<uint64_t> nSamp{0};   // read by telemetry() and noteSent() on other threads
    // message being joined (under mu: clearResults() drops it from another thread)
    std::vector<uint8_t> pending;
    int pendingPackets = 0;
    double lastPacketSec = -1e9;
    float pendLevel = -200, pendSnr = 0;
    double pendCfo = 0;
    // reports (under mu)
    uint64_t seq = 0;
    double nextReport = 0.25;
    int state = 0;
    double lastGood = -1e9, lastBurst = -1e9;
    float snrEma = 0, levelEma = -200;
    bool snrInit = false;
    double cfoEma = 0;
    uint64_t ok = 0, bad = 0, bursts = 0, messages = 0, decrypted = 0, unreadable = 0, replays = 0, sent = 0;
    std::deque<EsgChatLine> chat;
    std::deque<EsgPacket> packets;
    esubghz::Key key;
    esubghz::ReplayGuard guard;

    double now() const { return rate > 0 ? (double)nSamp / rate : 0; }

    void rebuild() {
        const double fs = rate;
        D = std::max(1, (int)std::lround(fs / kChanRate));
        chanRate = fs / D;
        const double w = -2 * kPi * offset.load() / fs;
        step = cf32((float)std::cos(w), (float)std::sin(w));
        rot = cf32(1, 0);
        const int N = 12 * D + 1;
        taps.assign((size_t)N, 0);
        const double fc = std::min(kChanCutHz, 0.45 * chanRate) / fs;
        double sum = 0;
        for (int i = 0; i < N; i++) {
            const double x = i - (N - 1) / 2.0;
            const double s = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
            const double win = 0.54 - 0.46 * std::cos(2 * kPi * i / (N - 1));
            taps[(size_t)i] = (float)(s * win);
            sum += s * win;
        }
        for (auto& t : taps) t = (float)(t / sum);
        ring.assign((size_t)N, cf32(0, 0));
        rpos = 0; dcount = 0;
        pe = nz = 0; nzMin = 0; initRise = 0; nzInit = above = below = 0;
        inBurst = false;
        hist.assign(256, cf32(0, 0));
        hpos = hfill = 0;
        cur.clear();
        finishMessage();   // the stream starts again: what was being joined is complete
        resetReq = false;
    }

    void burstDone() {
        const double fs = chanRate;
        const size_t L = cur.size();
        const double sps = fs / esubghz::kBaud;
        if (L < (size_t)(40 * sps)) return;   // shorter than a preamble, sync word and length
        std::vector<float> d(L, 0);
        for (size_t i = 1; i < L; i++) d[i] = (float)(std::arg(cur[i] * std::conj(cur[i - 1])) * fs / (2 * kPi));
        // carrier offset: the middle of the two frequencies of the FSK (10th and 90th percentile of the burst body), whatever the data
        const size_t a = s0 + (size_t)(0.0004 * fs);
        const size_t e = L > 80 ? L - 60 : L;
        if (e <= a + (size_t)(30 * sps)) return;
        std::vector<float> srt(d.begin() + (long)a, d.begin() + (long)e);
        std::nth_element(srt.begin(), srt.begin() + (long)(srt.size() / 10), srt.end());
        const float lo = srt[srt.size() / 10];
        std::nth_element(srt.begin(), srt.begin() + (long)(srt.size() * 9 / 10), srt.end());
        const float hi = srt[srt.size() * 9 / 10];
        const double cfo = 0.5 * (lo + hi);
        for (auto& v : d) v = (float)(v - cfo);
        double pw = 0;
        for (size_t i = a; i < e; i++) pw += std::norm(cur[i]);
        pw /= (double)(e - a);
        const float levelDb = (float)(10 * std::log10(pw + 1e-20));
        const float snr = (float)(10 * std::log10(std::max(1e-3, pw / std::max(1e-20f, nzAtStart) - 1)));
        // bit timing: the phase where one symbol's average of the discriminator is largest over the first symbols
        std::vector<double> ps(L + 1, 0);
        for (size_t i = 0; i < L; i++) ps[i + 1] = ps[i] + d[i];
        const size_t w = (size_t)std::max(1.0, std::floor(sps * 0.8));
        auto m = [&](double pos) { const long i = std::lround(pos); if (i < (long)w || i >= (long)L) return 0.0; return (ps[(size_t)i] - ps[(size_t)i - w]) / (double)w; };
        double best = -1, bphi = 0;
        for (double phi = 0; phi < sps; phi += 0.5) {
            double s = 0;
            for (int k = 1; k <= 24; k++) s += std::fabs(m((double)a + phi + k * sps));
            if (s > best) { best = s; bphi = phi; }
        }
        std::vector<uint8_t> bits;
        for (size_t k = 0;; k++) {
            const double pos = (double)a + bphi + (double)k * sps;
            if (pos >= (double)L - 1) break;
            bits.push_back(m(pos) > 0 ? 1 : 0);
        }
        const std::vector<Found> found = findPackets(bits);
        const double t = now();
        std::lock_guard<std::mutex> lk(mu);
        for (const Found& f : found) {
            EsgPacket pk;
            pk.wallTime = wallNow(); pk.sec = t; pk.len = (int)f.payload.size(); pk.crcOk = f.crcOk; pk.inverted = f.inverted;
            pk.levelDb = levelDb; pk.snrDb = snr; pk.cfoHz = cfo; pk.hex = hexOf(f.payload.data(), f.payload.size());
            packets.push_front(pk);
            if (packets.size() > kEsgPacketMax) packets.pop_back();
            if (!f.crcOk) { bad++; continue; }
            ok++;
            lastGood = t;
            cfoEma = cfo; levelEma = levelDb;
            if (!snrInit) { snrEma = snr; snrInit = true; } else snrEma = 0.8f * snrEma + 0.2f * snr;
            if (pending.empty()) { pendLevel = levelDb; pendSnr = snr; pendCfo = cfo; }
            pending.insert(pending.end(), f.payload.begin(), f.payload.end());
            pendingPackets++;
            lastPacketSec = t;
            if (pending.size() >= esubghz::kMaxMessage) finishMessageLocked();
        }
    }

    void finishMessage() { std::lock_guard<std::mutex> lk(mu); finishMessageLocked(); }

    void finishMessageLocked() {
        if (pending.empty()) return;
        const esubghz::Message m = esubghz::decodeMessage(key.on ? &key : nullptr, pending.data(), pending.size());
        EsgChatLine c;
        c.wallTime = wallNow(); c.sec = now(); c.dir = 0;
        c.hex = hexOf(pending.data(), pending.size());
        c.packets = pendingPackets; c.levelDb = pendLevel; c.snrDb = pendSnr; c.cfoHz = pendCfo;
        if (m.ok && m.encrypted) {
            c.runId = m.runId; c.counter = m.counter;
            if (guard.accept(m.runId, m.counter)) { c.kind = 1; decrypted++; } else { c.kind = 3; replays++; }
            c.text = m.text;
        } else if (m.ok) {
            c.kind = 0; c.text = m.text;
        } else if (m.looksEncrypted) {
            c.kind = 2; unreadable++;
        } else c.kind = 4;
        messages++;
        chat.push_back(c);
        if (chat.size() > kEsgChatMax) chat.pop_front();
        if (log) log(c.kind == 0 || c.kind == 1 ? "Flipper chat: " + esubghz::displayText(c.text) : c.kind == 2 ? std::string("Flipper chat: an encrypted message that this key does not open") : std::string("Flipper chat: a repeated or unreadable message"));
        pending.clear();
        pendingPackets = 0;
    }

    void feedSamples(const cf32* x, size_t n) {
        const size_t N = taps.size();
        for (size_t i = 0; i < n; i++) {
            rot *= step;
            if ((i & 1023) == 1023) rot /= std::abs(rot);
            ring[rpos] = x[i] * rot;
            rpos = rpos + 1 == N ? 0 : rpos + 1;
            if (++dcount < D) continue;
            dcount = 0;
            float re = 0, im = 0;
            size_t k = rpos;
            for (size_t t = 0; t < N; t++) { const cf32 v = ring[k]; re += v.real() * taps[t]; im += v.imag() * taps[t]; k = k + 1 == N ? 0 : k + 1; }
            const cf32 y(re, im);
            const float p = re * re + im * im;
            pe += 0.2f * (p - pe);
            if (nzInit < 2000) {
                // the first 2000 samples set the floor; a burst in them (a transmitter keyed with the recording, 5 ms in) once became the
                // floor and was never heard: a rise far over the quietest power so far ends the start-up there
                nzMin = nzInit == 0 ? pe : std::min(nzMin, pe);
                initRise = nzInit > 20 && pe > 30 * nzMin + 1e-9f ? initRise + 1 : 0;
                if (initRise >= 6) nzInit = 2000;   // the floor stays the mean of what came before the rise
                else if (initRise == 0) nz += (pe - nz) / (float)(++nzInit);
                else ++nzInit;
            }
            nz = std::max(nz, 1e-10f);   // a recording with digital silence: bursts still end
            if (!inBurst) {
                if (pe < 3 * nz) nz += 0.002f * (pe - nz);
                hist[hpos] = y; hpos = (hpos + 1) % hist.size(); hfill = std::min(hfill + 1, hist.size());
                if (nzInit >= 2000 && pe > 6 * nz) above++; else above = 0;
                if (above >= 6) {
                    inBurst = true; below = 0; nzAtStart = nz;
                    const size_t pre = std::min<size_t>(hfill, 100);
                    cur.clear();
                    for (size_t j = 0; j < pre; j++) cur.push_back(hist[(hpos + hist.size() - pre + j) % hist.size()]);
                    s0 = pre >= 12 ? pre - 12 : 0;
                }
            } else {
                cur.push_back(y);
                if (pe < 2.5f * nzAtStart) below++; else below = 0;
                if (below >= 80 || cur.size() > (size_t)(kMaxBurstSec * chanRate)) {
                    inBurst = false; above = 0; hfill = 0;
                    { std::lock_guard<std::mutex> lk(mu); bursts++; lastBurst = now(); }
                    burstDone();
                    cur.clear();
                }
            }
        }
    }
};

EsubghzReceiver::EsubghzReceiver() : p_(std::make_unique<Impl>()) {}
EsubghzReceiver::~EsubghzReceiver() = default;

void EsubghzReceiver::configure(double r) { p_->rate = r; p_->resetReq = true; }
void EsubghzReceiver::setSignalOffset(double hz) { if (hz != p_->offset.exchange(hz)) p_->resetReq = true; }
bool EsubghzReceiver::ready() const { return p_->rate >= esubghzTuning().minSampleRate - 1; }
void EsubghzReceiver::reset() { p_->resetReq = true; }
void EsubghzReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void EsubghzReceiver::setKey(const esubghz::Key& k) { std::lock_guard<std::mutex> lk(p_->mu); p_->key = k; }

void EsubghzReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->chat.clear(); p_->packets.clear();
    // a message half received before Clear would otherwise come out afterwards, or be joined to the next one. The replay guard is kept: a
    // Flipper's counter only goes up, so it rejects nothing genuine, and forgetting it would let a recording made before Clear play again.
    p_->pending.clear(); p_->pendingPackets = 0; p_->lastPacketSec = -1e9;
    p_->ok = p_->bad = p_->bursts = p_->messages = p_->decrypted = p_->unreadable = p_->replays = p_->sent = 0;
    p_->seq++;
}

void EsubghzReceiver::noteSent(const std::string& line, bool encrypted, size_t octets) {
    std::lock_guard<std::mutex> lk(p_->mu);
    EsgChatLine c;
    c.wallTime = wallNow(); c.sec = p_->now(); c.dir = 1; c.kind = encrypted ? 1 : 0; c.text = line;
    c.packets = (int)((octets + esubghz::kMaxChunk - 1) / esubghz::kMaxChunk);
    p_->chat.push_back(c);
    if (p_->chat.size() > kEsgChatMax) p_->chat.pop_front();
    p_->sent++;
    p_->seq++;
}

void EsubghzReceiver::feed(const cf32* x, size_t n) {
    Impl* p = p_.get();
    if (!ready()) return;
    if (p->resetReq) p->rebuild();
    p->feedSamples(x, n);
    p->nSamp += n;
    {
        std::lock_guard<std::mutex> lk(p->mu);
        if (!p->pending.empty() && p->now() - p->lastPacketSec > esubghz::kMessageGapSec) p->finishMessageLocked();
    }
    while (p->now() >= p->nextReport) {
        std::lock_guard<std::mutex> lk(p->mu);
        const int old = p->state;
        p->state = p->nextReport - p->lastGood < 5.0 ? 2 : (p->nextReport - p->lastBurst < 10.0 ? 1 : 0);
        p->seq++;
        p->nextReport += 0.25;
        if (p->log && p->state == 2 && old != 2) p->log("Flipper chat packets are coming in");
    }
}

bool EsubghzReceiver::telemetry(EsubghzTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl* p = p_.get();
    if (p->seq <= lastSeq) return false;
    out = EsubghzTelemetry();
    out.seq = p->seq; out.state = p->state; out.timeSec = p->now(); out.inputRate = p->rate; out.signalOffsetHz = p->offset;
    out.cfoHz = p->cfoEma; out.snrDb = p->snrInit ? p->snrEma : 0; out.levelDb = p->levelEma;
    out.dataValid = p->state == 2; out.blocksOk = p->ok; out.blocksBad = p->bad; out.bursts = p->bursts;
    out.messages = p->messages; out.decrypted = p->decrypted; out.unreadable = p->unreadable; out.replays = p->replays; out.sent = p->sent;
    out.keyOn = p->key.on;
    out.chat.assign(p->chat.begin(), p->chat.end());
    out.packets.assign(p->packets.begin(), p->packets.end());
    return true;
}

ModeTuning esubghzTuning() {
    ModeTuning t;
    t.stdMode = 206; t.id = "esubghz"; t.name = "Flipper Zero Sub-GHz chat";
    t.minMhz = 300; t.maxMhz = 928; t.defMhz = esubghz::kDefaultHz / 1e6;   // the CC1101's bands (300-348, 387-464, 779-928 MHz)
    t.sampleRate = 2e6;
    t.basebandHz = 0;
    t.bandwidthMhz = 0.2;
    t.minSampleRate = 200e3;
    t.tuneOffsetHz = 300e3;   // the radio sits 300 kHz above the chat frequency: the channel stays clear of the DC spike
    return t;
}

} // namespace dect2
