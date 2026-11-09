// Z-Wave monitor, see zwave_rx.h
#include "dect2/zwave/zwave_rx.h"
#include "dect2/zwave/zwave_proto.h"
#include "dect2/voicekeys/voice_keys.h"
#include "dect2/zwave/zwave_tel.h"
#include "dect2/map/mesh_topology_feed.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>

namespace dect2 {

using dect2::math::kPi;

namespace {

constexpr double kChanRate = 400e3;     // the rate of every channel stream, about
constexpr double kMaxBurstSec = 0.25;
constexpr int kTab = 256, kChunk = 4096;

struct Decoded { zwave::Frame f; int rate = 0; bool ok = false; bool sawSof = false; };

// SOF search in one bit stream; the first frame whose length is plausible and whose check passes
void searchFrames(const std::vector<uint8_t>& bits, int rate, Decoded& out) {
    const uint8_t sof = zwave::sofByte(rate);
    const int chk = zwave::checkOctets(rate);
    for (size_t p = 12; p + 8 + 8 * 9 <= bits.size(); p++) {
        int v = 0;
        for (int i = 0; i < 8; i++) v = (v << 1) | bits[p + (size_t)i];
        if (v != sof) continue;
        int flaws = 0;
        for (size_t i = p - 11; i < p; i++) if (bits[i] == bits[i - 1]) flaws++;
        if (flaws > 1) continue;
        const size_t q = p + 8;
        auto byteAt = [&](size_t k) { int b = 0; for (int i = 0; i < 8; i++) b = (b << 1) | bits[q + k * 8 + (size_t)i]; return b; };
        const int len = byteAt(7);
        if (len < 9 + chk || len > zwave::maxLength(rate) || q + (size_t)len * 8 > bits.size()) continue;
        out.sawSof = true;
        std::vector<uint8_t> m((size_t)len);
        for (int k = 0; k < len; k++) m[(size_t)k] = (uint8_t)byteAt((size_t)k);
        zwave::Frame f;
        if (zwave::parse(m.data(), m.size(), rate, f) && f.checkOk) { out.f = f; out.rate = rate; out.ok = true; return; }
    }
}

// FM demodulated burst -> frame at one rate
Decoded decodeRate(const std::vector<float>& d, size_t a, int rate, double chanRate) {
    Decoded res;
    const double spc = chanRate / zwave::chipRate(rate);
    const size_t w = (size_t)std::max(1.0, std::round(spc));
    if (d.size() < a + 40 * (size_t)spc + w) return res;
    std::vector<double> ps(d.size() + 1, 0);
    for (size_t i = 0; i < d.size(); i++) ps[i + 1] = ps[i] + d[i];
    auto m = [&](double pos) { const long i = std::lround(pos); if (i < (long)w || i >= (long)d.size()) return 0.0; return (ps[(size_t)i + 1] - ps[(size_t)i + 1 - w]) / (double)w; };
    double best = -1, bphi = 0;
    for (double phi = 0; phi < spc; phi += 0.5) {
        double s = 0;
        for (int k = 1; k <= 16; k++) s += std::fabs(m((double)a + phi + k * spc));
        if (s > best) { best = s; bphi = phi; }
    }
    std::vector<uint8_t> chips;
    for (size_t k = 0;; k++) {
        const double pos = (double)a + bphi + (double)k * spc;
        if (pos >= (double)d.size() - 1) break;
        chips.push_back(m(pos) > 0 ? 1 : 0);
    }
    for (int inv = 0; inv < 2 && !res.ok; inv++) {
        if (rate == 1) {
            for (int al = 0; al < 2 && !res.ok; al++) {
                std::vector<uint8_t> bits;
                for (size_t j = (size_t)al; j + 1 < chips.size(); j += 2) bits.push_back((uint8_t)((chips[j] ^ inv) > (chips[j + 1] ^ inv) ? 1 : 0));
                searchFrames(bits, rate, res);
            }
        } else {
            std::vector<uint8_t> bits(chips.size());
            for (size_t j = 0; j < chips.size(); j++) bits[j] = (uint8_t)(chips[j] ^ inv);
            searchFrames(bits, rate, res);
        }
    }
    return res;
}

struct Group {
    double mhz = 0;
    int mask = 0;                     // bit r: rate r decoded here
    std::vector<size_t> chanIdx[4];   // telemetry channels per rate
    cf32 step{1, 0};
    std::complex<double> rotD{1, 0}, stepB{1, 0};   // the mixer: phasor at the start of the next sample, and one table step
    double w = 0;
    std::vector<cf32> tab;                           // exp(j w k), k < kTab
    std::vector<float> mre, mim;                     // N - 1 mixed samples of history, then the block
    int D = 1;
    double chanRate = kChanRate;
    std::vector<float> taps;
    int dcount = 0;
    // burst detector
    float pe = 0, nz = 0;
    int nzInit = 0, above = 0, below = 0;
    bool inBurst = false;
    std::vector<cf32> hist;           // last pre-roll samples
    size_t hpos = 0, hfill = 0;
    std::vector<cf32> cur;
    size_t s0 = 0;
    float nzAtStart = 0;
};

} // namespace

struct ZwaveReceiver::Impl {
    std::mutex mu;
    std::atomic<double> rate{0}, centerHz{0}, dialHz{0};
    std::string regionSet;
    std::atomic<bool> resetReq{true};
    std::function<void(const std::string&)> log;
    std::vector<Group> groups;
    ZwaveTelemetry cfg;                          // region, channels and coverage of the current build
    uint64_t nSamp = 0;
    uint64_t seq = 0;
    double nextReport = 0.25;
    int state = 0;
    double lastGood = -1e9, lastBurst = -1e9;
    float snrEma = 0, levelEma = -200;
    bool snrInit = false;
    double cfoEma = 0;
    // tables (under mu)
    std::deque<ZwFrame> logq;
    MeshTopology topo;                           // routes and links of every home ID heard (mesh_topology.h)
    TopoSnapshot topoCache;
    double topoAt = -1e9;
    std::map<uint32_t, ZwNetwork> nets;
    std::map<uint64_t, ZwNode> nodes;
    std::map<std::string, uint64_t> perCc;
    zwave::S0Session s0;
    uint64_t ok = 0, bad = 0, bursts = 0, perRate[3] = {}, encS0 = 0, encS2 = 0, acks = 0;
    std::vector<ZwChannel> chans;

