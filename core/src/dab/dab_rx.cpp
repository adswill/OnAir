// DAB receiver, transmission mode I: null-symbol and phase-reference synchronisation, carrier and timing tracking, DQPSK
// demodulation, fast information channel (ensemble, services, labels) and the main service channel of the selected sub-channel.
#include "dect2/common/text_charset.h"
#include "dect2/dab/dab.h"
#include "dect2/dab/dab_journaline.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dab/dab_tii.h"
#include "dect2/dab/dab_fig.h"
#include "dect2/dab/dab_proto.h"
#include "dect2/dsp/resampler.h"
#include "dect2/channels/channel_find.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_map>
#include "dect2/common/bits.h"
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using namespace dab;
using cd = std::complex<double>;
using dect2::math::kPi;
constexpr int kMaxShift = 80;      // integer carrier offset searched at the start: +-80 kHz covers 50 ppm at L band (1.49 GHz, 75 kHz)
constexpr int kBackoff = 16;        // demodulation windows start this many samples before the useful part (inside the guard interval)
constexpr int kCoast = 12;          // frames decoded on the timing and carrier offset held through a fade before the sync counts as lost (1.15 s)
constexpr int kQuickShift = 2;      // a re-acquisition searches this many carriers around the offset known

// A 16-byte label in the charset FIG 1 names (EN 300 401 5.2.2.2.1; EBU Latin unless it says otherwise) to clean UTF-8
std::string labelText(int charset, const uint8_t* d, int n) {
    return text::clean(text::dabCharset(charset, d, (size_t)n));
}

// max*(a, b) = log(e^a + e^b): the larger plus a correction from a table (1/8 steps up to 8, then nothing). DECT2_DAB_MAXLOG=1: the larger only
float maxStar(float a, float b) {
    static const struct T { float v[65]; T() { for (int i = 0; i <= 64; i++) v[i] = (float)std::log1p(std::exp(-(i + 0.5) / 8.0)); v[64] = 0; } } t;
    static const bool maxLog = std::getenv("DECT2_DAB_MAXLOG") != nullptr;
    const float m = a > b ? a : b, d = std::fabs(a - b);
    if (maxLog || !(d < 8.f)) return m;
    return m + t.v[(int)(d * 8.f)];
}

bool baselineMode() { static const bool b = std::getenv("DECT2_DAB_BASELINE") != nullptr; return b; }

uint32_t getBits(const uint8_t* d, int pos, int n) { return (uint32_t)bits::getPackedMsb(d, (size_t)pos, n); }
} // namespace

struct DabReceiver::Impl {
    // ---- input
    double inRate = kRate;
    RationalResampler rs;
    bool resample = false;
    ChannelCentre cc;           // moves an ensemble that is far from the middle of the sample band there
    std::vector<cf32> cin;
    std::vector<cf32> buf, rsOut;
    uint64_t base = 0;        // absolute index of buf[0]
    Fft fft{kTu};
    std::vector<int> bin;     // FFT bin of the n-th carrier
    std::vector<cf32> prsConj;

    // ---- synchronisation
    int state = 0;            // 0 search, 2 locked
    bool mirror = false;      // the input is conjugated (a mirrored spectrum)
    uint64_t nextU = 0;       // predicted useful start of the next phase reference symbol (absolute)
    double cfo = 0;           // carrier offset in bins (1 kHz each)
    bool fresh = false;       // just found: no refinement needed for the first frame
    int missed = 0;
    uint64_t framesDone = 0;
    uint64_t syncSample = 0, endSample = 0;
    float cirPeak = 0;
    std::vector<float> cirKeep;
    // ---- fades, lost samples and re-acquisition (docs/DAB-AUDIT.md)
    double magRef = 0;        // slow average of the differential cell magnitude over good frames: the scale of the soft bits, so a faded frame gives weak ones
    double powRef = 0;        // mean sample power of the good frames: a frame far weaker than that is a fade, not a lost timing
    bool quick = false;       // the next search re-acquires: only around the carrier offset known, the decoding state is kept
    int quickTries = 0;
    uint64_t lastU = 0;       // useful start of the last frame whose MSC was decoded (absolute)
    bool haveLast = false;
    int lastCif = -1;         // CIF count of the first CIF of that frame
    int curSlot = 0;          // the FIB being parsed (its FIG 0/0 tells the CIF count of its CIF)
    int frameCif = -1;        // CIF count of the first CIF of the frame being decoded, -1 when its FIC did not say
    int gapCifs = 0;          // CIFs lost before the frame being decoded
    bool dupFrame = false;    // the frame being decoded was decoded before (a re-acquisition on the same frame)
    uint64_t syncLosses = 0, relocks = 0, erasedCifs = 0, impulses = 0;
    double impPow = 0;        // the impulse blanker's running mean power
    double sro = 0;           // sample clock error (relative): symbol l of a frame starts at U + l * kTs * (1 + sro), the next frame kFrame * (1 + sro) later
    // the start of symbol l of the frame at U, with the clock error: the window goes on the nearest sample, `rest` is what is left over (turned
    // out per carrier after the FFT)
    uint64_t symStart(uint64_t U, int l, double* rest = nullptr) const {
        const double x = (double)l * kTs * (1.0 + sro);
        const double r = std::round(x);
        if (rest) *rest = x - r;
        return U + (uint64_t)(int64_t)r;
    }
    dabtii::Detector tii;     // transmitter identification from the null symbols

    // ---- decoding state
    mutable std::mutex mu;
    DabEnsemble ens;
    DabTelemetry tel;
    uint64_t telSeq = 0;
    uint64_t fibOk = 0, fibBad = 0;
    int ficRecent = 0;
    // ---- FIC analysis: every distinct FIG as received, counted (the FIG 0/0 and 0/10 change all the time: only the last one is kept)
    std::unordered_map<std::string, DabFigRecord> figs;
    std::map<std::pair<int, int>, DabFicStat> figStat;
    uint64_t ficFill = 0, ficFibs = 0, ficBadN = 0, ficFrames = 0;   // the analysis's own counts, from the last reset (read under mu)
    int lastFill[12] = {};
    uint64_t ficSeq = 0;
    // programme types and languages (FIG 0/17), announcement support (0/18) and switching (0/19)
    struct PtyInfo { int pty = -1; bool dyn = false; int lang = -1; };
    std::map<uint32_t, PtyInfo> ptyInfo;
    std::map<uint32_t, DabAnnSupport> annSup;
    // extended labels (FIG 2, UTF-8 or UCS-2 in segments): the segments of each label heard, and the labels complete (those replace the FIG 1 ones)
    struct LabelSegs { int toggle = -1, total = 0; bool ucs2 = false; std::map<int, std::vector<uint8_t>> segs; };
    std::map<std::string, LabelSegs> fig2Segs;
    std::set<std::string> fig2Done;
    std::map<int, std::pair<DabAnnSwitch, uint64_t>> annSw;   // cluster -> the last switching FIG and the FIC frame it came in
    double snrDb = 0;
    std::vector<cf32> constel;

    std::vector<int8_t> ficSoft, mscSoft;
    // The time de-interleaver and the decoding of one sub-channel: 16 CIFs of soft bits. CIFs lost on the way (lost samples, frames that were not
    // demodulated) go in as erasures (soft value 0), so the logical frames around them still decode: every one is spread over 16 CIFs and the code
    // copes with a quarter of them missing. Nothing is flushed on a short loss.
    struct SubDec {
        std::vector<std::vector<int8_t>> hist = std::vector<std::vector<int8_t>>(16);
        uint64_t count = 0;
        int early = 16;           // CIFs in before the first logical frame is decoded (10 for DAB+ audio: Reed-Solomon and the CRCs catch a failed one)
        std::vector<int8_t> mother, one;
        std::vector<uint8_t> dec, bytes;
        void reset() { hist.assign(16, {}); count = 0; }
        // one CIF of the sub-channel (src nullptr: erased); out(bytes, n, erased) gets every logical frame decoded, erased when most of its soft bits were
        template <class F> bool cif(const DabSubchannel& sub, const int8_t* src, F&& out) {
            int n, br, info, sz, lv;
            if (sub.eep ? !eepGeometry(sub.size, sub.option, sub.level, n, br, info) : !uepGeometry(sub.uepIndex, sz, br, lv, info)) return false;
            const int bits = sub.size * kCuBits;
            std::vector<int8_t>& h = hist[count % 16];
            if (src) h.assign(src, src + bits);
            else h.assign((size_t)bits, 0);
            count++;
            // The first logical frames are decoded before all of their 16 CIFs are in: the ones from before the start are erased (6 of 16 at
            // 10 CIFs, which the code still copes with at a fair SNR; a frame that fails is caught by Reed-Solomon and the CRCs). That starts the
            // audio about 140 ms sooner after a lock or a service switch.
            if (count < (uint64_t)early) return true;
            static const int kMap[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
            mother.resize((size_t)(4 * (info + 6))); one.resize((size_t)bits); dec.resize((size_t)info); bytes.resize((size_t)info / 8);
            int zeros = 0;
            for (int i = 0; i < bits; i++) {
                const uint64_t back = (uint64_t)(15 - kMap[i & 15]);
                if (back >= count) { one[(size_t)i] = 0; zeros++; continue; }   // a CIF from before the start
                const uint64_t t = count - 1 - back;
                const std::vector<int8_t>& c = hist[t % 16];
                const int8_t v = (size_t)i < c.size() ? c[(size_t)i] : (int8_t)0;
                one[(size_t)i] = v;
                zeros += v == 0;
            }
            if (count < 16 && zeros * 8 > bits * 3) return true;   // more than 3/8 missing at the start: not worth trying
            if (sub.eep ? !eepDepuncture(one.data(), sub.size, sub.option, sub.level, mother.data()) : !uepDepuncture(one.data(), sub.uepIndex, mother.data())) return false;
            viterbiDecode(mother.data(), info, dec.data());
            descramble(dec.data(), info);
            for (int i = 0; i < info / 8; i++) { uint8_t v = 0; for (int b = 0; b < 8; b++) v = (uint8_t)((v << 1) | dec[(size_t)i * 8 + (size_t)b]); bytes[(size_t)i] = v; }
            out(bytes.data(), (int)bytes.size(), zeros * 4 > bits * 3);
            return true;
        }
    };
    SubDec mainDec, spiDecI;
    // selected sub-channel
    int selReq = -1, sel = -1;
    bool selCa = false;       // the selected audio component is scrambled (conditional access): not played, the panel says so
    DabSubchannel selSub;
    DabAudio audio;
    std::function<void(int, const uint8_t*, int)> tap;
    // a DMB video sub-channel (TS 102 427 / 102 428): its transport stream for the player
    bool selDmb = false;
    DmbDecoder dmb;
    std::function<void(const uint8_t*, size_t, double)> pktCb;
    // Journaline: the data groups of a packet mode component (selPktAddr >= 0) or of the PAD of the selected audio service
    JournalineDecoder jl;
    DabPacketDemux demux;
    int selPktAddr = -1, selJlExt = 0;
    bool selJlPad = false;
    // SPI (programme guide): a second sub-channel decoded next to the selected one (packet mode data service), its data groups go to spiDec
    DabSpiDecoder spiDec;
    DabPacketDemux spiDemux;
    int spiReq = -1, spiAddr = -1, spiSel = -1;
    bool spiDirty = false;
    DabSubchannel spiSub;

