// DVB-C2 test signal generator (EN 302 769 clauses 5 - 10). See dvbc2_gen.h.
#include "dect2/dvbc2/dvbc2_gen.h"
#include "dect2/t2/t2interleave.h"
#include "dect2/t2/t2fec.h"
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/tv/demo_ts.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/source/source.h"
#include "dect2/common/mathconst.h"
#include "dect2/dvbs/dvbs_ip.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace dect2 {
namespace dvbc2 {

namespace {
inline int64_t floorMod(int64_t a, int64_t b) { return ((a % b) + b) % b; }

// a PLP's transport stream -> BB frames -> XFECFrame cells (clause 5: normal mode, one packet per 1504-bit unit with the CRC-8 of the previous one)
struct PlpSource {
    PlpConfig cfg;
    Fec fec;
    Dims d;
    std::function<void(uint8_t*)> src;
    uint64_t packets = 0, blocks = 0;
    std::mt19937 rng{7};
    std::shared_ptr<dvbsip::GseEncapsulator> gse;   // shared, not unique: libstdc++ copies PlpSource when the vectors grow (SliceStream stays copyable)
    uint64_t ipPackets = 0;
    // GSE (payload type 2): the data field is GSE packets (TS 102 606), PDUs cut across BB frames where they do not fit
    void makeGseCells(std::vector<cf32>& cells) {
        if (!gse) gse = std::make_shared<dvbsip::GseEncapsulator>();
        const size_t bytes = (size_t)(d.kBch - 80) / 8;
        std::vector<uint8_t> pkt;
        while (gse->queued() < 3 && cfg.ip && cfg.ip(pkt)) {
            dvbsip::Label lab; lab.type = 2;
            const uint16_t proto = !pkt.empty() && (pkt[0] >> 4) == 6 ? 0x86DD : 0x0800;
            gse->push(proto, lab, pkt);
            ipPackets++;
        }
        const std::vector<uint8_t> field = gse->fill(bytes);
        std::vector<uint8_t> bits((size_t)d.kBch, 0);
        BbHeader h;
        h.tsGs = 2; h.sisMis = 1; h.ccmAcm = 1; h.upl = 0; h.dfl = (int)bytes * 8; h.sync = 0; h.syncd = 0;
        buildBbHeader(h, bits.data());
        for (size_t i = 0; i < field.size() && i < bytes; i++) for (int q = 0; q < 8; q++) bits[80 + i * 8 + (size_t)q] = (field[i] >> (7 - q)) & 1;
        encodeBlock(fec, bits, cells);
        blocks++;
    }
    void makeCells(std::vector<cf32>& cells) {
        if (cfg.payloadType == 2) { makeGseCells(cells); return; }
        const int npk = (d.kBch - 80) / 1504;
        std::vector<uint8_t> bits((size_t)d.kBch, 0);
        BbHeader h;
        h.tsGs = 3; h.sisMis = 1; h.ccmAcm = 1; h.upl = 1504; h.dfl = npk * 1504; h.sync = 0x47; h.syncd = 0;
        buildBbHeader(h, bits.data());
        uint8_t prevCrc = 0;
        for (int k = 0; k < npk; k++) {
            uint8_t pk[188];
            src(pk);
            packets++;
            const size_t o = 80 + (size_t)k * 1504;
            auto putByte = [&](size_t at, uint8_t v) { for (int q = 0; q < 8; q++) bits[at + (size_t)q] = (v >> (7 - q)) & 1; };
            putByte(o, prevCrc);
            for (int j = 0; j < 187; j++) putByte(o + 8 + (size_t)j * 8, pk[1 + j]);
            unsigned crc = 0;
            for (int j = 0; j < 187; j++) for (int q = 7; q >= 0; q--) { const unsigned fb = ((crc >> 7) & 1) ^ ((pk[1 + j] >> q) & 1); crc = (crc << 1) & 0xff; if (fb) crc ^= 0xD5; }
            prevCrc = (uint8_t)crc;
        }
        encodeBlock(fec, bits, cells);
        blocks++;
    }
};

// the cell stream of one data slice: data slice packets (clause 7) back to back
struct SliceStream {
    SliceConfig cfg;
    std::vector<PlpSource> plps;
    size_t nextPlp = 0;
    std::deque<cf32> pending;        // cells not yet placed
    size_t cellsOfCurrentBlock = 0;  // Type 1: cells of the XFECFrame in progress still pending (for PLP_START)
    void refill() {
        PlpSource& p = plps[nextPlp];
        nextPlp = (nextPlp + 1) % plps.size();
        std::vector<cf32> cells;
        if (cfg.type == 1) {
            FecFrameHeader h;
            h.plpId = p.cfg.id; h.fecType = p.fec.fecType; h.mod = p.fec.mod; h.cod = p.fec.cod; h.counter = 0;
            encodeHeader(h.pack(), cfg.fecHeaderType == 1, cells);
            pending.insert(pending.end(), cells.begin(), cells.end());
        }
        p.makeCells(cells);
        pending.insert(pending.end(), cells.begin(), cells.end());
    }
    cf32 next() {
        if (pending.empty()) refill();
        cf32 c = pending.front();
        pending.pop_front();
        return c;
    }
    // Type 1: the data cell index (from the start of the frame's stream) of the first complete XFECFrame
    int plpStart() {
        if (pending.empty()) return 0;
        return (int)pending.size();   // what is pending is the rest of the block in progress (Type 1 has no headers)
    }
};
} // namespace

struct Generator::Impl {
    GenConfig cfg;
    L1Part2 l1;
    int dxv = 12, gs = 64, ns = 4160;
    int64_t kBase = 0;                       // absolute carrier of bin 0
    int64_t kMin = 0, kMax = 0;
    std::vector<SliceStream> slices;
    std::vector<SliceCells> geom;
    std::vector<std::pair<int64_t, int64_t>> notches;
    // per-bin tables over the window
    std::vector<uint8_t> wk, pilotBit, cp, inSystem, inNotch;
    std::vector<int> qOf;                    // preamble data cell index q of the bin (-1 pilot)
    // frame state
    uint64_t frameNo = 0;
    int symInFrame = 0, preambleSyms = 1;
    L1Coded coded;
    std::vector<std::vector<std::vector<cf32>>> tiOut;   // [slice][row][cell] of the current TI block
    std::vector<int> tiRows;
    // time domain
    Fft fft{kFft};
    std::vector<cf32> sym, out;              // symbol with its guard interval; impaired output at the output rate
    size_t outPos = 0;
    float scale = 1;
    ExactResampler resampler;
    bool useResampler = false;
    std::vector<cf32> echoBuf;
    size_t echoLen = 0, echoPos = 0;
    cf32 echoGain{0, 0};
    double phase = 0, dph = 0;
    uint64_t symbolNo = 0;                   // l, counted on through the frames (the phase reference of clause 10.1)
    float noiseSigma = 0;
    genutil::NoiseSource noise;
    std::mt19937 rng;

