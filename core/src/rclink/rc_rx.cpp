// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radio-control and mesh link monitor; see rc_rx.h and docs/MESH-RC-3.8.md.
#include "dect2/rclink/rc_rx.h"
#include "dect2/rclink/rc_fsk.h"
#include "dect2/mesh/mesh_lora.h"
#include "../loraraw/loraraw_bank.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <tuple>
#include <mutex>

namespace dect2 {

using lora::detail::Bank;
using namespace rclink;

static int64_t wallNow() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

const std::vector<RcElrsDomain>& rcElrsDomains() {
    static const std::vector<RcElrsDomain> d = {
        {"AU915", 915500000, 926900000, 20}, {"FCC915", 903500000, 926900000, 40}, {"EU868", 863275000, 869575000, 13}, {"IN866", 865375000, 866950000, 4},
        {"AU433", 433420000, 434420000, 3},  {"EU433", 433100000, 434450000, 3},   {"US433", 433250000, 438000000, 8},  {"US433W", 423500000, 438000000, 20},
        {"TH920", 920500000, 924700000, 8},
    };
    return d;
}

ModeTuning rcLinkTuning() {
    ModeTuning t;
    t.stdMode = 362;
    t.id = "rclink";
    t.name = "RC and mesh links";
    t.minMhz = 137; t.maxMhz = 2500; t.defMhz = 868;
    t.sampleRate = 8e6;
    t.bandwidthMhz = 8;
    t.minSampleRate = 1e6;
    return t;
}

namespace {
struct FskDef {
    int proto = 0;
    double rate = 0;
    std::vector<uint8_t> pattern;
    int maxErrors = 1;
    size_t maxBytes = 0;
    bool msbFirst = true;
    int kind = 0;       // 0 SiK, 1 FrSky D8, 2 FrSky D16, 3 FlySky
};

struct LoraChain {
    std::unique_ptr<Bank> normal, inverted;
    double centreOffsetHz = 0;
    int proto = 0;
};
} // namespace

struct RcLinkReceiver::Impl {
    std::mutex m;
    std::function<void(const std::string&)> log;
    double fs = 0, offset = 0, centre = 0;
    int protocols = Reticulum;
    std::string phrase;
    int domain = 2;
    double loraBw = 0;
    int loraSf = 0, sikKbps = 0;
    bool dirty = true;

    BurstFinder finder;
    std::vector<FskDef> defs;
    std::vector<LoraChain> chains;
    std::map<int, RnodeReassembly> reas;
    std::map<int, std::vector<uint8_t>> sikTail;
    ElrsLink elrs;
    int64_t nIn = 0;
    std::vector<cf32> conj;

    RcTelemetry tel;
    uint64_t seq = 0;

    void addFrame(const Decoded& d, double freqHz, float snr, const uint8_t* raw, size_t n) {
        std::lock_guard<std::mutex> g(m);
        const int64_t now = wallNow();
        RcFrame f;
        f.utc = now;
        f.timeSec = fs > 0 ? (double)nIn / fs : 0;
        f.proto = d.proto;
        f.id = d.id;
        f.type = d.type;
        f.detail = d.detail;
        f.hex = hexOf(raw, std::min<size_t>(n, 64));
        f.freqHz = freqHz;
        f.snrDb = snr;
        f.encrypted = d.encrypted;
        tel.frames.insert(tel.frames.begin(), std::move(f));
        if (tel.frames.size() > kRcFramesMax) tel.frames.pop_back();
        RcDevice* dev = nullptr;
        for (RcDevice& x : tel.devices) if (x.proto == d.proto && x.id == d.id) { dev = &x; break; }
        if (!dev) {
            RcDevice nd;
            nd.proto = d.proto; nd.id = d.id; nd.firstSeen = now;
            tel.devices.insert(tel.devices.begin(), std::move(nd));
            if (tel.devices.size() > kRcDevicesMax) tel.devices.pop_back();
            dev = &tel.devices.front();
            if (log) log(std::string(protoName(d.proto)) + ": new link " + d.id);
        }
        dev->count++;
        dev->lastSeen = now;
        dev->lastType = d.type;
        dev->lastDetail = d.detail;
        if (d.hasPos) { dev->hasPos = true; dev->lat = d.lat; dev->lon = d.lon; dev->altM = d.altM; }
        tel.blocksOk++;
        tel.state = 2;
        tel.snrDb = snr;
        tel.dataValid = true;
        seq++;
    }