    Impl() : bin(kCarriers), prsConj(kTu) {
        const auto& tab = freqInterleaver();
        for (int n = 0; n < kCarriers; n++) bin[(size_t)n] = (tab[(size_t)n] + kTu) % kTu;
        const auto& x = phaseReference();
        for (int i = 0; i < kTu; i++) prsConj[(size_t)i] = std::conj(x[(size_t)i]);
        ficSoft.resize((size_t)kFicSymbols * 2 * kCarriers);
        mscSoft.resize((size_t)(kSymbols - 1 - kFicSymbols) * 2 * kCarriers);
        audio.select(-1, false, 0);
        audio.setDataGroupSink([this](const uint8_t* g, size_t n) { if (selJlPad) jl.feedDataGroup(g, n); });
        audio.setMotSink([this](const MotObject& o) { if (spiReq >= 0 && spiAddr == -2) spiDec.motObject(o); });   // SPI in the PAD of the selected service (spiAddr -2)
        dmb.setPacketSink([this](const uint8_t* pk, size_t n) { if (pktCb) pktCb(pk, n, 0.024); });
    }

    // ------------------------------------------------------------ helpers
    const cf32* at(uint64_t abs) const { return &buf[(size_t)(abs - base)]; }

    // FFT of a window of kTu samples starting at `abs`, derotated by `cfoBins` (carrier offset in bins)
    void windowFft(uint64_t abs, double cfoBins, std::vector<cf32>& out) {
        out.resize(kTu);
        if (abs < base || abs - base + (uint64_t)kTu > (uint64_t)buf.size()) { std::fill(out.begin(), out.end(), cf32(0, 0)); return; }   // never read outside the buffer
        const cf32* s = at(abs);
        double x = cfoBins * (double)abs / kTu;
        x -= std::floor(x);
        const double w = -2.0 * kPi * cfoBins / kTu;
        cd rot = std::polar(1.0, -2.0 * kPi * x), step = std::polar(1.0, w);
        for (int i = 0; i < kTu; i++) {
            out[(size_t)i] = cf32((float)(s[i].real() * rot.real() - s[i].imag() * rot.imag()), (float)(s[i].real() * rot.imag() + s[i].imag() * rot.real()));
            rot *= step;
            if ((i & 255) == 255) rot /= std::abs(rot);
        }
        fft.forward(out.data());
    }

    // Correlates a spectrum (already derotated, or shifted by `shift` bins) with the phase reference; returns the peak / median ratio
    // and the peak position (a signed offset in samples: the window starts `idx` samples before the true useful start when idx < 0)
    // The same search without the median: the peak against the mean power of the impulse response, for ranking many (position, shift) pairs cheaply;
    // the candidates it picks are then measured with cirOf
    float cirFast(const std::vector<cf32>& R, int shift, std::vector<cf32>& tmp) {
        tmp.resize(kTu);
        const int s = ((shift % kTu) + kTu) % kTu;
        const int first = kTu - s;   // k + s wraps at k = first
        for (int k = 0; k < first; k++) tmp[(size_t)k] = R[(size_t)(k + s)] * prsConj[(size_t)k];
        for (int k = first; k < kTu; k++) tmp[(size_t)k] = R[(size_t)(k + s - kTu)] * prsConj[(size_t)k];
        fft.inverse(tmp.data());
        float pk = 0, sum = 0;
        for (int i = 0; i < kTu; i++) { const float n = std::norm(tmp[(size_t)i]); sum += n; if (n > pk) pk = n; }
        return pk / (sum * (1.f / (float)kTu) + 1e-18f);
    }

    float cirOf(const std::vector<cf32>& R, int shift, int& idx, std::vector<cf32>& tmp, std::vector<float>* mag = nullptr) {
        tmp.assign(kTu, cf32(0, 0));
        for (int k = 0; k < kTu; k++) tmp[(size_t)k] = R[(size_t)((k + shift + kTu) % kTu)] * prsConj[(size_t)k];
        fft.inverse(tmp.data());
        static thread_local std::vector<float> m;
        m.resize(kTu);
        float pk = 0; int pi = 0;
        for (int i = 0; i < kTu; i++) { m[(size_t)i] = std::abs(tmp[(size_t)i]); if (m[(size_t)i] > pk) { pk = m[(size_t)i]; pi = i; } }
        if (mag) { mag->assign(m.begin(), m.end()); }
        static thread_local std::vector<float> c;
        c = m;
        std::nth_element(c.begin(), c.begin() + kTu / 2, c.end());
        idx = pi < kTu / 2 ? pi : pi - kTu;
        return pk / (c[(size_t)kTu / 2] + 1e-9f);
    }

    // Mean sample power over the first `symbols` symbols of the frame whose useful part starts at U (every 7th sample)
    double framePower(uint64_t U, int symbols = kSymbols - 1) const {
        const uint64_t a = U >= (uint64_t)kTg ? U - (uint64_t)kTg : 0, b = U + (uint64_t)symbols * kTs - (uint64_t)kTg;
        if (a < base || b > base + buf.size()) return 0;
        double s = 0; size_t n = 0;
        for (uint64_t i = a; i < b; i += 7) { s += std::norm(buf[(size_t)(i - base)]); n++; }
        return n ? s / (double)n : 0;
    }

    // Where the useful part starts, from the impulse response `mag` of a window (index = delay from its start, `peak` the strongest path as a
    // signed offset). The strongest path is not always the one to follow: in a single frequency network a weaker transmitter is often heard
    // first, and two of nearly equal strength make the strongest one jump between them. The window goes on the earliest path within 10 dB of
    // the strongest when all of them fit in the guard interval; when they do not, where the most of their energy does.
    int placeWindow(const std::vector<float>& mag, int peak) const {
        if (mag.size() != (size_t)kTu) return peak;
        auto P = [&](int off) { const float m = mag[(size_t)(((off % kTu) + kTu) % kTu)]; return m * m; };
        static thread_local std::vector<float> c;
        c.resize(kTu);
        for (int i = 0; i < kTu; i++) c[(size_t)i] = mag[(size_t)i] * mag[(size_t)i];
        std::nth_element(c.begin(), c.begin() + kTu / 2, c.end());
        const float pk = P(peak), thr = std::max(0.1f * pk, 12.f * c[(size_t)kTu / 2]);
        const int span = kTg - kBackoff;   // paths from kBackoff before to span after the useful start are free of interference
        int first = peak, last = peak;
        for (int o = peak - span; o < peak; o++) if (P(o) >= thr) { first = o; break; }
        for (int o = peak + span; o > peak; o--) if (P(o) >= thr) { last = o; break; }
        if (last - first <= span - 8) return first;
        int best = peak; double bestE = -1;
        for (int u = first; u <= last; u++) {
            double e = 0;
            for (int o = u - kBackoff; o <= u + span; o++) { const float v = P(o); if (v >= thr) e += v; }
            if (e > bestE + 1e-12) { bestE = e; best = u; }
        }
        return best;
    }