    double now() const { return rate > 0 ? (double)nSamp / rate : 0; }

    void rebuild() {
        const double fs = rate;
        groups.clear();
        chans.clear();
        const double centre = centerHz > 0 ? centerHz.load() : dialHz.load();
        const auto& regs = zwave::regions();
        const zwave::Region* reg = zwave::region(regionSet);
        auto covered = [&](double mhz) { return fs > 0 && std::fabs(mhz * 1e6 - centre) < 0.45 * fs; };
        if (!reg) {
            int best = -1;
            for (const auto& r : regs) {
                int n = 0;
                for (const auto& c : r.chans) n += covered(c.mhz);
                if (n > best) { best = n; reg = &r; }
            }
        }
        cfg = ZwaveTelemetry();
        cfg.region = reg->id; cfg.regionName = reg->name; cfg.inputRate = fs;
        cfg.loMhz = (centre - 0.45 * fs) / 1e6; cfg.hiMhz = (centre + 0.45 * fs) / 1e6;
        if (centre <= 0) cfg.loMhz = cfg.hiMhz = 0;
        for (const auto& c : reg->chans) {
            ZwChannel zc; zc.mhz = c.mhz; zc.rate = c.rate; zc.covered = centre > 0 && covered(c.mhz);
            chans.push_back(zc);
            if (!zc.covered) continue;
            cfg.channelsCovered++;
            Group* g = nullptr;
            for (auto& x : groups) if (std::fabs(x.mhz - c.mhz) < 0.06) g = &x;
            if (!g) { groups.emplace_back(); g = &groups.back(); g->mhz = c.mhz; }
            g->mask |= 1 << c.rate;
            g->chanIdx[c.rate].push_back(chans.size() - 1);
        }
        for (auto& g : groups) {   // front end
            g.D = std::max(1, (int)std::lround(fs / kChanRate));
            g.chanRate = fs / g.D;
            const double w = -2 * kPi * (g.mhz * 1e6 - centre) / fs;
            g.step = cf32((float)std::cos(w), (float)std::sin(w));
            const int N = 8 * g.D + 1;
            g.taps.assign((size_t)N, 0);
            const double fc = 110e3 / fs;
            double sum = 0;
            for (int i = 0; i < N; i++) {
                const double x = i - (N - 1) / 2.0;
                const double s = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
                const double win = 0.54 - 0.46 * std::cos(2 * kPi * i / (N - 1));
                g.taps[(size_t)i] = (float)(s * win);
                sum += s * win;
            }
            for (auto& t : g.taps) t = (float)(t / sum);
            g.tab.resize(kTab);
            for (int k = 0; k < kTab; k++) g.tab[(size_t)k] = cf32((float)std::cos(w * k), (float)std::sin(w * k));
            g.w = w;
            g.stepB = std::polar(1.0, w * kTab);
            g.rotD = std::complex<double>(1, 0);
            g.mre.assign((size_t)N - 1 + kChunk, 0.f); g.mim.assign((size_t)N - 1 + kChunk, 0.f);
            g.hist.assign(512, cf32(0, 0));
        }
        resetReq = false;
    }