    explicit Impl(const GenConfig& c) : cfg(c), noise(c.seed), rng(c.seed) {
        dxv = dx(cfg.gi); gs = guardSamples(cfg.gi); ns = symbolSamples(cfg.gi);
        kBase = cfg.tuneCarrier - kFft / 2;
        kMin = cfg.startFrequency; kMax = cfg.startFrequency + (int64_t)cfg.bandwidth * dxv;
        for (const NotchConfig& n : cfg.notches) if (n.width > 0) notches.push_back({kMin + (int64_t)n.start * dxv + 1, kMin + (int64_t)(n.start + n.width) * dxv - 1});
        // L1 (the per-frame fields are filled in buildL1)
        l1.networkId = cfg.networkId; l1.systemId = cfg.systemId; l1.startFrequency = cfg.startFrequency; l1.bandwidth = cfg.bandwidth;
        l1.guardInterval = cfg.gi; l1.frameLength = kDataSymbols;
        l1.reservedTone = cfg.reservedTones ? 1 : 0;
        for (const NotchConfig& n : cfg.notches) { L1Notch ln; ln.start = n.start; ln.width = n.width; l1.notches.push_back(ln); }
        for (const SliceConfig& s : cfg.slices) {
            SliceStream ss;
            ss.cfg = s;
            for (const PlpConfig& p : s.plps) {
                PlpSource ps;
                ps.cfg = p;
                ps.fec.fecType = p.fecType; ps.fec.mod = p.mod; ps.fec.cod = p.cod;
                ps.d = dims(ps.fec);
                ps.src = p.ts ? p.ts : dvbt::testTsSource(cfg.seed + (unsigned)p.id);
                ss.plps.push_back(std::move(ps));
            }
            slices.push_back(std::move(ss));
            SliceCells g;
            g.gi = cfg.gi; g.sysKMin = kMin; g.sysKMax = kMax; g.notches = notches; g.reservedTones = cfg.reservedTones;
            g.kMin = kMin + (int64_t)(s.tunePos + s.offsetLeft) * dxv;
            g.kMax = kMin + (int64_t)(s.tunePos + s.offsetRight) * dxv - 1;
            geom.push_back(g);
            L1DataSlice ls;
            ls.id = s.id; ls.tunePos = s.tunePos; ls.offsetLeft = s.offsetLeft; ls.offsetRight = s.offsetRight; ls.tiDepth = s.tiDepth; ls.type = s.type;
            ls.fecHeaderType = s.fecHeaderType; ls.constConf = s.type == 1 ? 1 : 0;
            for (auto& n : notches) if (n.second + 1 == g.kMin) ls.leftNotch = 1;
            for (const PlpConfig& p : s.plps) {
                L1Plp lp;
                lp.id = p.id; lp.type = p.type; lp.payloadType = p.payloadType; lp.groupId = p.groupId; lp.fecType = p.fecType; lp.mod = p.mod; lp.cod = p.cod;
                lp.tsId = p.tsId; lp.onId = p.onId;
                ls.plps.push_back(lp);
            }
            l1.slices.push_back(ls);
        }
        tiOut.resize(slices.size());
        tiRows.assign(slices.size(), 0);
        // the window tables
        wk.resize(kFft); pilotBit.resize(kFft); cp.resize(kFft); inSystem.resize(kFft); inNotch.resize(kFft); qOf.assign(kFft, -1);
        ReferenceBlock rb;
        for (int b = 0; b < kFft; b++) {
            const int64_t k = kBase + b;
            wk[(size_t)b] = dataScramble(k);
            inSystem[(size_t)b] = k >= kMin && k <= kMax;
            inNotch[(size_t)b] = 0;
            for (auto& n : notches) if (k >= n.first && k <= n.second) inNotch[(size_t)b] = 1;
            cp[(size_t)b] = isContinualPilot(k);
            if (floorMod(k, kPreamblePilotSpacing) == 0) { rb.set(k >= 0 ? k / kL1Carriers : -((-k + kL1Carriers - 1) / kL1Carriers)); pilotBit[(size_t)b] = rb.bit(k); }
            else { pilotBit[(size_t)b] = 0; qOf[(size_t)b] = preambleCellIndex((int)floorMod(k, kL1Carriers)); }
        }
        // level: the mean power of a data symbol over the window
        double p = 0;
        int cnt = 0;
        for (int b = 0; b < kFft; b++) if (inSystem[(size_t)b] && !inNotch[(size_t)b]) { const int64_t k = kBase + b; const bool pil = cp[(size_t)b] || k == kMin || k == kMax || isScatteredPilot(k, 0, cfg.gi); p += pil ? kPilotAmp * kPilotAmp : 1.0; cnt++; }
        if (cnt == 0) p = 1;
        scale = (float)(cfg.level / std::sqrt(p / kFft));
        // impairments
        if (cfg.echoDb > 0 && cfg.echoUs > 0) {
            echoLen = (size_t)std::max(1.0, std::round(cfg.echoUs * 1e-6 * kSampleRate));
            echoBuf.assign(echoLen, cf32(0, 0));
            echoGain = std::polar((float)std::pow(10.0, -cfg.echoDb / 20), (float)(cfg.echoDeg * math::kPi / 180));
        }
        // a transmitter clock fast by p makes every symbol shorter at the output: the resampler aims at (1 + p) times the output rate
        // (the exact resampler, not the 24-tap clock corrector: the signal reaches 0.42 of the native rate, where that one is no longer flat)
        useResampler = std::fabs(cfg.sampleRate - kSampleRate) > 1e-3 || cfg.clockPpm != 0;
        if (useResampler) resampler.configure(kSampleRate, cfg.sampleRate / (1 + cfg.clockPpm * 1e-6), 48);   // long: the band edge is at 0.42 of the native rate
        dph = 2 * math::kPi * cfg.cfoHz / cfg.sampleRate;
        if (cfg.snrDb < 199) {
            const double snr = std::pow(10.0, cfg.snrDb / 10);
            const double var = (double)scale * scale / snr * cfg.sampleRate / kSampleRate;   // per complex sample at the output rate
            noiseSigma = (float)std::sqrt(var / 2);
        }
        sym.resize((size_t)ns);
    }