    void trim(uint64_t keepFrom) {
        if (keepFrom <= base) return;
        const size_t drop = (size_t)std::min<uint64_t>(keepFrom - base, buf.size());
        buf.erase(buf.begin(), buf.begin() + (long)drop);
        base += drop;
    }

    // ------------------------------------------------------------ search
    uint64_t searchNext = 0;   // no phase reference search before the samples have reached this absolute index
    int searchFails = 0;       // searches that found a null but no phase reference, since the last lock
    bool quietOn = false;
    uint64_t quietStart = 0, quietSamples = 0, quietPub = 0;   // the stretch of input searched without a null symbol in it

    bool searchSync() {
        const size_t need = (size_t)kFrame + kTnull + 4096 + kTs;
        if (buf.size() < need) return false;
        // After a false null the next search waits a while: on a channel with no DAB the power varies (other services, bursts, gain changes) and the
        // phase reference search, the costly part, would otherwise run on every block of samples that comes in
        if (base + buf.size() < searchNext) { trim(base + buf.size() - need); return false; }
        // energy in windows of 2048 samples (stride 128) over one frame: the minimum lies in the null symbol
        const int win = 2048, stride = 128;
        std::vector<double> pw(buf.size() / 64 + 1, 0.0);   // block powers
        const size_t blocks = (kFrame + kTnull) / 64;
        for (size_t b = 0; b < blocks; b++) { double s = 0; for (int i = 0; i < 64; i++) s += std::norm(buf[b * 64 + (size_t)i]); pw[b] = s; }
        double best = 1e300; size_t bestPos = 0;
        double run = 0; const int nb = win / 64;
        for (int i = 0; i < nb; i++) run += pw[(size_t)i];
        for (size_t b = 0; b + (size_t)nb <= (size_t)kFrame / 64; b += stride / 64) {
            double s = 0; for (int i = 0; i < nb; i++) s += pw[b + (size_t)i];
            if (s < best) { best = s; bestPos = b * 64; }
        }
        double mean = 0; size_t cnt = 0;
        for (size_t b = 0; b < (size_t)kFrame / 64; b++) { mean += pw[b]; cnt++; }
        mean /= (double)cnt * 64.0;
        if (mean <= 1e-12 || best / (double)win > 0.6 * mean) {   // no null: not a DAB frame (yet)
            const uint64_t end = base + buf.size();
            if (!quietOn) { quietOn = true; quietStart = base; }
            quietSamples = end - quietStart;
            if (quietSamples >= quietPub + (uint64_t)kFrame) { quietPub = quietSamples; publish(); }   // for the scan: it moves on when nothing like DAB is there
            trim(end - (size_t)kFrame);
            return false;
        }
        quietOn = false; quietSamples = 0; quietPub = 0;
        // the PRS useful part starts about 1832 samples after the centre of the quietest window; search around it
        const long nominal = (long)bestPos + win / 2 + 1832;
        float bestPk = 0; long bestP = 0; int bestShift = 0, bestIdx = 0;
        std::vector<cf32> R, tmp;
        struct Cand { float fast; long p; int sh; };
        Cand top[3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        // a re-acquisition after a lost sync only looks near the carrier offset it had (53 times less work, and no false lock far off)
        const int shLo = quick ? (int)std::lround(cfo) - kQuickShift : -kMaxShift, shHi = quick ? (int)std::lround(cfo) + kQuickShift : kMaxShift;
        for (long p = nominal - 1400; p <= nominal + 1000; p += 128) {
            if (p < 0 || p + kTu > (long)buf.size()) continue;
            windowFft(base + (uint64_t)p, 0.0, R);
            for (int sh = shLo; sh <= shHi; sh++) {
                const float f = cirFast(R, sh, tmp);
                if (f > top[2].fast) {
                    top[2] = {f, p, sh};
                    std::sort(top, top + 3, [](const Cand& a, const Cand& b) { return a.fast > b.fast; });
                }
            }
        }
        for (const Cand& c : top) {   // the best few by the fast measure, measured as before
            if (c.fast <= 0) continue;
            windowFft(base + (uint64_t)c.p, 0.0, R);
            int idx;
            const float pk = cirOf(R, c.sh, idx, tmp);
            if (pk > bestPk) { bestPk = pk; bestP = c.p; bestShift = c.sh; bestIdx = idx; }
        }
        if (bestPk < 25.f) {
            if (quick && ++quickTries < 6) {   // the signal may still be fading: look again near the same offset, the spectrum is not mirrored
                trim(base + buf.size() - (size_t)kFrame);
                return false;
            }
            if (quick) { quick = false; haveLast = false; lastCif = -1; mainDec.reset(); spiDecI.reset(); audio.flushSync(); }   // a full search: start over
            // no phase reference: the next attempt looks at the mirrored spectrum (I and Q swapped by the radio or the file format)
            mirror = !mirror;
            for (auto& v : buf) v = std::conj(v);
            trim(base + buf.size() - (size_t)kFrame);
            searchFails++;
            searchNext = base + buf.size() + (uint64_t)(kFrame / 2) * (uint64_t)std::min(searchFails, 6);   // half a frame, up to three
            return false;
        }
        const uint64_t U = base + (uint64_t)(bestP + bestIdx);
        cfo = bestShift;
        nextU = U;
        fresh = true;
        missed = 0;
        state = 2;
        searchFails = 0; searchNext = 0;
        if (quick) relocks++;
        else tii.reset();   // a new lock: which frames carry TII may have changed sides
        quick = false; quickTries = 0;
        cirPeak = bestPk;
        syncSample = U;
        trim(U > 2 * kTs ? U - 2 * kTs : 0);
        return true;
    }

    // ------------------------------------------------------------ soft bits with a channel estimate
    // Plain differential detection (the product of two noisy symbols) loses about 3 dB against coherent detection. The channel is known at the
    // phase reference symbol and changes slowly from symbol to symbol, so it is estimated: from the phase reference first, then from each symbol
    // with the decided symbols taken out. The estimate is cleaned in the delay domain: only the taps of the impulse response that stand out of
    // the noise in the phase reference symbol are kept (a few dozen of 2048), which takes the noise and most decision errors out of it. The soft
    // bits of each DQPSK symbol d_l then come from both received symbols (max-log over the unknown absolute phase s_{l-1}):
    //   M(d) = max_s Re(conj(s) (conj(H_{l-1}) y_{l-1} + conj(H_l) y_l conj(d)))
    // which turns into differential detection when the estimate is poor and into coherent detection when it is good. It does not depend on
    // the absolute phase of the estimate in steps of 90 degrees, so a slipped estimate costs nothing. DECT2_DAB_BASELINE=1 turns it and the other decoding changes of 0.3.0 off (for the before/after measurement in test_dab_sens).
    bool diffOnly = baselineMode();
    std::vector<cf32> ys;                     // the spectra of the frame's symbols, carrier n of symbol l at ys[l * kCarriers + n]
    std::vector<cf32> hA, hB, hP, hTmp;
    bool coherent = false;                    // the last frame's soft bits came from the channel estimate (shown in the telemetry)
    std::vector<float> tapMask;
    double llrRef = 0;                        // slow average of |LLR| over good frames (the scale of the soft bits, as magRef)

    // The channel at the carriers from the raw estimates e (one per carrier): through the delay domain, only the masked taps kept
    void cleanEstimate(const cf32* e, cf32* out) {
        hTmp.assign(kTu, cf32(0, 0));
        for (int n = 0; n < kCarriers; n++) hTmp[(size_t)bin[(size_t)n]] = e[n];
        fft.inverse(hTmp.data());
        for (int i = 0; i < kTu; i++) hTmp[(size_t)i] *= tapMask[(size_t)i];
        fft.forward(hTmp.data());
        for (int n = 0; n < kCarriers; n++) out[n] = hTmp[(size_t)bin[(size_t)n]];
    }

    bool channelSoft(uint64_t, bool good) {
        coherent = false;
        if (ys.size() != (size_t)kSymbols * kCarriers) return false;
        std::vector<cf32> e(kCarriers);
        // the phase reference: the raw estimate and the taps that stand out (12 times the median tap power), widened by two samples
        for (int n = 0; n < kCarriers; n++) e[(size_t)n] = ys[(size_t)n] * prsConj[(size_t)bin[(size_t)n]];
        hTmp.assign(kTu, cf32(0, 0));
        for (int n = 0; n < kCarriers; n++) hTmp[(size_t)bin[(size_t)n]] = e[(size_t)n];
        fft.inverse(hTmp.data());
        std::vector<float> p(kTu), c;
        for (int i = 0; i < kTu; i++) p[(size_t)i] = std::norm(hTmp[(size_t)i]);
        c = p;
        std::nth_element(c.begin(), c.begin() + kTu / 2, c.end());
        const float thr = 12.f * c[(size_t)kTu / 2];
        tapMask.assign(kTu, 0.f);
        int kept = 0;
        for (int i = 0; i < kTu; i++)
            if (p[(size_t)i] > thr) for (int d = -2; d <= 2; d++) tapMask[(size_t)((i + d + kTu) % kTu)] = 1.f / kTu;
        for (float m : tapMask) kept += m > 0;
        if (kept == 0 || kept > kTu / 4) return false;   // nothing stands out (no signal) or nothing is sparse (noise-like): plain differential
        hA.resize(kCarriers); hB.resize(kCarriers);
        cleanEstimate(e.data(), hA.data());
        static const float r2 = 0.70710678f;
        std::vector<float> L((size_t)(kSymbols - 1) * 2 * kCarriers);
        double sum = 0, dH = 0, pH = 0;
        hP.assign(hA.begin(), hA.end());   // the estimate two symbols back (a straight line through the last two predicts the next)
        for (int l = 1; l < kSymbols; l++) {
            const cf32* y0 = &ys[(size_t)(l - 1) * kCarriers];
            const cf32* y1 = &ys[(size_t)l * kCarriers];
            const bool diag = l & 1;   // the phase reference is on the axes, every DQPSK step turns by an odd multiple of 45 degrees
            // decide s_l with the estimate of the symbol before, then estimate the channel of this one
            for (int n = 0; n < kCarriers; n++) {
                const cf32 z = std::conj(2.f * hA[(size_t)n] - hP[(size_t)n]) * y1[n];
                cf32 s;
                if (diag) s = cf32(z.real() >= 0 ? r2 : -r2, z.imag() >= 0 ? r2 : -r2);
                else if (std::fabs(z.real()) >= std::fabs(z.imag())) s = cf32(z.real() >= 0 ? 1.f : -1.f, 0.f);
                else s = cf32(0.f, z.imag() >= 0 ? 1.f : -1.f);
                e[(size_t)n] = y1[n] * std::conj(s);
            }
            cleanEstimate(e.data(), hB.data());
            // the noise of this symbol: what is left after the channel times the decided symbol (wrong decisions make it larger, not smaller)
            double res = 0, pw = 0;
            for (int n = 0; n < kCarriers; n++) { res += std::norm(e[(size_t)n] - hB[(size_t)n]); pw += std::norm(hB[(size_t)n]); }
            const float g = (float)(2.0 * kCarriers / std::max(res, 1e-6 * pw + 1e-30));   // 2 / sigma^2
            float* o = &L[(size_t)(l - 1) * 2 * kCarriers];
            for (int n = 0; n < kCarriers; n++) {
                const cf32 A = std::conj(hA[(size_t)n]) * y0[n], B = std::conj(hB[(size_t)n]) * y1[n];
                // log of the likelihood of d = (dr + j di) / sqrt 2, summed over the four s_{l-1} (log-MAP with the Jacobian correction)
                auto M = [&](float dr, float di) {
                    const float wr = g * (A.real() + (B.real() * dr + B.imag() * di) * r2), wi = g * (A.imag() + (B.imag() * dr - B.real() * di) * r2);
                    if (diag) return maxStar(maxStar(wr, -wr), maxStar(wi, -wi));   // s_{l-1} on the axes
                    const float a = (wr + wi) * r2, b = (wr - wi) * r2;
                    return maxStar(maxStar(a, -a), maxStar(b, -b));
                };
                const float mpp = M(1, 1), mpm = M(1, -1), mmp = M(-1, 1), mmm = M(-1, -1);
                const float l0 = maxStar(mmp, mmm) - maxStar(mpp, mpm);   // > 0: the real part negative, bit 1
                const float l1 = maxStar(mpm, mmm) - maxStar(mpp, mmp);
                o[n] = l0; o[kCarriers + n] = l1;
                sum += std::fabs(l0) + std::fabs(l1);
            }
            for (int n = 0; n < kCarriers; n++) { dH += std::norm(hB[(size_t)n] - hA[(size_t)n]); pH += std::norm(hA[(size_t)n]); }
            hP.swap(hA);
            hA.swap(hB);
        }
        // A channel that changes fast (Doppler of some 40 Hz and more, a car on a motorway in Band III) makes the decided symbols, and with them
        // the estimate, unreliable: that frame goes back to plain differential detection, which follows the channel without decisions.
        coherent = std::isfinite(sum) && std::isfinite(dH) && dH < 0.05 * pH;
        if (!coherent) return false;
        const double mean = sum / ((double)L.size()) + 1e-12;
        if (good) llrRef = llrRef > 0 ? 0.8 * llrRef + 0.2 * mean : mean;
        const float sc = (float)(40.0 / std::max(mean, llrRef));
        auto q8 = [&](float v) { return (int8_t)std::max(-127.f, std::min(127.f, std::round(v * sc))); };
        for (int l = 1; l < kSymbols; l++) {
            const float* o = &L[(size_t)(l - 1) * 2 * kCarriers];
            int8_t* dst = l <= kFicSymbols ? &ficSoft[(size_t)(l - 1) * 2 * kCarriers] : &mscSoft[(size_t)(l - kFicSymbols - 1) * 2 * kCarriers];
            for (int i = 0; i < 2 * kCarriers; i++) dst[i] = q8(o[i]);
        }
        return true;
    }

    // ------------------------------------------------------------ one frame
    bool processFrame() {
        const uint64_t needEnd = nextU + (uint64_t)kSymbols * kTs + kTu + 256;
        if (base + buf.size() < needEnd) return false;
        if (nextU < base + (uint64_t)kTg + 64) { state = 0; return true; }   // the cyclic prefix of the first symbol is read from U - kTg: the samples must still be buffered
        uint64_t U = nextU;
        std::vector<cf32> R, tmp;
        const bool wasFresh = fresh;
        if (!fresh) {   // refine the timing with the phase reference correlation (the window sits inside the guard interval)
            const uint64_t p = U - 64;
            windowFft(p, cfo, R);
            int idx;
            std::vector<float> mag;
            cirPeak = cirOf(R, 0, idx, tmp, &mag);
            if (cirPeak >= 12.f) {
                const uint64_t Un = p + (uint64_t)(int64_t)placeWindow(mag, idx);
                // the sample clock error, from how far the frame moved against the prediction (jumps between echoes are left out)
                const int64_t e = (int64_t)Un - (int64_t)U;
                if (!missed && std::llabs(e) <= 48) sro = std::max(-4e-4, std::min(4e-4, sro + 0.25 * (double)e / kFrame));
                U = Un; missed = 0; cirKeep = mag;
            }
            else {
                missed++;
                // A fade (the frame far weaker than the good ones) keeps the timing and the carrier offset and is decoded: its soft bits come out
                // weak and the de-interleaver keeps going. A frame of normal strength without the phase reference where it should be means lost
                // samples or a jump: it is not decoded (its CIFs go in as erasures) and the sync is found again at once, near the offset known.
                // (the power of the phase reference symbol itself: a fade that ends within the frame still took the reference)
                const double pw = framePower(U, 1);
                const bool fade = powRef > 0 && pw < 0.3 * powRef;
                if (!fade || missed >= kCoast) {
                    state = 0; quick = true; quickTries = 0; syncLosses++;
                    trim(U > (uint64_t)kFrame / 2 + 2 * kTs ? U - (uint64_t)kFrame / 2 - 2 * kTs : 0);   // the search starts before the frame expected
                    publish();
                    return true;
                }
            }
        } else {
            windowFft(U, cfo, R);
            int idx; std::vector<float> mag;
            cirPeak = cirOf(R, 0, idx, tmp, &mag);
            cirKeep = mag;
            fresh = false;
        }
        // carrier offset from the cyclic prefixes of the whole frame (raw samples: total offset, unwrapped around the previous value)
        {
            cd acc = 0;
            for (int l = 0; l < kSymbols; l++) {
                const uint64_t u = symStart(U, l);
                const cf32* a = at(u - kTg);
                const cf32* b = at(u + kTu - kTg);
                for (int i = 0; i < kTg; i++) acc += cd(a[i].real(), -a[i].imag()) * cd(b[i].real(), b[i].imag());
            }
            const double e = std::arg(acc) / (2.0 * kPi);
            const double d = e - cfo;
            double nc = missed ? cfo : cfo + d - std::round(d);   // held through a fade (the cyclic prefixes of noise say nothing)
            if (wasFresh) {
                // The integer part comes from the search at the start and the cyclic prefix only knows the offset modulo one carrier spacing. At an
                // offset of n + 1/2 spacings both neighbours of the true value look equally good in the search and the unwrapping above may take
                // the wrong one, a whole spacing off. The first frame after a lock therefore picks the candidate with the sharpest phase reference peak.
                double bestC = nc;
                float bestPk = -1.f;
                for (int k = -1; k <= 1; k++) {
                    std::vector<cf32> Rc, tc;
                    int ix;
                    windowFft(U, nc + k, Rc);
                    const float pk = cirOf(Rc, 0, ix, tc);
                    if (pk > bestPk) { bestPk = pk; bestC = nc + k; }
                }
                nc = bestC;
            }
            cfo = nc;
        }
        // TII: the spectrum of the null symbol of this frame. A window of T_u in its middle stays clear of the end of the frame before and
        // of the echoes that run into the phase reference's guard interval (the null symbol is 608 samples longer than T_u)
        {
            const uint64_t back = (uint64_t)kTg + (uint64_t)kTnull - (uint64_t)(kTnull - kTu) / 2;   // how far before U the null window starts
            if (U >= base + back) {   // just after a lock the buffer may not reach back that far (a read before the buffer crashed on slow starts)
                std::vector<cf32> N;
                windowFft(U - back, cfo, N);
                tii.addNull(N, framesDone);
            }
        }
        // demodulate all symbols
        std::vector<cf32> prev, cur;
        windowFft(U - kBackoff, cfo, prev);
        // pass 1: differential cells; scale by the mean magnitude. With a sample clock error the windows follow the symbols (symStart) and the
        // fraction of a sample left between two symbols, a phase slope across the carriers, is turned out of their product.
        std::vector<cf32> q((size_t)(kSymbols - 1) * kCarriers);
        static thread_local std::vector<cf32> rot;
        rot.resize((size_t)kCarriers + 1);
        const auto& carrier = freqInterleaver();
        double magSum = 0, restPrev = 0;
        ys.resize((size_t)kSymbols * kCarriers);
        for (int n = 0; n < kCarriers; n++) ys[(size_t)n] = prev[(size_t)bin[(size_t)n]];
        for (int l = 1; l < kSymbols; l++) {
            double rest = 0;
            windowFft(symStart(U, l, &rest) - kBackoff, cfo, cur);
            {   // the spectrum kept for the channel estimation, the fraction of a sample turned out (the same slope as below, absolute)
                cf32* y = &ys[(size_t)l * kCarriers];
                if (std::fabs(rest) > 1e-4) {
                    const cd step = std::polar(1.0, 2.0 * kPi * rest / kTu);
                    cd z = std::polar(1.0, -2.0 * kPi * rest * (kCarriers / 2) / kTu);
                    for (int i = 0; i <= kCarriers; i++) { rot[(size_t)i] = cf32((float)z.real(), (float)z.imag()); z *= step; }
                    for (int n = 0; n < kCarriers; n++) y[n] = cur[(size_t)bin[(size_t)n]] * rot[(size_t)(carrier[(size_t)n] + kCarriers / 2)];
                } else for (int n = 0; n < kCarriers; n++) y[n] = cur[(size_t)bin[(size_t)n]];
            }
            const double dr = rest - restPrev;
            restPrev = rest;
            const bool turn = std::fabs(dr) > 1e-4;
            if (turn) {   // exp(j 2 pi k dr / kTu) for the carriers k = -768..768
                const cd step = std::polar(1.0, 2.0 * kPi * dr / kTu);
                cd z = std::polar(1.0, -2.0 * kPi * dr * (kCarriers / 2) / kTu);
                for (int i = 0; i <= kCarriers; i++) { rot[(size_t)i] = cf32((float)z.real(), (float)z.imag()); z *= step; }
            }
            cf32* o = &q[(size_t)(l - 1) * kCarriers];
            for (int n = 0; n < kCarriers; n++) {
                const int k = bin[(size_t)n];
                o[n] = cur[(size_t)k] * std::conj(prev[(size_t)k]);
                if (turn) o[n] *= rot[(size_t)(carrier[(size_t)n] + kCarriers / 2)];
                magSum += std::abs(o[n]);
            }
            // Common phase error: oscillator phase noise (and what is left of the carrier offset) turns all carriers of a symbol alike. The
            // turn between two symbols is measured from the fourth power of the cells (the DQPSK points sit at odd multiples of 45 degrees) and
            // taken out of the cells (the differential soft bits). Taking it out of the symbols for the channel estimate as well cost 0.3 dB at
            // the FIC threshold and gained nothing (those frames go differential).
            if (!baselineMode()) {
                cd s4 = 0;
                double m2s = 0;
                for (int n = 0; n < kCarriers; n++) {
                    const cd z(o[n].real(), o[n].imag());
                    const double m2 = std::norm(z);
                    if (m2 > 0) { const cd z2 = z * z; s4 += z2 * z2 / m2; m2s += m2; }
                }
                if (std::abs(s4) > 0.1 * m2s) {   // only where there is a signal (noise alone gives about 0.03)
                    const double err = std::arg(-s4) / 4.0;
                    const cf32 r1((float)std::cos(err), (float)-std::sin(err));
                    for (int n = 0; n < kCarriers; n++) o[n] *= r1;
                }
            }
            prev.swap(cur);
        }
        const double meanMag = magSum / ((double)(kSymbols - 1) * kCarriers) + 1e-12;
        // The soft bits are scaled by a slow average of the cell magnitude over the good frames, not by this frame's own: a faded frame (or one of
        // noise) then gives weak soft bits that the Viterbi decoder takes as nearly erased, instead of confident noise spread over 16 CIFs.
        if (!missed) {
            magRef = magRef > 0 ? 0.8 * magRef + 0.2 * meanMag : meanMag;
            const double pw = framePower(U);
            powRef = powRef > 0 ? 0.8 * powRef + 0.2 * pw : pw;
        }
        const float sc = (float)(40.0 / std::max(meanMag, magRef));
        auto soft = [&](int l, int8_t* dst) {   // l = 1..75: first half real parts, second half imaginary parts (positive = bit 1)
            const cf32* o = &q[(size_t)(l - 1) * kCarriers];
            for (int n = 0; n < kCarriers; n++) {
                dst[n] = (int8_t)std::max(-127.f, std::min(127.f, std::round(-o[n].real() * sc)));
                dst[kCarriers + n] = (int8_t)std::max(-127.f, std::min(127.f, std::round(-o[n].imag() * sc)));
            }
        };
        if (!diffOnly && channelSoft(U, !missed)) {}
        else {
            for (int l = 1; l <= kFicSymbols; l++) soft(l, &ficSoft[(size_t)(l - 1) * 2 * kCarriers]);
            for (int l = kFicSymbols + 1; l < kSymbols; l++) soft(l, &mscSoft[(size_t)(l - kFicSymbols - 1) * 2 * kCarriers]);
        }
        // quality: phase scatter of the differential cells around the nearest odd multiple of 45 degrees
        {
            double err2 = 0; size_t cnt = 0;
            for (int l = 10; l < 14; l++) {
                const cf32* o = &q[(size_t)(l - 1) * kCarriers];
                for (int n = 0; n < kCarriers; n++) {
                    const cd z = cd(o[n].real(), o[n].imag());
                    const double ph = std::arg(-std::pow(z, 4)) / 4.0;   // error from the grid, in (-pi/4, pi/4]
                    err2 += ph * ph; cnt++;
                }
            }
            const double v = std::max(1e-3, err2 / (double)cnt);
            snrDb = -10.0 * std::log10(v);
            constel.assign(q.begin() + (long)(8 * kCarriers), q.begin() + (long)(9 * kCarriers));
            double msum = 0;
            for (auto& c : constel) { const float m = std::abs(c); if (m > 1e-9f) c *= std::sqrt(m) / m; msum += std::sqrt(m); }   // compress the radius: four clusters on the diagonals
            if (msum > 1e-9) { const float g = (float)(constel.size() / msum); for (auto& c : constel) c *= g; }   // unit mean radius, whatever the input level
        }
        decodeFic();
        // CIFs lost since the last frame decoded: from FIG 0/0 (the CIF count) when this frame's FIC carried it, else from the samples between
        gapCifs = 0; dupFrame = false;
        if (haveLast) {
            const double fr = (double)(int64_t)(U - lastU) / kFrame;
            gapCifs = 4 * std::max(0, (int)std::llround(fr) - 1);
            dupFrame = fr < 0.5;
            if (frameCif >= 0 && lastCif >= 0) {
                const int c = (((frameCif - lastCif - 4) % 5000) + 5000) % 5000;
                if (c >= 5000 - 64) dupFrame = true;                 // the count went back: this frame was decoded already
                else if (c < 2500) { gapCifs = c; dupFrame = false; }
            }
        }
        if (!dupFrame) {
            decodeMsc();
            lastCif = frameCif >= 0 ? frameCif : (lastCif >= 0 ? (lastCif + 4 + gapCifs) % 5000 : -1);
            lastU = U; haveLast = true;
        }
        framesDone++;
        endSample = U;
        nextU = U + (uint64_t)std::llround((double)kFrame * (1.0 + sro));
        trim(nextU > (uint64_t)kFrame / 2 + 2 * kTs ? nextU - (uint64_t)kFrame / 2 - 2 * kTs : 0);   // half a frame more is kept for a re-acquisition
        publish();
        return true;
    }

    // ------------------------------------------------------------ FIC
    void decodeFic() {
        int ok = 0;
        std::lock_guard<std::mutex> lk(mu);
        frameCif = -1;
        for (int cw = 0; cw < 4; cw++) {
            int8_t mother[3096];
            uint8_t bits[768];
            if (!ficDepuncture(&ficSoft[(size_t)cw * 2304], mother)) continue;
            viterbiDecode(mother, 768, bits);
            descramble(bits, 768);
            uint8_t by[96];
            for (int i = 0; i < 96; i++) { uint8_t v = 0; for (int b = 0; b < 8; b++) v = (uint8_t)((v << 1) | bits[i * 8 + b]); by[i] = v; }
            for (int f = 0; f < 3; f++) {
                const uint8_t* fib = by + f * 32;
                if (crc16(fib, 30) == (uint16_t)((fib[30] << 8) | fib[31])) { ok++; fibOk++; curSlot = cw * 3 + f; parseFib(fib, cw * 3 + f); }
                else { fibBad++; ficBadN++; lastFill[cw * 3 + f] = -1; }
            }
        }
        ficSeq++;
        ficFrames++;
        if (framesDone % 64 == 0) pruneFigs();
        ficRecent = ok;
        ens.valid = ens.eid != 0 && !ens.services.empty();
    }

    // The FIG with this header and payload, seen once more
    void recordFig(int type, const uint8_t* d, int len, int slot) {
        DabFigRecord r;
        r.type = type;
        r.data.assign(d, d + len);
        const dabp::FigHead fh = dabp::figHead(type, d, (size_t)len);   // the FIG header flags (dab_proto: Rust)
        r.cn = fh.cn; r.oe = fh.oe; r.pd = fh.pd; r.ext = fh.ext;
        std::string key;
        key += (char)type;
        if (type == 0 && (r.ext == 0 || r.ext == 10)) key += (char)r.ext;   // the CIF count / the clock: the latest only
        else key.append((const char*)d, (size_t)len);
        DabFicStat& st = figStat[{type, r.ext}];
        st.type = type; st.ext = r.ext;
        if (!st.count) st.firstFrame = framesDone;
        st.count++; st.bytes += (uint64_t)len + 1; st.lastFrame = framesDone;
        auto it = figs.find(key);
        if (it == figs.end()) {
            r.count = 1; r.firstFrame = r.lastFrame = framesDone; r.lastFib = slot;
            if (figs.size() < 4000) figs.emplace(std::move(key), std::move(r));
            return;
        }
        DabFigRecord& e = it->second;
        e.count++; e.lastFrame = framesDone; e.lastFib = slot;
        e.data = std::move(r.data);
    }

    // Instances not heard for 5 minutes are gone (a reconfiguration, an ensemble that changed)
    void pruneFigs() {
        const uint64_t keep = 3125;   // frames of 96 ms
        for (auto it = figs.begin(); it != figs.end();) {
            if (framesDone > it->second.lastFrame + keep) it = figs.erase(it);
            else ++it;
        }
    }

    void parseFib(const uint8_t* fib, int slot) {
        int used = 0;
        for (const dabp::FigRef& f : dabp::fibWalk(fib, 30, used)) {
            const uint8_t* d = fib + f.off;
            const int len = (int)f.len;
            recordFig(f.type, d, len, slot);
            if (f.type == 0) parseFig0(d, len);
            else if (f.type == 1) parseFig1(d, len);
            else if (f.type == 2) parseFig2(d, len);
        }
        lastFill[slot] = std::min(used, 30);
        ficFill += (uint64_t)std::min(used, 30);
        ficFibs++;
    }

    // FIG 0: the parsing is dab_proto (fig0*); this applies the entries to the ensemble tables
    void parseFig0(const uint8_t* d, int len) {
        const size_t n = (size_t)len;
        const int ext = d[0] & 31;
        // C/N = 1 in the sub-channel and service organisation: the configuration after an announced reconfiguration. Only the current one is
        // followed (the new one arrives with C/N = 0 once it is in force).
        if ((d[0] & 0x80) && (ext == 1 || ext == 2)) return;
        if (ext == 0) {
            // the CIF count (modulo 5000) of the CIF of this FIB's group: the frame's first CIF is that minus the group
            if (n >= 5) frameCif = (((d[3] & 0x1F) * 250 + d[4] - curSlot / 3) % 5000 + 5000) % 5000;
            int eid;
            if (dabp::fig0Eid(d, n, eid)) {
                // a different ensemble id on the same tuning (a new mux): drop what the previous one told us, the sub-channel ids are only valid per ensemble
                if (ens.eid != 0 && ens.eid != (uint16_t)eid) { ens = DabEnsemble(); ptyInfo.clear(); annSup.clear(); annSw.clear(); fig2Segs.clear(); fig2Done.clear(); spiDec.reset(); spiDec.clearStore(); spiDirty = true; }
                ens.eid = (uint16_t)eid;
            }
        } else if (ext == 1) {
            for (const dabp::Fig01Sub& e : dabp::fig0Subchannels(d, n)) {
                DabSubchannel s;
                s.id = e.id;
                s.start = e.start;
                if (e.eep) {
                    s.eep = true;
                    s.option = e.option;
                    s.level = e.level;
                    s.size = e.size;
                    int nn, br, info;
                    s.bitrate = eepGeometry(s.size, s.option, s.level, nn, br, info) ? br : 0;
                } else {
                    s.eep = false;
                    s.uepIndex = e.uepIndex;
                    int lv, info;
                    if (!e.uepTable && uepGeometry(s.uepIndex, s.size, s.bitrate, lv, info)) s.level = lv - 1;
                    else { s.size = 0; s.bitrate = 0; }   // table switch 1 is not defined
                }
                ens.subs[s.id] = s;
            }
        } else if (ext == 3) {   // packet mode service components: SCId -> sub-channel, address (EN 300 401 clause 6.3.2)
            for (const dabp::Fig03Packet& e : dabp::fig0Packet(d, n)) {
                DabEnsemble::PacketComp pc;
                pc.dg = e.dg;
                pc.dscty = e.dscty;
                pc.subId = e.subId;
                pc.address = e.address;
                ens.packet[e.scid] = pc;
            }
        } else if (ext == 2) {
            for (const dabp::Fig02Service& e : dabp::fig0Services(d, n)) {
                DabService& sv = ens.services[e.sid];
                sv.sid = e.sid;
                std::vector<DabComponent> comps;
                for (const dabp::Fig02Comp& c : e.comps) {
                    DabComponent dc;
                    dc.tmid = c.tmid; dc.ascty = c.ascty; dc.dscty = c.dscty; dc.subId = c.subId; dc.scid = c.scid; dc.primary = c.primary;
                    comps.push_back(dc);
                }
                sv.comps = comps;
            }
            // conditional access (not in dab_proto): the service's CAId and every component's CA flag, walked the same way
            {
                const bool pd = (d[0] >> 5) & 1;
                size_t p = 1;
                while (p + (pd ? 5 : 3) <= n) {
                    const uint32_t sid = pd ? ((uint32_t)d[p] << 24 | (uint32_t)d[p + 1] << 16 | (uint32_t)d[p + 2] << 8 | d[p + 3]) : ((uint32_t)d[p] << 8 | d[p + 1]);
                    p += pd ? 4 : 2;
                    const int caId = (d[p] >> 4) & 7, nc = d[p] & 15;
                    p++;
                    auto it = ens.services.find(sid);
                    if (it != ens.services.end()) it->second.caId = caId;
                    for (int k = 0; k < nc && p + 2 <= n; k++, p += 2)
                        if (it != ens.services.end() && k < (int)it->second.comps.size()) it->second.comps[(size_t)k].ca = d[p + 1] & 1;
                }
            }
        } else if (ext == 13) {   // user applications (TS 101 756 table 16): which sub-channel carries DMB video
            for (const dabp::Fig013App& a : dabp::fig0UserApps(d, n)) {
                if (a.type == 0x44a) {   // Journaline: its user application data (version, extended header length, point of entry)
                    DabService& sv = ens.services[a.sid];
                    sv.sid = a.sid;
                    int v, x, en;
                    if (jlUserAppData(d + a.off, (size_t)a.len, v, x, en)) { sv.journaline = true; sv.jlScIdS = a.scids; sv.jlExt = x; sv.jlEntry = en; }
                }
                if (a.type == 0x007) {   // SPI: the profiles are the bytes of the user application data up to a 0x00
                    DabService& sv = ens.services[a.sid];
                    sv.sid = a.sid;
                    sv.spi = true; sv.spiScIdS = a.scids; sv.spiProfiles.clear();
                    for (int i = 0; i < a.len && d[a.off + i] != 0; i++) sv.spiProfiles.push_back(d[a.off + i]);
                }
                if (a.scids == 0 && a.index == 0) {   // the primary component's first application (PAD applications of a DMB radio service follow it)
                    DabService& sv = ens.services[a.sid];
                    sv.sid = a.sid;
                    sv.userApp = a.type;
                    if (a.type == 0x009 && a.len >= 1) sv.dmbProfile = d[a.off];
                }
            }
        } else if (ext == 17) {   // programme type: S/D flag, P/S flag, language, programme type, complementary type
            for (const dabp::Fig017Pty& e : dabp::fig0Pty(d, n)) {
                PtyInfo pi;
                pi.lang = e.lang;
                pi.pty = e.pty;
                if (e.primary && pi.pty >= 0) { PtyInfo& x = ptyInfo[e.sid]; if (pi.lang < 0) pi.lang = x.lang; pi.dyn = e.sd; x = pi; }
            }
        } else if (ext == 18) {   // announcement support: SId, ASu flags, clusters
            for (const dabp::Fig018Support& e : dabp::fig0AnnSupport(d, n)) {
                DabAnnSupport as;
                as.asu = e.asu;
                as.clusters = e.clusters;
                annSup[e.sid] = as;
            }
        } else if (ext == 19) {   // announcement switching: cluster, ASw flags, new flag, region flag, sub-channel
            for (const dabp::Fig019Switch& e : dabp::fig0AnnSwitch(d, n)) {
                DabAnnSwitch sw;
                sw.cluster = e.cluster;
                sw.asw = e.asw;
                sw.newFlag = e.newFlag;
                sw.region = e.region;
                sw.subId = e.subId;
                annSw[sw.cluster] = {sw, ficFrames};
            }
        } else if (ext == 10) {
            dabp::Fig010Time t;
            if (dabp::fig0Time(d, n, t) && t.mjd > 40587) ens.utc = t.utc;
        }
    }

    void parseFig1(const uint8_t* d, int len) {
        dabp::Fig1Label l;
        if (!dabp::fig1(d, (size_t)len, l)) return;
        auto done = [&](int e, uint32_t id) { return fig2Done.count(std::to_string(e) + ":" + std::to_string(id)) > 0; };   // a FIG 2 label of it is known: that one is kept
        if (l.ext == 0) { if (!done(0, 0)) ens.label = labelText(l.charset, d + l.off, 16); }
        else if (l.ext == 1 || l.ext == 5) {   // programme service label (16 bit SId), data service label (32 bit SId, e.g. DMB television)
            DabService& sv = ens.services[l.sid];
            sv.sid = l.sid;
            if (!done(l.ext, l.sid)) sv.label = labelText(l.charset, d + l.off, 16);
        } else if (l.ext == 4) {   // service component label: P/D, SCIdS, SId, label
            const auto it = ens.services.find(l.sid);
            if (it != ens.services.end() && !fig2Done.count("4:" + std::to_string(l.sid) + ":" + std::to_string(l.scids))) it->second.compLabels[l.scids] = labelText(l.charset, d + l.off, 16);
        }
    }

    // FIG 2: labels in UTF-8 or UCS-2 of up to 8 segments (EN 300 401 8.1.13, TS 103 176)
    void parseFig2(const uint8_t* d, int len) {
        dabp::Fig2Seg g;
        if (!dabp::fig2(d, (size_t)len, g)) return;
        const int ext = g.ext, seg = g.seg;
        const uint32_t sid = g.sid;
        const int scids = g.scids;
        std::string key = std::to_string(ext) + ":";
        if (ext == 0) key += "0";
        else if (ext == 1 || ext == 5) key += std::to_string(sid);
        else key += std::to_string(sid) + ":" + std::to_string(scids);
        LabelSegs& L = fig2Segs[key];
        if (L.toggle != g.toggle) { L = LabelSegs(); L.toggle = g.toggle; }
        if (seg == 0) {
            L.ucs2 = g.ucs2;
            L.total = g.total;
            if (!g.valid) return;
        }
        L.segs[seg].assign(d + g.textOff, d + g.textOff + g.textLen);
        if (!L.total || (int)L.segs.size() != L.total) return;
        std::vector<uint8_t> all;
        for (int i = 0; i < L.total; i++) {
            const auto it = L.segs.find(i);
            if (it == L.segs.end()) return;
            all.insert(all.end(), it->second.begin(), it->second.end());
        }
        const std::string text = text::clean(L.ucs2 ? text::ucs2be(all.data(), all.size()) : std::string(all.begin(), all.end()));
        if (text.empty()) return;
        if (ext == 0) ens.label = text;
        else if (ext == 1 || ext == 5) { DabService& sv = ens.services[sid]; sv.sid = sid; sv.label = text; }
        else { const auto it = ens.services.find(sid); if (it == ens.services.end()) return; it->second.compLabels[scids] = text; }
        fig2Done.insert(key);
    }

    // ------------------------------------------------------------ MSC
    void applySelection() {
        // (ens is locked by the caller)
        if (selReq == sel) return;
        if (selReq < 0) { sel = -1; selDmb = false; audio.select(-1, false, 0); dmb.reset(); mainDec.reset(); return; }
        auto it = ens.subs.find(selReq);
        if (it == ens.subs.end()) return;   // wait until the ensemble tells us about it
        selSub = it->second;
        bool plus = true, isDmb = false;
        selCa = false;
        for (const auto& kv : ens.services) {
            for (const auto& c : kv.second.comps) if (c.tmid == 0 && c.subId == selReq) { plus = c.ascty == 63; selCa = c.ca; }
            const DabComponent* d = kv.second.dmb();
            if (d && d->subId == selReq) isDmb = true;
        }
        sel = selReq;
        selDmb = isDmb;
        mainDec.reset();
        dmb.reset();
        demux.setAddress(selPktAddr);
        mainDec.early = !isDmb && selPktAddr < 0 && plus && !baselineMode() ? 10 : 16;   // MP2 has no check on its audio bits: it waits for all 16
        if (isDmb || selPktAddr >= 0) audio.select(-1, false, 0);
        else audio.select(sel, plus, selSub.bitrate);
    }

    void decodeMsc() {
        decodeMain();
        decodeAux();
    }

    // The CIFs lost before this frame go in as erasures (up to 20: beyond that nothing of the 16 CIFs of interleaving is left, the decoding
    // starts over); then the frame's own four
    template <class F> void feedCifs(SubDec& d, const DabSubchannel& sub, F&& out) {
        if (gapCifs > 20) d.reset();
        else for (int k = 0; k < gapCifs; k++) d.cif(sub, nullptr, out);
        for (int c = 0; c < kCifPerFrame; c++) d.cif(sub, &mscSoft[(size_t)c * kCifBits + (size_t)sub.start * kCuBits], out);
    }

    // The programme guide sub-channel (packet mode): decoded next to the selected one, its packets go to the SPI decoder
    void decodeAux() {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (spiReq < 0) { spiSel = -1; return; }
            if (spiAddr == -2) { spiSel = -1; return; }   // SPI comes from the PAD of the selected service: no extra sub-channel
            auto it = ens.subs.find(spiReq);
            if (it == ens.subs.end()) return;
            if (spiDirty || spiSel != spiReq) {
                spiSub = it->second; spiSel = spiReq; spiDirty = false;
                spiDecI.reset();
                spiDemux.setAddress(spiAddr);
                spiDemux.setMaxGroup(16384);
            } else if (it->second.start != spiSub.start || it->second.size != spiSub.size || it->second.eep != spiSub.eep || it->second.option != spiSub.option || it->second.level != spiSub.level || it->second.uepIndex != spiSub.uepIndex) {
                spiSub = it->second; spiDecI.reset();
            }
        }
        if (spiSub.size <= 0 || spiSub.start + spiSub.size > kCifCu) return;
        feedCifs(spiDecI, spiSub, [this](const uint8_t* bytes, int n, bool) {
            if (tap) tap(spiSel, bytes, n);
            spiDemux.feed(bytes, (size_t)n, [this](const uint8_t* g, size_t m) { spiDec.dataGroup(g, m); });
        });
    }