    void burstDone(Group& g) {
        const double fs = g.chanRate;
        const size_t L = g.cur.size();
        if (L < 400) return;
        bursts++;
        lastBurst = now();
        std::vector<float> d(L, 0);
        for (size_t i = 1; i < L; i++) d[i] = (float)(std::arg(g.cur[i] * std::conj(g.cur[i - 1])) * fs / (2 * kPi));
        const size_t a = g.s0 + (size_t)(0.0004 * fs);
        const size_t e = std::min(L, g.s0 + (size_t)(0.0024 * fs));
        double cfo = 0;
        if (e > a + 20) { for (size_t i = a; i < e; i++) cfo += d[i]; cfo /= (double)(e - a); }
        for (auto& v : d) v = (float)(v - cfo);
        // level of the burst
        double pw = 0; size_t np = 0;
        const size_t pb = std::min(L, g.s0 + (size_t)(0.0004 * fs)), pend = L > 200 ? L - 160 : L;
        for (size_t i = pb; i < pend; i++) { pw += std::norm(g.cur[i]); np++; }
        pw = np ? pw / (double)np : 0;
        const float levelDb = (float)(10 * std::log10(pw + 1e-20));
        const float snr = (float)(10 * std::log10(std::max(1e-3, pw / std::max(1e-20f, g.nzAtStart) - 1)));
        Decoded best; bool saw = false; int sawRate = 0;
        for (int r = 1; r <= 3 && !best.ok; r++) {
            if (!(g.mask & (1 << r))) continue;
            Decoded dec = decodeRate(d, a, r, fs);
            if (dec.ok) best = dec;
            else if (dec.sawSof) { saw = true; sawRate = r; }
        }
        std::lock_guard<std::mutex> lk(mu);
        if (!best.ok) {
            if (saw) { bad++; for (size_t ci : g.chanIdx[sawRate]) chans[ci].bad++; }
            return;
        }
        addFrame(g, best, levelDb, snr, cfo);
    }