    void rebuild() {
        dirty = false;
        finder.configure(fs);
        dsmPend.clear(); dsmDone.clear(); dsmDoneFrom = dsmDoneTo = -1;
        defs.clear();
        chains.clear();
        if (protocols & Sik) {
            static const int kAir[] = {2, 4, 8, 16, 19, 24, 32, 48, 64, 96, 128, 192, 250};
            for (int k : kAir) if (sikKbps == 0 || k == sikKbps) {
                if (k * 1e3 * 2.5 > fs) continue;
                FskDef d; d.proto = Sik; d.rate = k * 1e3; d.kind = 0; d.maxBytes = 300; d.maxErrors = 1;
                d.pattern = bitsOfBytes({0xAA, 0xAA, 0x2D, 0xD4});
                defs.push_back(d);
            }
        }
        if (protocols & Frsky) {
            for (auto [r, kind, mb] : std::vector<std::tuple<double, int, size_t>>{{31044, 1, 22}, {70.0e3, 2, 40}, {99.97e3, 2, 40}}) {
                if (r * 2.5 > fs) continue;
                FskDef d; d.proto = Frsky; d.rate = r; d.kind = kind; d.maxBytes = mb; d.maxErrors = 1;
                d.pattern = bitsOfBytes({0xAA, 0xAA, 0xD3, 0x91});
                defs.push_back(d);
            }
        }
        if ((protocols & Flysky) && fs >= 2.5e6) {
            for (bool msb : {true, false}) {
                FskDef d; d.proto = Flysky; d.rate = 500e3; d.kind = 3; d.maxBytes = 40; d.maxErrors = 1; d.msbFirst = msb;
                d.pattern = bitsOfBytes({0x54, 0x75, 0xC5, 0x2A}, msb);
                defs.push_back(d);
            }
        }
        if ((protocols & Sfhss) && 128143.0 * 2.5 <= fs) {   // Futaba S-FHSS: CC2500 2-FSK 128 kbit/s, 38 kHz deviation, 4 preamble bytes, sync D3 91, 13 + 2 bytes
            FskDef d; d.proto = Sfhss; d.rate = 128143; d.kind = 4; d.maxBytes = 15; d.maxErrors = 1;
            d.pattern = bitsOfBytes({0xAA, 0xAA, 0xD3, 0x91});
            defs.push_back(d);
        }
        if (protocols & Reticulum) {
            std::vector<double> bws = {125000, 250000, 62500};
            std::vector<int> sfs = {7, 8, 9, 10, 11};
            if (loraBw > 0) bws = {loraBw};
            if (loraSf > 0) sfs = {loraSf};
            for (double bw : bws) {
                if (bw * 2 > fs) continue;
                std::vector<lora::Params> ps;
                for (int sf : sfs) {
                    lora::Params p;
                    p.sf = sf; p.bwHz = bw; p.cr = 5; p.preamble = 8; p.syncWord = 0x12;
                    p.ldro = lora::autoLdro(sf, bw); p.crc = true;
                    ps.push_back(p);
                }
                LoraChain c;
                c.proto = Reticulum;
                c.centreOffsetHz = offset;
                c.normal = std::make_unique<Bank>(fs, offset, bw, ps);
                chains.push_back(std::move(c));
            }
        }
        if (protocols & Elrs) {
            const auto& dom = rcElrsDomains();
            const RcElrsDomain& dm = dom[std::clamp(domain, 0, (int)dom.size() - 1)];
            const double mid = 0.5 * (dm.startHz + dm.stopHz);
            const double ref = centre > 0 ? centre : mid + offset;
            std::vector<lora::Params> ps;
            struct Row { int sf, cr, pre; size_t len; };
            for (const Row& r : std::vector<Row>{{6, 7, 8, 8}, {7, 7, 8, 8}, {8, 7, 10, 8}, {9, 7, 10, 8}, {6, 8, 8, 13}}) {
                lora::Params p;
                p.sf = r.sf; p.bwHz = 500000; p.cr = r.cr; p.preamble = r.pre; p.syncWord = 0x12; p.ldro = false; p.crc = false;
                p.implicitHdr = true; p.implicitLen = (int)r.len;
                ps.push_back(p);
            }
            const double step = dm.channels > 1 ? (dm.stopHz - dm.startHz) / (dm.channels - 1) : 0;
            int made = 0;
            for (int i = 0; i < dm.channels && made < 16; i++) {
                const double off = dm.startHz + i * step - ref;
                if (std::fabs(off) > 0.5 * fs - 300e3) continue;
                LoraChain c;
                c.proto = Elrs;
                c.centreOffsetHz = off;
                c.normal = std::make_unique<Bank>(fs, off, 500000, ps);
                c.inverted = std::make_unique<Bank>(fs, -off, 500000, ps);
                chains.push_back(std::move(c));
                made++;
            }
            if (!phrase.empty()) { elrs.known = true; elrs.phrase = true; elrs.uid = elrsUidFromPhrase(phrase); elrs.uid4 = elrs.uid[4]; elrs.uid5 = elrs.uid[5]; }
            else { elrs.phrase = false; }
        }
    }