    void decodeMain() {
        {
            std::lock_guard<std::mutex> lk(mu);
            applySelection();
            // a reconfiguration may move or resize the selected sub-channel: follow it (the 16 CIFs of interleaving restart)
            if (sel >= 0) {
                auto it = ens.subs.find(sel);
                if (it != ens.subs.end()) {
                    const DabSubchannel& n = it->second;
                    if (n.start != selSub.start || n.size != selSub.size || n.eep != selSub.eep || n.option != selSub.option || n.level != selSub.level || n.uepIndex != selSub.uepIndex) {
                        selSub = n;
                        mainDec.reset();
                        audio.flushSync();   // the superframes start over with the de-interleaver
                    }
                }
            }
        }
        if (sel < 0) return;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (const auto& kv : ens.services) for (const auto& c : kv.second.comps) if (c.tmid == 0 && c.subId == sel) selCa = c.ca;
        }
        if (selSub.size <= 0 || selSub.start + selSub.size > kCifCu) return;
        if (gapCifs > 20) audio.flushSync();   // the superframe phase is lost with them
        erasedCifs += (uint64_t)std::min(gapCifs, 20);
        feedCifs(mainDec, selSub, [this](const uint8_t* bytes, int n, bool erased) {
            if (tap) tap(sel, bytes, n);
            if (selPktAddr >= 0) demux.feed(bytes, (size_t)n, [this](const uint8_t* g, size_t m) { jl.feedDataGroup(g, m); });
            else if (selDmb) dmb.push(bytes, n);
            else if (!selCa) audio.push(bytes, n, erased);   // scrambled audio decoded would only be noise
        });
    }