    void addFrame(Group& g, const Decoded& dec, float levelDb, float snr, double cfo) {
        const zwave::Frame& f = dec.f;
        ok++; perRate[dec.rate - 1]++;
        lastGood = now();
        cfoEma = cfo;
        levelEma = levelDb;
        if (!snrInit) { snrEma = snr; snrInit = true; } else snrEma = 0.8f * snrEma + 0.2f * snr;
        for (size_t ci : g.chanIdx[dec.rate]) chans[ci].frames++;
        s0.observe(f);
        std::string s0Text;
        int enc = zwave::encryption(f.payload);
        if (enc == 1) {
            voicekeys::CallKeyInfo ki; ki.protocol = voicekeys::Protocol::ZWave;
            for (const auto& k : voicekeys::KeyStore::instance().candidates(ki)) {
                std::vector<uint8_t> pl;
                if (k.key.size() == 16 && s0.decrypt(f, k.key.data(), pl)) { zwave::Frame in = f; in.payload = pl; s0Text = "[S0 decrypted] " + zwave::describe(in); break; }
            }
        }
        if (enc == 1) encS0++; else if (enc == 2) encS2++;
        if (f.headerType == zwave::kHdrAck) acks++;
        ZwFrame z;
        z.wallTime = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        z.sec = now(); z.mhz = g.mhz; z.rate = dec.rate;
        z.home = zwave::homeText(f.homeId); z.src = zwave::nodeText(f.src);
        if (f.headerType == zwave::kHdrMulticast) {
            for (size_t i = 0; i < f.group.size(); i++) z.dst += (i ? "," : "") + std::to_string(f.group[i]);
        } else z.dst = zwave::nodeText(f.dst);
        z.type = f.routed ? "routed" : zwave::headerName(f.headerType);
        z.text = s0Text.empty() ? zwave::describe(f) : s0Text;
        z.seq = f.seq; z.ackReq = f.ackReq; z.encryption = enc; z.rssi = levelDb; z.snrDb = snr; z.len = f.len;
        std::vector<uint8_t> raw = zwave::build(f, dec.rate);
        z.hex = zwave::hex(raw.data(), raw.size());
        logq.push_front(z);
        if (logq.size() > kZwLogMax) logq.pop_back();
        if (!f.payload.empty() && f.headerType != zwave::kHdrAck && !f.routed) {
            const char* n = zwave::ccName(f.payload[0]);
            if (*n) perCc[n]++;
            else { char b[16]; snprintf(b, sizeof b, "0x%02X", f.payload[0]); perCc[b]++; }
        }
        // tables
        ZwNetwork& nt = nets[f.homeId];
        if (nt.frames == 0) { nt.homeId = f.homeId; nt.home = z.home; nt.firstSec = z.sec; }
        nt.frames++; if (enc) nt.encrypted++;
        nt.rssi = levelDb; nt.lastSec = z.sec; nt.lastWall = z.wallTime;
        auto node = [&](int id) -> ZwNode& {
            ZwNode& n = nodes[((uint64_t)f.homeId << 8) | (uint64_t)id];
            if (n.id == 0 && n.sent == 0 && n.received == 0) { n.homeId = f.homeId; n.home = z.home; n.id = id; n.firstSec = z.sec; n.role = id == 1 ? "controller" : "node"; }
            return n;
        };
        topoFeedZwave(topo, f, snr, z.sec);
        ZwNode& s = node(f.src);
        s.sent++; if (enc) s.encrypted++;
        s.rate = dec.rate; s.rssi = levelDb; s.lastSec = z.sec; s.lastWall = z.wallTime;
        s.lowPower = f.lowPower; s.beaming = f.beam != 0;
        if (!f.payload.empty() && f.headerType != zwave::kHdrAck && !f.routed) {
            const char* n = zwave::ccName(f.payload[0]);
            s.lastCc = *n ? n : "?";
        }
        if (f.headerType == zwave::kHdrMulticast) { for (int dn : f.group) if (dn >= 1 && dn <= 232) node(dn).received++; }
        else if (f.dst >= 1 && f.dst <= 232) node(f.dst).received++;
        if (f.src == 1) nt.controller = 1;
        int cnt = 0;
        for (auto& kv : nodes) if (kv.second.homeId == f.homeId) cnt++;
        nt.nodes = cnt;
        if (nets.size() > kZwNetMax) nets.erase(nets.begin());
        if (nodes.size() > kZwNodeMax) nodes.erase(nodes.begin());
    }