    // Spektrum DSM2 / DSMX: 1 Mchip/s, 64 chip 8DR symbols. The SOP code (and so the pn row and SOP column) is not known in advance: all 40 are tried.
    // The spectrum of a DSSS burst is lumpy: the finder may report pieces of it, in frequency and in time. Each piece queues a search window from one
    // packet length before it to one packet length after it (searched once those samples are in); the piece's centre, the nearest 1 MHz channel (when the
    // radio's frequency is known) and +-400 kHz are tried. Pieces that start inside a packet already decoded are dropped.
    std::vector<Burst> dsmPend;
    int64_t dsmDoneFrom = -1, dsmDoneTo = -1;
    std::vector<std::pair<int64_t, int64_t>> dsmDone;   // the packets decoded lately: first chip after the SOP, end
    int64_t dsmNeed() const { return (int64_t)((24 + 128 + 19 * 64 + 96) * fs / 1e6); }
    void dsmDrain(bool all) {
        std::sort(dsmPend.begin(), dsmPend.end(), [](const Burst& a, const Burst& b) { return a.start < b.start; });
        size_t i = 0;
        for (; i < dsmPend.size(); i++) {
            const Burst& b0 = dsmPend[i];
            if (!all && b0.start + dsmNeed() > finder.end()) break;
            if (b0.start >= dsmDoneFrom && b0.start <= dsmDoneTo) continue;
            std::vector<double> fq = {b0.freqHz};
            if (centre > 0) { const double ch = 2400e6 + std::round((centre + b0.freqHz - 2400e6) / 1e6) * 1e6 - centre; if (std::fabs(ch - b0.freqHz) > 50e3) fq.push_back(ch); }
            fq.push_back(b0.freqHz - 400e3);
            fq.push_back(b0.freqHz + 400e3);
            for (double f : fq) {
                Burst b = b0;
                b.freqHz = f;
                b.start = std::max(finder.begin(), b0.start - dsmNeed());
                b.end = std::min(finder.end(), b0.start + dsmNeed());
                if (dsmTry(b)) break;
            }
        }
        dsmPend.erase(dsmPend.begin(), dsmPend.begin() + (std::ptrdiff_t)i);
    }
    bool dsmTry(const Burst& b) {
        double actual = 0;
        const std::vector<float> disc = discriminate(finder, b, 1.3e6, 4e6, &actual);
        if (actual < 3.5e6 || disc.empty()) return false;
        const double sps = actual / 1e6;
        SyncHit best;
        int bestRow = -1, bestCol = -1;
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 8; col++) {
                const DsmCodes k = dsmCodes(row, col);
                std::vector<uint8_t> pat;
                for (int j = 0; j < 64; j++) pat.push_back((uint8_t)((k.sop >> j) & 1));
                for (int j = 0; j < 64; j++) pat.push_back((uint8_t)(!((k.sop >> j) & 1)));
                const SyncHit h = findSync(disc, sps, pat, 16, false);
                if (h.found && (!best.found || h.errors < best.errors)) { best = h; bestRow = row; bestCol = col; }
            }
        if (!best.found) return false;
        const DsmCodes k = dsmCodes(bestRow, bestCol);
        const std::vector<uint8_t> chips = sliceBytes(disc, sps, best, 19 * 8, false);
        if (chips.size() < 19 * 8) return false;
        uint8_t v[19];
        for (int s = 0; s < 19; s++) {
            uint64_t c = 0;
            for (int i = 0; i < 8; i++) c |= (uint64_t)chips[(size_t)(8 * s + i)] << (8 * i);
            int e = 0;
            const int x = dsmDespread(k, c, &e);
            if (x < 0 || e > 14) return false;
            v[s] = (uint8_t)x;
        }
        if (v[0] != 16) return false;
        if (std::fabs(best.offset * actual / (2 * M_PI)) > 600e3) return false;   // heard through the filter's skirt (an alias): a closer centre is tried
        const int64_t at = b.start + (int64_t)std::llround(best.position * std::max(1.0, std::floor(fs / 4e6)));   // the first chip after the SOP
        for (const auto& w : dsmDone) if (at > w.first - (int64_t)(64 * fs / 1e6) && at < w.second) return true;   // heard already, from another piece
        const double f = (centre > 0 ? centre : 0.0) + b.freqHz + best.offset * actual / (2 * M_PI);   // the discriminator's mean is the residual carrier
        int rf = -1;
        if (centre > 0) { const long n = std::lround((f - 2400e6) / 1e6); if (n >= 0 && n < 80 && std::fabs(f - 2400e6 - n * 1e6) < 300e3) rf = (int)n; }
        Decoded x = dsmParse(v + 1, 16, rf, bestRow);
        if (!x.ok) return false;
        dsmDoneFrom = at - (int64_t)(160 * fs / 1e6);
        dsmDoneTo = at + (int64_t)(19 * 64 * fs / 1e6);
        dsmDone.push_back({at, dsmDoneTo});
        if (dsmDone.size() > 16) dsmDone.erase(dsmDone.begin());
        char t[60];
        std::snprintf(t, sizeof t, ", pn row %d, SOP column %d", bestRow, bestCol);
        x.detail += t;
        addFrame(x, f, b.snrDb, v + 1, 16);
        return true;
    }