    void buildL1() {
        for (size_t i = 0; i < slices.size(); i++) {
            L1DataSlice& ls = l1.slices[i];
            if (ls.type == 0) for (L1Plp& p : ls.plps) p.start = slices[i].plpStart();
        }
        l1.changeCounter = 0;
        encodeL1(l1, cfg.l1TiMode, coded);
        preambleSyms = std::max(1, coded.preambleSymbols);
    }

    // preamble symbol lp into X (window bins, bin 0 = the lowest frequency)
    void preambleSymbol(int lp, std::vector<cf32>& X) {
        std::vector<cf32> headerCells;
        encodeHeader(coded.header.pack(), false, headerCells);
        const std::vector<cf32>& row = coded.rows[(size_t)lp];
        std::vector<cf32> unit(headerCells);
        unit.insert(unit.end(), row.begin(), row.end());
        const auto& H = freqInterleaverSeqRef(2, kL1DataCells, (lp & 1) != 0);
        std::vector<cf32> a((size_t)kL1DataCells);
        for (int q = 0; q < kL1DataCells; q++) a[(size_t)q] = unit[(size_t)H[(size_t)q] % unit.size()];
        const float app = preamblePilotAmp(cfg.gi);
        std::fill(X.begin(), X.end(), cf32(0, 0));
        for (int b = 0; b < kFft; b++) {
            if (!inSystem[(size_t)b] || inNotch[(size_t)b]) continue;
            if (qOf[(size_t)b] < 0) X[(size_t)b] = cf32(app * (1.f - 2.f * pilotBit[(size_t)b]), 0.f);
            else { const cf32 v = a[(size_t)qOf[(size_t)b]]; X[(size_t)b] = wk[(size_t)b] ? -v : v; }
        }
    }