    void feedGroup(Group& g, const cf32* x, size_t n) {
        const size_t N = g.taps.size();
        while (n > 0) {
            // the block mixed to the channel (a table of one 256 sample run times the phasor of the run: no per-sample recurrence), after the N - 1 samples of history
            const size_t m = std::min(n, (size_t)kChunk);
            float* mr = g.mre.data() + (N - 1);
            float* mi = g.mim.data() + (N - 1);
            for (size_t i0 = 0; i0 < m; i0 += kTab) {
                const float br = (float)g.rotD.real(), bi = (float)g.rotD.imag();
                const size_t e = std::min(m, i0 + (size_t)kTab);
                const cf32* tb = g.tab.data();
                for (size_t i = i0; i < e; i++) {
                    const float tr = tb[i - i0].real(), ti = tb[i - i0].imag();
                    const float pr = br * tr - bi * ti, pi = br * ti + bi * tr;
                    const float xr = x[i].real(), xi = x[i].imag();
                    mr[i] = xr * pr - xi * pi; mi[i] = xr * pi + xi * pr;
                }
                g.rotD *= (e - i0 == (size_t)kTab) ? g.stepB : std::polar(1.0, g.w * (double)(e - i0));
            }
            g.rotD /= std::abs(g.rotD);
            for (size_t i = 0; i < m; i++) {
                if (++g.dcount < g.D) continue;
                g.dcount = 0;
                float o[2];
                genutil::dot2(g.taps.data(), g.mre.data() + i, g.mim.data() + i, (int)N, o);
                const float re = o[0], im = o[1];
            const cf32 y(re, im);
            const float p = re * re + im * im;
            g.pe += 0.2f * (p - g.pe);
            if (g.nzInit < 2000) { g.nz += (g.pe - g.nz) / (float)(++g.nzInit); }
            if (!g.inBurst) {
                if (g.pe < 3 * g.nz) g.nz += 0.002f * (g.pe - g.nz);
                g.hist[g.hpos] = y; g.hpos = (g.hpos + 1) % g.hist.size(); g.hfill = std::min(g.hfill + 1, g.hist.size());
                if (g.nzInit >= 2000 && g.pe > 6 * g.nz) g.above++; else g.above = 0;
                if (g.above >= 6) {
                    g.inBurst = true; g.below = 0; g.nzAtStart = g.nz;
                    const size_t pre = std::min<size_t>(g.hfill, 206);
                    g.cur.clear();
                    for (size_t j = 0; j < pre; j++) g.cur.push_back(g.hist[(g.hpos + g.hist.size() - pre + j) % g.hist.size()]);
                    g.s0 = pre >= 12 ? pre - 12 : 0;
                }
            } else {
                g.cur.push_back(y);
                if (g.pe < 2.5f * g.nzAtStart) g.below++; else g.below = 0;
                if (g.below >= 160 || g.cur.size() > (size_t)(kMaxBurstSec * g.chanRate)) {
                    g.inBurst = false; g.above = 0; g.hfill = 0;
                    burstDone(g);
                    g.cur.clear();
                }
            }
            }
            std::copy(g.mre.begin() + (long)m, g.mre.begin() + (long)(m + N - 1), g.mre.begin());
            std::copy(g.mim.begin() + (long)m, g.mim.begin() + (long)(m + N - 1), g.mim.begin());
            x += m; n -= m;
        }
    }
};

ZwaveReceiver::ZwaveReceiver() : p_(std::make_unique<Impl>()) {}
ZwaveReceiver::~ZwaveReceiver() = default;

void ZwaveReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void ZwaveReceiver::setSignalOffset(double) {}
void ZwaveReceiver::setCenterHz(double hz) { if (hz != p_->centerHz.exchange(hz)) p_->resetReq = true; }
void ZwaveReceiver::setDialHz(double hz) { if (hz != p_->dialHz.exchange(hz)) p_->resetReq = true; }
void ZwaveReceiver::setRegion(const std::string& id) { std::lock_guard<std::mutex> lk(p_->mu); if (id != p_->regionSet) { p_->regionSet = id; p_->resetReq = true; } }
bool ZwaveReceiver::ready() const { return p_->rate >= zwaveTuning().minSampleRate - 1; }
void ZwaveReceiver::reset() { p_->resetReq = true; }
void ZwaveReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

void ZwaveReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->logq.clear(); p_->nets.clear(); p_->nodes.clear(); p_->perCc.clear();
    p_->topo.clear(); p_->topoCache = TopoSnapshot(); p_->topoAt = -1e9;
    p_->ok = p_->bad = p_->bursts = p_->encS0 = p_->encS2 = p_->acks = 0;
    for (auto& v : p_->perRate) v = 0;
    for (auto& c : p_->chans) c.frames = c.bad = 0;
    p_->seq++;
}