    void onBurst(const Burst& b) {
        { std::lock_guard<std::mutex> g(m); tel.bursts++; if (tel.state == 0) tel.state = 1; }
        if ((protocols & Dsm) && fs >= 4e6 && b.bwHz < 3.5e6) dsmPend.push_back(b);
        bool done = false;
        for (const FskDef& d : defs) {
            if (done) break;
            if (b.bwHz > 8 * d.rate + 200e3) continue;
            double actual = 0;
            const std::vector<float> disc = discriminate(finder, b, 1.2 * d.rate + 20e3, 8 * d.rate, &actual);
            if (actual < 2 * d.rate || disc.empty()) continue;
            const double sps = actual / d.rate;
            const SyncHit hit = findSync(disc, sps, d.pattern, d.maxErrors, d.kind == 3);
            if (!hit.found) continue;
            const std::vector<uint8_t> bytes = sliceBytes(disc, sps, hit, d.maxBytes, d.msbFirst);
            done = handle(d, bytes, b);
        }
    }

    bool handle(const FskDef& d, const std::vector<uint8_t>& bytes, const Burst& b) {
        const double f = (centre > 0 ? centre : 0.0) + b.freqHz;
        if (d.kind == 0) {
            SikPacket s = sikParse(bytes.data(), bytes.size());
            if (!s.ok) return false;
            std::vector<uint8_t>& tail = sikTail[s.netId];
            tail.insert(tail.end(), s.data.begin(), s.data.end());
            size_t used = 0;
            const std::vector<Decoded> frames = mavlinkScan(tail.data(), tail.size(), &used);
            bool any = false;
            for (Decoded x : frames) {
                x.id = "net " + std::to_string(s.netId) + ", " + x.id;
                addFrame(x, f, b.snrDb, tail.data(), used);
                any = true;
            }
            tail.erase(tail.begin(), tail.begin() + (std::ptrdiff_t)used);
            if (tail.size() > 300) tail.clear();
            if (!any) {
                Decoded x;
                x.ok = true; x.proto = Sik; x.id = "net " + std::to_string(s.netId); x.type = s.golay ? "SiK packet (ECC)" : "SiK packet";
                char t[120];
                std::snprintf(t, sizeof t, "%zu bytes, window %d, bonus %d, resend %d", s.data.size(), s.window, (int)s.bonus, (int)s.resend);
                x.detail = t;
                addFrame(x, f, b.snrDb, s.data.data(), s.data.size());
            }
            return true;
        }
        Decoded x;
        if (d.kind == 1) x = frskyD8Parse(bytes.data(), bytes.size());
        else if (d.kind == 2) x = frskyXParse(bytes.data(), bytes.size());
        else if (d.kind == 4) x = sfhssParse(bytes.data(), bytes.size());
        else x = flyskyParse(bytes.data(), bytes.size());
        if (!x.ok) return false;
        addFrame(x, f, b.snrDb, bytes.data(), std::min<size_t>(bytes.size(), d.kind == 1 ? 20 : bytes.size()));
        return true;
    }