    // ------------------------------------------------------------ telemetry
    void publish() {
        std::lock_guard<std::mutex> lk(mu);
        DabTelemetry t;
        t.state = state;
        t.sync = state == 2 && missed == 0;
        t.cirPeak = cirPeak;
        t.cfoHz = cfo * 1000.0 + (mirror ? -1 : 1) * cc.offsetHz();
        t.snrDb = snrDb;
        t.frames = framesDone;
        t.fibOk = fibOk; t.fibBad = fibBad; t.ficRecentOk = ficRecent;
        t.ensemble = ens.valid;
        t.ensembleLabel = ens.label;
        t.services = (int)ens.services.size();
        t.constellation = constel;
        t.cir = cirKeep;
        t.audio = audio.stats();
        t.dmb = dmb.stats();
        t.dmb.active = selDmb;
        t.dmb.sub = selDmb ? sel : -1;
        for (const auto& f : tii.found()) t.tii.push_back({f.mainId, f.subId, f.levelDb, f.marginDb});
        t.tiiFrames = tii.framesSeen();
        t.sroPpm = sro * 1e6;
        t.coherent = coherent;
        t.scrambled = sel >= 0 && selCa;
        t.quietMs = state == 2 ? 0.0 : 1000.0 * (double)quietSamples / kRate;
        t.syncLosses = syncLosses; t.relocks = relocks; t.erasedCifs = erasedCifs; t.impulses = impulses;
        t.seq = ++telSeq;
        tel = std::move(t);
    }