void ZwaveReceiver::feed(const cf32* x, size_t n) {
    Impl* p = p_.get();
    if (!ready()) return;
    if (p->resetReq) { std::lock_guard<std::mutex> lk(p->mu); p->rebuild(); }
    for (auto& g : p->groups) p->feedGroup(g, x, n);
    p->nSamp += n;
    while (p->now() >= p->nextReport) {
        std::lock_guard<std::mutex> lk(p->mu);
        const int old = p->state;
        p->state = p->nextReport - p->lastGood < 5.0 ? 2 : (p->nextReport - p->lastBurst < 10.0 ? 1 : 0);
        p->seq++;
        p->nextReport += 0.25;
        std::function<void(const std::string&)> f = p->log;
        if (f && p->state == 2 && old != 2) f("Z-Wave frames are coming in");
        else if (f && p->state != 2 && old == 2) f("no Z-Wave frames for 5 s");
    }
}

bool ZwaveReceiver::telemetry(ZwaveTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl* p = p_.get();
    if (p->seq <= lastSeq) return false;
    out = p->cfg;
    out.seq = p->seq; out.state = p->state; out.timeSec = p->now(); out.inputRate = p->rate;
    out.cfoHz = p->cfoEma; out.snrDb = p->snrInit ? p->snrEma : 0; out.levelDb = p->levelEma;
    out.dataValid = p->state == 2; out.blocksOk = p->ok; out.blocksBad = p->bad; out.bursts = p->bursts;
    for (int i = 0; i < 3; i++) out.perRate[i] = p->perRate[i];
    out.encS0 = p->encS0; out.encS2 = p->encS2; out.acks = p->acks;
    out.chans = p->chans;
    out.perCc.assign(p->perCc.begin(), p->perCc.end());
    std::sort(out.perCc.begin(), out.perCc.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& kv : p->nets) out.networks.push_back(kv.second);
    std::sort(out.networks.begin(), out.networks.end(), [](const ZwNetwork& a, const ZwNetwork& b) { return a.lastSec > b.lastSec; });
    for (const auto& kv : p->nodes) out.nodes.push_back(kv.second);
    std::sort(out.nodes.begin(), out.nodes.end(), [](const ZwNode& a, const ZwNode& b) { return a.lastSec > b.lastSec; });
    out.log.assign(p->logq.begin(), p->logq.end());
    if (p->now() - p->topoAt >= 1.0 || p->topoAt > p->now()) { p->topoCache = p->topo.snapshot(); p->topoAt = p->now(); }
    out.topo = p->topoCache;
    return true;
}

ModeTuning zwaveTuning() {
    ModeTuning t;
    t.stdMode = 100; t.id = "zwave"; t.name = "Z-Wave";
    t.minMhz = 860; t.maxMhz = 930; t.defMhz = 869.13;   // the middle of the European channels: 868.42 / 868.40 and 869.85 both inside 2 Msps
    t.sampleRate = 2e6;
    t.basebandHz = 0;
    t.bandwidthMhz = 1.8;
    t.minSampleRate = 1e6;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