    // the time interleaver of one slice: fills tiOut[s] with the next tiDepth data symbols' cells (in carrier order, before the FI)
    void tiBlock(size_t s, int ld) {
        SliceStream& ss = slices[s];
        const SliceCells& g = geom[s];
        const int depth = ss.cfg.tiDepth == 0 ? 1 : 4 << (ss.cfg.tiDepth - 1);
        const int width = g.width();
        auto& rows = tiOut[s];
        rows.assign((size_t)depth, {});
        if (width <= 0) return;
        std::vector<std::vector<uint8_t>> isData((size_t)depth, std::vector<uint8_t>((size_t)width, 0));
        for (int r = 0; r < depth; r++) for (int c = 0; c < width; c++) isData[(size_t)r][(size_t)c] = g.isData(g.kMin + c, ld + r);
        std::vector<std::vector<cf32>> mem((size_t)depth, std::vector<cf32>((size_t)width, cf32(0, 0)));
        std::vector<std::pair<int, int>> order;
        tiWriteOrder(depth, width, order);
        for (auto& rc : order) if (isData[(size_t)rc.first][(size_t)rc.second]) mem[(size_t)rc.first][(size_t)rc.second] = ss.next();
        for (int r = 0; r < depth; r++) for (int c = 0; c < width; c++) if (isData[(size_t)r][(size_t)c]) rows[(size_t)r].push_back(mem[(size_t)r][(size_t)c]);
    }