    void reset() {
        buf.clear(); base = 0; state = 0; mirror = false; cfo = 0; searchNext = 0; searchFails = 0; quietOn = false; quietSamples = 0; quietPub = 0; cc.reset(); missed = 0; framesDone = 0; rs.reset(); tii.reset();
        magRef = 0; powRef = 0; sro = 0; quick = false; quickTries = 0; haveLast = false; lastCif = -1; frameCif = -1; gapCifs = 0; impPow = 0;
        syncLosses = 0; relocks = 0; erasedCifs = 0; impulses = 0;
        std::lock_guard<std::mutex> lk(mu);
        telSeq++;
        ens = DabEnsemble();   // services of the previous mux must not survive a retune (#179)
        spiDec.reset(); spiDec.clearStore(); spiDirty = true; spiDecI.reset();
        tel = DabTelemetry(); tel.seq = telSeq;
        mainDec.reset();
        ptyInfo.clear(); annSup.clear(); annSw.clear(); fig2Segs.clear(); fig2Done.clear();
        figs.clear(); figStat.clear(); ficFill = 0; ficFibs = 0; ficBadN = 0; ficFrames = 0; ficSeq++;
        for (int& v : lastFill) v = 0;
        audio.flushSync();
        dmb.reset();
    }

    void run() {
        for (int guard = 0; guard < 64; guard++) {
            if (state == 0) { if (!searchSync()) return; }
            else if (!processFrame()) return;
        }
    }
};