    void drainLora(LoraChain& c, Bank& bank) {
        for (auto& o : bank.decs) {
            for (const lora::RxFrame& fr : o.frames) {
                const double fq = (centre > 0 ? centre : 0.0) + c.centreOffsetHz + fr.cfoHz;
                if (c.proto == Reticulum) {
                    if (!fr.crcOk) continue;
                    std::vector<uint8_t> pkt;
                    if (!rnodeUnframe(fr.payload.data(), fr.payload.size(), reas[(int)o.p.bwHz + o.p.sf], pkt)) continue;
                    const Decoded d = reticulumParse(pkt.data(), pkt.size());
                    if (d.ok) addFrame(d, fq, (float)fr.snrDb, pkt.data(), pkt.size());
                    else { std::lock_guard<std::mutex> g(m); tel.blocksBad++; }
                } else {
                    const Decoded d = elrsParse(fr.payload.data(), fr.payload.size(), elrs);
                    if (d.ok) addFrame(d, fq, (float)fr.snrDb, fr.payload.data(), fr.payload.size());
                    else { std::lock_guard<std::mutex> g(m); tel.blocksBad++; }
                }
            }
            o.frames.clear();
        }
    }

    void updatePreambles() {
        uint64_t n = 0;
        for (const LoraChain& c : chains) {
            for (const auto& o : c.normal->decs) n += o.dm->stats().preambles;
            if (c.inverted) for (const auto& o : c.inverted->decs) n += o.dm->stats().preambles;
        }
        std::lock_guard<std::mutex> g(m);
        tel.loraPreambles = n;
        if (tel.state == 0 && n > 0) tel.state = 1;
        tel.timeSec = fs > 0 ? (double)nIn / fs : 0;
        tel.inputRate = fs;
        tel.protocols = protocols;
        tel.phraseSet = !phrase.empty();
        seq++;
        tel.seq = seq;
    }
};