    void dataSymbol(int ld, std::vector<cf32>& X) {
        std::fill(X.begin(), X.end(), cf32(0, 0));
        ReferenceBlock rb;
        // pilots of the whole system (also outside the slices: stuffing slices are not generated, those carriers stay empty)
        for (int b = 0; b < kFft; b++) {
            if (!inSystem[(size_t)b] || inNotch[(size_t)b]) continue;
            const int64_t k = kBase + b;
            bool pil = cp[(size_t)b] || k == kMin || k == kMax || isScatteredPilot(k, ld, cfg.gi);
            if (!pil) for (auto& n : notches) if (k == n.first - 1 || k == n.second + 1) pil = true;
            if (pil) X[(size_t)b] = cf32(kPilotAmp * (1.f - 2.f * pilotBit[(size_t)b]), 0.f);
            // reserved tones (clause 9.7): free values, here a random phase at 0.8 of the pilot amplitude (what a PAPR reduction might put there)
            else if (cfg.reservedTones && isReservedTone(k, ld, cfg.gi)) { const float a = (float)(rng() % 3600) * (float)math::kPi / 1800.f; X[(size_t)b] = std::polar(0.8f * kPilotAmp, a); }
        }
        for (size_t s = 0; s < slices.size(); s++) {
            SliceStream& ss = slices[s];
            const int depth = ss.cfg.tiDepth == 0 ? 1 : 4 << (ss.cfg.tiDepth - 1);
            if (ld % depth == 0) { tiBlock(s, ld); tiRows[s] = 0; }
            const std::vector<cf32>& cells = tiOut[s][(size_t)tiRows[s]++];
            std::vector<int64_t> carriers;
            geom[s].dataCarriers(ld, carriers);
            const int n = (int)carriers.size();
            if (n == 0 || (int)cells.size() != n) continue;
            const auto& H = freqInterleaverSeqRef(2, n, (ld & 1) != 0);
            for (int q = 0; q < n; q++) {
                const int64_t b = carriers[(size_t)q] - kBase;
                if (b >= 0 && b < kFft) X[(size_t)b] = cells[(size_t)H[(size_t)q]];
            }
        }
    }