DabReceiver::DabReceiver() : p_(new Impl) {}
DabReceiver::~DabReceiver() = default;

void DabReceiver::configure(double inputRateHz) {
    Impl& I = *p_;
    I.inRate = inputRateHz;
    I.resample = std::fabs(inputRateHz - kRate) > 1.0;
    if (I.resample) I.rs.configure(inputRateHz, kRate, nullptr, 1.6e6);
    // an ensemble beyond the search's +-80 kHz is moved to the middle when there is no lock after 0.5 s (the lock takes 0.2 s, p90, down to 3 dB)
    I.cc.configure(inputRateHz, 1.536e6, 60e3, 0.5);
    I.reset();
}

void DabReceiver::reset() { p_->reset(); }

void DabReceiver::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    // a channel away from the middle of the sample band (beyond the +-80 kHz of the search) is moved there first
    I.cin.assign(x, x + n);
    // samples that are not numbers (a broken driver or file) are zeroed first: one would spread through the filters, the FFTs and the channel estimate
    for (cf32& v : I.cin) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) { v = cf32(0, 0); I.impulses++; }
    I.cc.process(I.cin.data(), n, I.state == 2 && I.fibOk > 0);
    if (I.cc.takeChanged()) { I.buf.clear(); I.base = 0; I.state = 0; I.searchNext = 0; I.searchFails = 0; I.quietOn = false; I.quietSamples = 0; I.quietPub = 0; I.rs.reset(); }
    x = I.cin.data();
    const size_t from = I.buf.size();
    if (I.resample) { I.rsOut.clear(); I.rs.process(x, n, I.rsOut); I.buf.insert(I.buf.end(), I.rsOut.begin(), I.rsOut.end()); }
    else I.buf.insert(I.buf.end(), x, x + n);
    if (I.mirror) for (size_t i = from; i < I.buf.size(); i++) I.buf[i] = std::conj(I.buf[i]);
    // Impulse blanker: a sample 12 dB over the mean power (ignition, switching, a spark) is zeroed. OFDM itself goes that high about once in
    // ten million samples; a zeroed sample costs the symbol far less than the impulse would.
    if (I.buf.size() > from) {
        double s = 0;
        for (size_t i = from; i < I.buf.size(); i++) s += std::min<double>(std::norm(I.buf[i]), I.impPow > 0 ? 16.0 * I.impPow : 1e30);
        const double m = s / (double)(I.buf.size() - from);
        if (std::isfinite(m)) I.impPow = I.impPow > 0 ? 0.95 * I.impPow + 0.05 * m : m;
        if (I.impPow > 0) {
            const float lim = (float)(16.0 * I.impPow);
            for (size_t i = from; i < I.buf.size(); i++) if (std::norm(I.buf[i]) > lim) { I.buf[i] = cf32(0, 0); I.impulses++; }
        }
    }
    I.run();
    I.audio.tick();
}