RcLinkReceiver::RcLinkReceiver() : p_(new Impl) {}
RcLinkReceiver::~RcLinkReceiver() = default;

void RcLinkReceiver::configure(double rate) {
    std::lock_guard<std::mutex> g(p_->m);
    p_->fs = rate;
    p_->dirty = true;
    p_->nIn = 0;
}
void RcLinkReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> g(p_->m); if (p_->offset != hz) { p_->offset = hz; p_->dirty = true; } }
void RcLinkReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> g(p_->m); if (p_->centre != hz) { p_->centre = hz; p_->dirty = true; } }
bool RcLinkReceiver::ready() const { return p_->fs >= 1e6; }
void RcLinkReceiver::reset() { std::lock_guard<std::mutex> g(p_->m); p_->dirty = true; p_->sikTail.clear(); p_->reas.clear(); }
void RcLinkReceiver::clearResults() {
    std::lock_guard<std::mutex> g(p_->m);
    p_->tel.frames.clear(); p_->tel.devices.clear();
    p_->tel.blocksOk = p_->tel.blocksBad = p_->tel.bursts = 0;
    p_->tel.state = 0;
    p_->seq++;
}
void RcLinkReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> g(p_->m); p_->log = std::move(cb); }
void RcLinkReceiver::setProtocols(int mask) { std::lock_guard<std::mutex> g(p_->m); if (p_->protocols != mask) { p_->protocols = mask; p_->dirty = true; } }
void RcLinkReceiver::setBindingPhrase(const std::string& phrase) { std::lock_guard<std::mutex> g(p_->m); p_->phrase = phrase; p_->dirty = true; }
void RcLinkReceiver::setElrsDomain(int index) { std::lock_guard<std::mutex> g(p_->m); if (p_->domain != index) { p_->domain = index; p_->dirty = true; } }
void RcLinkReceiver::setLoraPlan(double bwHz, int sf) { std::lock_guard<std::mutex> g(p_->m); p_->loraBw = bwHz; p_->loraSf = sf; p_->dirty = true; }
void RcLinkReceiver::setSikAirRate(int kbps) { std::lock_guard<std::mutex> g(p_->m); if (p_->sikKbps != kbps) { p_->sikKbps = kbps; p_->dirty = true; } }

void RcLinkReceiver::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    bool rebuild;
    { std::lock_guard<std::mutex> g(s.m); rebuild = s.dirty && s.fs > 0; }
    if (rebuild) { std::lock_guard<std::mutex> g(s.m); s.rebuild(); }
    if (s.fs <= 0) return;
    if (!s.defs.empty() || (s.protocols & Dsm)) s.finder.feed(x, n, [&](const Burst& b) { s.onBurst(b); });
    if (!s.dsmPend.empty()) s.dsmDrain(false);
    bool anyInv = false;
    for (LoraChain& c : s.chains) if (c.inverted) anyInv = true;
    if (anyInv) { s.conj.resize(n); for (size_t i = 0; i < n; i++) s.conj[i] = std::conj(x[i]); }
    for (LoraChain& c : s.chains) {
        c.normal->feed(x, n);
        s.drainLora(c, *c.normal);
        if (c.inverted) {
            c.inverted->feed(s.conj.data(), n);
            s.drainLora(c, *c.inverted);
        }
    }
    s.nIn += (int64_t)n;
    s.updatePreambles();
}

void RcLinkReceiver::flush() {
    Impl& s = *p_;
    if (!s.defs.empty() || (s.protocols & Dsm)) s.finder.flush([&](const Burst& b) { s.onBurst(b); });
    if (!s.dsmPend.empty()) s.dsmDrain(true);
    s.updatePreambles();
}

bool RcLinkReceiver::telemetry(RcTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> g(p_->m);
    if (p_->tel.seq == lastSeq) return false;
    out = p_->tel;
    return true;
}

} // namespace dect2