    void nextSymbol() {
        if (symInFrame == 0) { buildL1(); frameNo++; }
        std::vector<cf32> X((size_t)kFft);
        if (symInFrame < preambleSyms) preambleSymbol(symInFrame, X);
        else dataSymbol(symInFrame - preambleSyms, X);
        symInFrame++;
        if (symInFrame >= preambleSyms + kDataSymbols) symInFrame = 0;
        // bin b (lowest frequency first) -> IFFT index (b - N/2) mod N
        std::vector<cf32> t((size_t)kFft);
        for (int b = 0; b < kFft; b++) t[(size_t)((b + kFft / 2) % kFft)] = X[(size_t)b];
        fft.inverse(t.data());
        const float s = scale / std::sqrt((float)kFft);
        for (int i = 0; i < kFft; i++) sym[(size_t)(gs + i)] = t[(size_t)i] * s;
        for (int i = 0; i < gs; i++) sym[(size_t)i] = sym[(size_t)(kFft + i)];
        // clause 10.1: carrier k sits at the absolute frequency k / T_U with its phase reference at the start of each symbol's useful part.
        // Seen from this baseband (centred on the tuning carrier) that is a common phase of -2 pi k_tune (G + l N_s) / N on symbol l.
        {
            const int64_t num = (int64_t)cfg.tuneCarrier * ((int64_t)gs + (int64_t)(symbolNo % (uint64_t)kFft) * ns) % kFft;
            const double ph = -2 * math::kPi * (double)num / kFft;
            const cf32 rot((float)std::cos(ph), (float)std::sin(ph));
            for (auto& v : sym) v *= rot;
            symbolNo++;
        }
        // echo
        if (echoLen) for (auto& v : sym) { const cf32 d = echoBuf[echoPos]; echoBuf[echoPos] = v; echoPos = (echoPos + 1) % echoLen; v += echoGain * d; }
        // to the output rate, then the carrier offset and the noise
        std::vector<cf32> o;
        if (useResampler) resampler.process(sym.data(), sym.size(), o); else o = sym;
        for (auto& v : o) {
            if (dph != 0) { v *= cf32((float)std::cos(phase), (float)std::sin(phase)); phase += dph; if (phase > math::kPi) phase -= 2 * math::kPi; else if (phase < -math::kPi) phase += 2 * math::kPi; }
        }
        if (noiseSigma > 0) noise.add(o.data(), o.size(), noiseSigma);
        out.insert(out.end(), o.begin(), o.end());
    }

    void generate(cf32* dst, size_t n) {
        size_t done = 0;
        while (done < n) {
            if (outPos >= out.size()) { out.clear(); outPos = 0; while (out.empty()) nextSymbol(); }
            const size_t take = std::min(n - done, out.size() - outPos);
            memcpy(dst + done, out.data() + outPos, take * sizeof(cf32));
            outPos += take; done += take;
        }
    }
};

Generator::Generator(const GenConfig& cfg) : p_(new Impl(cfg)) {}
Generator::~Generator() = default;
const GenConfig& Generator::config() const { return p_->cfg; }
void Generator::generate(cf32* out, size_t n) { p_->generate(out, n); }
const L1Part2& Generator::l1() const { return p_->l1; }
uint64_t Generator::frames() const { return p_->frameNo; }
uint64_t Generator::packets() const { uint64_t n = 0; for (auto& s : p_->slices) for (auto& p : s.plps) n += p.packets; return n; }
double Generator::netBitrate(size_t slice, size_t plp) const {
    if (slice >= p_->slices.size() || plp >= p_->slices[slice].plps.size()) return 0;
    const SliceCells& g = p_->geom[slice];
    std::vector<int64_t> c;
    double cells = 0;
    for (int ld = 0; ld < kDy; ld++) { g.dataCarriers(ld, c); cells += (double)c.size(); }
    cells /= kDy;
    const SliceStream& ss = p_->slices[slice];
    const double cellsPerSecond = cells / symbolSeconds(p_->cfg.gi) * (double)kDataSymbols / (kDataSymbols + p_->preambleSyms);
    const PlpSource& ps = ss.plps[plp];
    const double headerShare = ss.cfg.type == 1 ? (double)headerCells(ss.cfg.fecHeaderType == 1) / (ps.d.cells + headerCells(ss.cfg.fecHeaderType == 1)) : 0;
    return dvbc2::netBitrate(ps.fec, cellsPerSecond * (1 - headerShare)) / (double)ss.plps.size();
}

GenConfig defaultSystem(int gi, int64_t tuneCarrier) {
    GenConfig c;
    c.gi = gi;
    const int d = dx(gi);
    c.tuneCarrier = tuneCarrier - ((tuneCarrier % d) + d) % d;   // a multiple of D_X: K_min = tune - 1704 is then one too
    c.startFrequency = c.tuneCarrier - kL1Carriers / 2;
    c.bandwidth = kL1Carriers / d;                                // K_max = K_min + 3408: 3 409 carriers, the edge pilot included
    SliceConfig s;
    s.id = 1; s.tunePos = kL1Carriers / 2 / d; s.offsetLeft = -s.tunePos; s.offsetRight = s.tunePos;
    PlpConfig p;
    p.id = 1;
    s.plps.push_back(p);
    c.slices.push_back(s);
    return c;
}

} // namespace dvbc2