bool DabReceiver::telemetry(DabTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    out.audio = p_->audio.stats();
    return true;
}

DabEnsemble DabReceiver::ensemble() const {
    const int sub = p_->audio.stats().sub;
    const std::string dls = sub >= 0 ? p_->audio.dynamicLabel() : std::string();
    bool running = false;
    const std::vector<DabDlpTag> dlp = sub >= 0 ? p_->audio.dlPlus(&running) : std::vector<DabDlpTag>();
    std::lock_guard<std::mutex> lk(p_->mu);
    DabEnsemble e = p_->ens;
    if (!dls.empty()) for (auto& kv : e.services) { const DabComponent* c = kv.second.playable(); if (c && c->subId == sub) { kv.second.dls = dls; kv.second.dlPlus = dlp; kv.second.dlpRunning = running; } }
    for (auto& kv : e.services) {
        const auto it = p_->ptyInfo.find(kv.first);
        if (it != p_->ptyInfo.end()) { kv.second.pty = it->second.pty; kv.second.ptyDynamic = it->second.dyn; kv.second.language = it->second.lang; }
    }
    e.annSupport = p_->annSup;
    for (const auto& kv : p_->annSw)   // an announcement is on while its FIG keeps coming (they repeat about every second) and says so
        if (kv.second.first.asw != 0 && p_->ficFrames <= kv.second.second + 40) e.annActive.push_back(kv.second.first);
    return e;
}

DabFicAnalysis DabReceiver::ficAnalysis() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    DabFicAnalysis a;
    a.seq = p_->ficSeq;
    a.frames = p_->ficFrames;
    a.fibOk = p_->ficFibs; a.fibBad = p_->ficBadN;
    a.fillBytes = p_->ficFill; a.fibCount = p_->ficFibs;
    for (int i = 0; i < 12; i++) a.lastFill[i] = p_->lastFill[i];
    a.figs.reserve(p_->figs.size());
    for (const auto& kv : p_->figs) a.figs.push_back(kv.second);
    std::sort(a.figs.begin(), a.figs.end(), [](const DabFigRecord& x, const DabFigRecord& y) {
        if (x.type != y.type) return x.type < y.type;
        if (x.ext != y.ext) return x.ext < y.ext;
        if (x.cn != y.cn) return x.cn < y.cn;
        if (x.oe != y.oe) return x.oe < y.oe;
        if (x.pd != y.pd) return x.pd < y.pd;
        return x.data < y.data;
    });
    for (const auto& kv : p_->figStat) a.stats.push_back(kv.second);
    return a;
}

void DabReceiver::select(int subId) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->selPktAddr = -1; p_->selJlPad = false;
    p_->selReq = subId;
    if (subId < 0) p_->applySelection();
}
int DabReceiver::selected() const { return p_->selReq; }
DabAudio& DabReceiver::audio() { return p_->audio; }
DmbDecoder& DabReceiver::dmb() { return p_->dmb; }
JournalineDecoder& DabReceiver::journaline() { return p_->jl; }
DabSpiDecoder& DabReceiver::spi() { return p_->spiDec; }
void DabReceiver::selectSpi(int subId, int packetAddress) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (subId == p_->spiReq && packetAddress == p_->spiAddr) return;
    p_->spiReq = subId;
    p_->spiAddr = subId < 0 ? -1 : packetAddress;
    p_->spiDirty = true;
    p_->spiDemux.reset();
    if (subId < 0) p_->spiSel = -1;
}
DabReceiver::SpiLink DabReceiver::spiLink() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    const DabPacketDemux& d = p_->spiDemux;
    SpiLink l;
    l.packets = d.packets; l.bad = d.badPackets; l.lost = d.lostPackets; l.orphan = d.orphanPackets; l.other = d.otherIdPackets; l.empty = d.emptyPackets; l.unitsOk = d.unitsOk; l.unitsDropped = d.unitsDropped;
    return l;
}
int DabReceiver::spiSelected() const { return p_->spiReq; }
void DabReceiver::selectJournaline(int subId, int packetAddress, int extHeader) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->jl.reset();
    p_->jl.setExtHeader(extHeader);
    p_->selPktAddr = packetAddress;
    p_->selJlPad = packetAddress < 0;
    p_->selReq = subId;
    p_->sel = -1;   // restart the sub-channel decoding for the new use
}
void DabReceiver::setPacketCallback(std::function<void(const uint8_t*, size_t, double)> cb) { p_->pktCb = std::move(cb); }
void DabReceiver::setFrameTap(std::function<void(int, const uint8_t*, int)> cb) { p_->tap = std::move(cb); }

} // namespace dect2