ModeTuning dvbc2Tuning() {
    ModeTuning t;
    t.stdMode = 743; t.id = "dvbc2"; t.name = "DVB-C2";
    t.minMhz = 47; t.maxMhz = 1002; t.defMhz = 346.0;
    t.sampleRate = 10e6; t.basebandHz = 8e6; t.bandwidthMhz = 8;
    t.minSampleRate = 8e6;
    return t;
}

namespace {
class Dvbc2Synth : public ModeSynth {
public:
    Dvbc2Synth(const dvbc2::GenConfig& c) : g_(c), rate_(c.sampleRate) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { g_.generate(out, n); }
private:
    dvbc2::Generator g_;
    double rate_;
};

dvbc2::GenConfig synthConfig(const SynthConfig& cfg, double sampleRate) {
    const int gi = cfg.modeOpt[1] == 1 ? 0 : 1;
    const int64_t tune = (int64_t)std::llround(dvbc2Tuning().defMhz * 1e6 / dvbc2::kCarrierSpacingHz);
    dvbc2::GenConfig c = dvbc2::defaultSystem(gi, tune);
    static const int mods[8] = {3, 1, 2, 3, 2, 3, 4, 5}, cods[8] = {4, 3, 1, 2, 5, 5, 2, 4};
    const int opt = std::clamp(cfg.modeOpt[0], 0, 7);
    dvbc2::SliceConfig& s = c.slices[0];
    s.type = cfg.modeOpt[2] >= 1 ? 1 : 0;
    s.fecHeaderType = cfg.modeOpt[2] == 2 ? 1 : 0;
    s.tiDepth = std::clamp(cfg.modeOpt[3], 0, 3);
    for (auto& p : s.plps) { p.mod = mods[opt]; p.cod = cods[opt]; p.fecType = cfg.modeOpt[5] == 1 ? 0 : 1; }
    if (s.type == 1 && cfg.modeOpt[6] == 1) { dvbc2::PlpConfig p2 = s.plps[0]; p2.id = 2; p2.tsId = 2; s.plps.push_back(p2); }
    if (cfg.modeOpt[4] == 1) { dvbc2::NotchConfig n; n.start = s.tunePos / 2; n.width = gi == 1 ? 3 : 1; c.notches.push_back(n); }   // 35 or 23 carriers
    c.sampleRate = sampleRate > 0 ? sampleRate : 10e6;
    c.snrDb = cfg.snrDb;
    c.cfoHz = cfg.cfoHz;
    c.clockPpm = cfg.modeVal[0] + cfg.sroPpm;
    if (cfg.echoDb > 0) { c.echoDb = cfg.echoDb; c.echoUs = cfg.echoDelay / c.sampleRate * 1e6; }
    return c;
}
} // namespace

double dvbc2SynthNetBitrate(const SynthConfig& cfg) {
    dvbc2::GenConfig c = synthConfig(cfg, 10e6);
    c.snrDb = 200;
    dvbc2::Generator g(c);
    double r = 0;
    for (size_t i = 0; i < c.slices[0].plps.size(); i++) r += g.netBitrate(0, i);
    return r;
}

std::unique_ptr<ModeSynth> makeDvbc2Synth(const SynthConfig& cfg, double sampleRate, std::function<void(uint8_t*)> ts) {
    dvbc2::GenConfig c = synthConfig(cfg, sampleRate);
    if (!ts) ts = demoTsSource(dvbc2SynthNetBitrate(cfg) / (double)c.slices[0].plps.size());
    for (auto& p : c.slices[0].plps) p.ts = ts;
    return std::make_unique<Dvbc2Synth>(c);
}
std::unique_ptr<ModeSynth> makeDvbc2Synth(const SynthConfig& cfg, double sampleRate) { return makeDvbc2Synth(cfg, sampleRate, {}); }

} // namespace dect2
