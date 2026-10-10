// DAB receiver, transmission mode I: null-symbol and phase-reference synchronisation, carrier and timing tracking, DQPSK
// demodulation, fast information channel (ensemble, services, labels) and the main service channel of the selected sub-channel.
#include "dect2/text_charset.h"
#include "dect2/dab.h"
#include "dect2/fftutil.h"
#include "dect2/dab_tii.h"
#include "dect2/resampler.h"
#include "dect2/channel_find.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <mutex>

namespace dect2 {

namespace {
using namespace dab;
using cd = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;
constexpr int kMaxShift = 80;      // integer carrier offset searched at the start: +-80 kHz covers 50 ppm at L band (1.49 GHz, 75 kHz)
constexpr int kBackoff = 16;        // demodulation windows start this many samples before the useful part (inside the guard interval)

// A 16-byte label in the charset FIG 1 names (EN 300 401 5.2.2.2.1; EBU Latin unless it says otherwise) to clean UTF-8
std::string labelText(int charset, const uint8_t* d, int n) {
    return text::clean(text::dabCharset(charset, d, (size_t)n));
}

uint32_t getBits(const uint8_t* d, int pos, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | ((d[(pos + i) >> 3] >> (7 - ((pos + i) & 7))) & 1);
    return v;
}
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
    dabtii::Detector tii;     // transmitter identification from the null symbols

    // ---- decoding state
    mutable std::mutex mu;
    DabEnsemble ens;
    DabTelemetry tel;
    uint64_t telSeq = 0;
    uint64_t fibOk = 0, fibBad = 0;
    int ficRecent = 0;
    double snrDb = 0;
    std::vector<cf32> constel;

    std::vector<int8_t> ficSoft, mscSoft;
    // selected sub-channel
    int selReq = -1, sel = -1;
    DabSubchannel selSub;
    std::vector<std::vector<int8_t>> hist;   // 16 CIFs of the sub-channel's soft bits
    uint64_t cifCount = 0;
    DabAudio audio;
    std::function<void(int, const uint8_t*, int)> tap;
    // a DMB video sub-channel (TS 102 427 / 102 428): its transport stream for the player
    bool selDmb = false;
    DmbDecoder dmb;
    std::function<void(const uint8_t*, size_t, double)> pktCb;

    Impl() : bin(kCarriers), prsConj(kTu) {
        const auto& tab = freqInterleaver();
        for (int n = 0; n < kCarriers; n++) bin[(size_t)n] = (tab[(size_t)n] + kTu) % kTu;
        const auto& x = phaseReference();
        for (int i = 0; i < kTu; i++) prsConj[(size_t)i] = std::conj(x[(size_t)i]);
        ficSoft.resize((size_t)kFicSymbols * 2 * kCarriers);
        mscSoft.resize((size_t)(kSymbols - 1 - kFicSymbols) * 2 * kCarriers);
        hist.assign(16, {});
        audio.select(-1, false, 0);
        dmb.setPacketSink([this](const uint8_t* pk, size_t n) { if (pktCb) pktCb(pk, n, 0.024); });
    }

    // ------------------------------------------------------------ helpers
    const cf32* at(uint64_t abs) const { return &buf[(size_t)(abs - base)]; }

    // FFT of a window of kTu samples starting at `abs`, derotated by `cfoBins` (carrier offset in bins)
    void windowFft(uint64_t abs, double cfoBins, std::vector<cf32>& out) {
        out.resize(kTu);
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

    void trim(uint64_t keepFrom) {
        if (keepFrom <= base) return;
        const size_t drop = (size_t)std::min<uint64_t>(keepFrom - base, buf.size());
        buf.erase(buf.begin(), buf.begin() + (long)drop);
        base += drop;
    }

    // ------------------------------------------------------------ search
    bool searchSync() {
        const size_t need = (size_t)kFrame + kTnull + 4096 + kTs;
        if (buf.size() < need) return false;
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
        if (best / (double)win > 0.6 * mean) { trim(base + buf.size() - (size_t)kFrame); return false; }   // no null: not a DAB frame (yet)
        // the PRS useful part starts about 1832 samples after the centre of the quietest window; search around it
        const long nominal = (long)bestPos + win / 2 + 1832;
        float bestPk = 0; long bestP = 0; int bestShift = 0, bestIdx = 0;
        std::vector<cf32> R, tmp;
        for (long p = nominal - 1400; p <= nominal + 1000; p += 128) {
            if (p < 0 || p + kTu > (long)buf.size()) continue;
            windowFft(base + (uint64_t)p, 0.0, R);
            for (int sh = -kMaxShift; sh <= kMaxShift; sh++) {
                int idx;
                const float pk = cirOf(R, sh, idx, tmp);
                if (pk > bestPk) { bestPk = pk; bestP = p; bestShift = sh; bestIdx = idx; }
            }
        }
        if (bestPk < 25.f) {
            // no phase reference: the next attempt looks at the mirrored spectrum (I and Q swapped by the radio or the file format)
            mirror = !mirror;
            for (auto& v : buf) v = std::conj(v);
            trim(base + buf.size() - (size_t)kFrame);
            return false;
        }
        const uint64_t U = base + (uint64_t)(bestP + bestIdx);
        cfo = bestShift;
        nextU = U;
        fresh = true;
        missed = 0;
        state = 2;
        tii.reset();   // a new lock: which frames carry TII may have changed sides
        cirPeak = bestPk;
        syncSample = U;
        trim(U > 2 * kTs ? U - 2 * kTs : 0);
        return true;
    }

    // ------------------------------------------------------------ one frame
    bool processFrame() {
        const uint64_t needEnd = nextU + (uint64_t)kSymbols * kTs + kTu + 256;
        if (base + buf.size() < needEnd) return false;
        if (nextU < base + 128) { state = 0; return true; }
        uint64_t U = nextU;
        std::vector<cf32> R, tmp;
        const bool wasFresh = fresh;
        if (!fresh) {   // refine the timing with the phase reference correlation (the window sits inside the guard interval)
            const uint64_t p = U - 64;
            windowFft(p, cfo, R);
            int idx;
            std::vector<float> mag;
            cirPeak = cirOf(R, 0, idx, tmp, &mag);
            if (cirPeak >= 12.f) { U = p + (uint64_t)(int64_t)idx; missed = 0; cirKeep = mag; }
            else { missed++; if (missed >= 4) { state = 0; publish(); return true; } }
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
                const uint64_t u = U + (uint64_t)l * kTs;
                const cf32* a = at(u - kTg);
                const cf32* b = at(u + kTu - kTg);
                for (int i = 0; i < kTg; i++) acc += cd(a[i].real(), -a[i].imag()) * cd(b[i].real(), b[i].imag());
            }
            const double e = std::arg(acc) / (2.0 * kPi);
            const double d = e - cfo;
            double nc = cfo + d - std::round(d);
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
            std::vector<cf32> N;
            windowFft(U - (uint64_t)kTg - (uint64_t)kTnull + (uint64_t)(kTnull - kTu) / 2, cfo, N);
            tii.addNull(N, framesDone);
        }
        // demodulate all symbols
        std::vector<cf32> prev, cur;
        windowFft(U - kBackoff, cfo, prev);
        const float inv = 1.f;
        (void)inv;
        // pass 1: differential cells; scale by the mean magnitude
        std::vector<cf32> q((size_t)(kSymbols - 1) * kCarriers);
        double magSum = 0;
        for (int l = 1; l < kSymbols; l++) {
            windowFft(U + (uint64_t)l * kTs - kBackoff, cfo, cur);
            cf32* o = &q[(size_t)(l - 1) * kCarriers];
            for (int n = 0; n < kCarriers; n++) {
                const int k = bin[(size_t)n];
                o[n] = cur[(size_t)k] * std::conj(prev[(size_t)k]);
                magSum += std::abs(o[n]);
            }
            prev.swap(cur);
        }
        const double meanMag = magSum / ((double)(kSymbols - 1) * kCarriers) + 1e-12;
        const float sc = (float)(40.0 / meanMag);
        auto soft = [&](int l, int8_t* dst) {   // l = 1..75: first half real parts, second half imaginary parts (positive = bit 1)
            const cf32* o = &q[(size_t)(l - 1) * kCarriers];
            for (int n = 0; n < kCarriers; n++) {
                dst[n] = (int8_t)std::max(-127.f, std::min(127.f, std::round(-o[n].real() * sc)));
                dst[kCarriers + n] = (int8_t)std::max(-127.f, std::min(127.f, std::round(-o[n].imag() * sc)));
            }
        };
        for (int l = 1; l <= kFicSymbols; l++) soft(l, &ficSoft[(size_t)(l - 1) * 2 * kCarriers]);
        for (int l = kFicSymbols + 1; l < kSymbols; l++) soft(l, &mscSoft[(size_t)(l - kFicSymbols - 1) * 2 * kCarriers]);
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
        decodeMsc();
        framesDone++;
        endSample = U;
        nextU = U + kFrame;
        trim(nextU > 2 * kTs ? nextU - 2 * kTs : 0);
        publish();
        return true;
    }

    // ------------------------------------------------------------ FIC
    void decodeFic() {
        int ok = 0;
        std::lock_guard<std::mutex> lk(mu);
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
                if (crc16(fib, 30) == (uint16_t)((fib[30] << 8) | fib[31])) { ok++; fibOk++; parseFib(fib); }
                else fibBad++;
            }
        }
        ficRecent = ok;
        ens.valid = ens.eid != 0 && !ens.services.empty();
    }

    void parseFib(const uint8_t* fib) {
        int i = 0;
        while (i < 30 && fib[i] != 0xFF) {
            const int type = fib[i] >> 5, len = fib[i] & 31;
            if (i + 1 + len > 30) break;
            const uint8_t* d = fib + i + 1;
            i += 1 + len;
            if (len < 1) continue;
            if (type == 0) parseFig0(d, len);
            else if (type == 1) parseFig1(d, len);
        }
    }

    void parseFig0(const uint8_t* d, int len) {
        const int ext = d[0] & 31, pd = (d[0] >> 5) & 1;
        const uint8_t* b = d + 1;
        const int n = len - 1;
        // C/N = 1 in the sub-channel and service organisation: the configuration after an announced reconfiguration. Only the current one is
        // followed (the new one arrives with C/N = 0 once it is in force).
        if ((d[0] & 0x80) && (ext == 1 || ext == 2)) return;
        if (ext == 0 && n >= 2) ens.eid = (uint16_t)((b[0] << 8) | b[1]);
        else if (ext == 1) {
            int p = 0;
            while (p + 3 <= n) {
                DabSubchannel s;
                s.id = b[p] >> 2;
                s.start = ((b[p] & 3) << 8) | b[p + 1];
                if (b[p + 2] & 0x80) {
                    if (p + 4 > n) break;
                    s.eep = true;
                    s.option = (b[p + 2] >> 4) & 7;
                    s.level = (b[p + 2] >> 2) & 3;
                    s.size = ((b[p + 2] & 3) << 8) | b[p + 3];
                    int nn, br, info;
                    s.bitrate = eepGeometry(s.size, s.option, s.level, nn, br, info) ? br : 0;
                    p += 4;
                } else {
                    s.eep = false;
                    s.uepIndex = b[p + 2] & 0x3F;
                    int lv, info;
                    if (!(b[p + 2] & 0x40) && uepGeometry(s.uepIndex, s.size, s.bitrate, lv, info)) s.level = lv - 1;
                    else { s.size = 0; s.bitrate = 0; }   // table switch 1 is not defined
                    p += 3;
                }
                ens.subs[s.id] = s;
            }
        } else if (ext == 2) {
            int p = 0;
            while (p + 3 <= n) {
                uint32_t sid;
                if (pd == 0) { sid = (uint32_t)((b[p] << 8) | b[p + 1]); p += 2; }
                else { if (p + 5 > n) break; sid = ((uint32_t)b[p] << 24) | ((uint32_t)b[p + 1] << 16) | ((uint32_t)b[p + 2] << 8) | b[p + 3]; p += 4; }
                const int nc = b[p] & 15;
                p++;
                DabService& sv = ens.services[sid];
                sv.sid = sid;
                std::vector<DabComponent> comps;
                for (int c = 0; c < nc && p + 2 <= n; c++, p += 2) {
                    DabComponent dc;
                    dc.tmid = b[p] >> 6;
                    if (dc.tmid == 0) { dc.ascty = b[p] & 63; dc.subId = b[p + 1] >> 2; dc.primary = (b[p + 1] >> 1) & 1; }
                    else if (dc.tmid == 1) { dc.dscty = b[p] & 63; dc.subId = b[p + 1] >> 2; dc.primary = (b[p + 1] >> 1) & 1; }
                    comps.push_back(dc);
                }
                sv.comps = comps;
            }
        } else if (ext == 13) {   // user applications (TS 101 756 table 16): which sub-channel carries DMB video
            int p = 0;
            while (p + (pd ? 5 : 3) <= n) {
                uint32_t sid;
                if (pd == 0) { sid = (uint32_t)((b[p] << 8) | b[p + 1]); p += 2; }
                else { sid = ((uint32_t)b[p] << 24) | ((uint32_t)b[p + 1] << 16) | ((uint32_t)b[p + 2] << 8) | b[p + 3]; p += 4; }
                const int scids = b[p] >> 4, nua = b[p] & 15;
                p++;
                for (int u = 0; u < nua && p + 2 <= n; u++) {
                    const int type = (b[p] << 3) | (b[p + 1] >> 5), len = b[p + 1] & 31;
                    p += 2;
                    if (p + len > n) break;
                    if (scids == 0 && u == 0) {   // the primary component's first application (PAD applications of a DMB radio service follow it)
                        DabService& sv = ens.services[sid];
                        sv.sid = sid;
                        sv.userApp = type;
                        if (type == 0x009 && len >= 1) sv.dmbProfile = b[p];
                    }
                    p += len;
                }
            }
        } else if (ext == 10 && n >= 4) {
            const uint32_t mjd = getBits(b, 1, 17);
            const int flag = (int)getBits(b, 20, 1);
            const int hh = (int)getBits(b, 21, 5), mm = (int)getBits(b, 26, 6);
            int ss = 0;
            if (flag && n >= 6) ss = (int)getBits(b, 32, 6);
            if (mjd > 40587) ens.utc = (int64_t)(mjd - 40587) * 86400 + hh * 3600 + mm * 60 + ss;
        }
    }

    void parseFig1(const uint8_t* d, int len) {
        const int ext = d[0] & 7, cs = d[0] >> 4;
        const uint8_t* b = d + 1;
        const int n = len - 1;
        if (ext == 0 && n >= 18) ens.label = labelText(cs, b + 2, 16);
        else if (ext == 1 && n >= 18) {
            const uint32_t sid = (uint32_t)((b[0] << 8) | b[1]);
            DabService& sv = ens.services[sid];
            sv.sid = sid;
            sv.label = labelText(cs, b + 2, 16);
        } else if (ext == 5 && n >= 20) {   // data service label (32-bit SId), e.g. DMB television
            const uint32_t sid = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
            DabService& sv = ens.services[sid];
            sv.sid = sid;
            sv.label = labelText(cs, b + 4, 16);
        }
    }

    // ------------------------------------------------------------ MSC
    void applySelection() {
        // (ens is locked by the caller)
        if (selReq == sel) return;
        if (selReq < 0) { sel = -1; selDmb = false; audio.select(-1, false, 0); dmb.reset(); hist.assign(16, {}); cifCount = 0; return; }
        auto it = ens.subs.find(selReq);
        if (it == ens.subs.end()) return;   // wait until the ensemble tells us about it
        selSub = it->second;
        bool plus = true, isDmb = false;
        for (const auto& kv : ens.services) {
            for (const auto& c : kv.second.comps) if (c.tmid == 0 && c.subId == selReq) plus = c.ascty == 63;
            const DabComponent* d = kv.second.dmb();
            if (d && d->subId == selReq) isDmb = true;
        }
        sel = selReq;
        selDmb = isDmb;
        hist.assign(16, {});
        cifCount = 0;
        dmb.reset();
        if (isDmb) audio.select(-1, false, 0);
        else audio.select(sel, plus, selSub.bitrate);
    }

    void decodeMsc() {
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
                        hist.assign(16, {});
                        cifCount = 0;
                    }
                }
            }
        }
        if (sel < 0) return;
        if (selSub.size <= 0 || selSub.start + selSub.size > kCifCu) return;
        int n, br, info, sz, lv;
        if (selSub.eep ? !eepGeometry(selSub.size, selSub.option, selSub.level, n, br, info) : !uepGeometry(selSub.uepIndex, sz, br, lv, info)) return;
        const int bits = selSub.size * kCuBits;
        std::vector<int8_t> mother((size_t)(4 * (info + 6))), one((size_t)bits);
        std::vector<uint8_t> dec((size_t)info), bytes((size_t)info / 8);
        static const int kMap[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
        for (int c = 0; c < kCifPerFrame; c++) {
            const int8_t* src = &mscSoft[(size_t)c * kCifBits + (size_t)selSub.start * kCuBits];
            hist[cifCount % 16].assign(src, src + bits);
            cifCount++;
            if (cifCount < 16) continue;
            for (int i = 0; i < bits; i++) {
                const uint64_t t = cifCount - 1 - (uint64_t)(15 - kMap[i & 15]);
                one[(size_t)i] = hist[t % 16][(size_t)i];
            }
            if (selSub.eep ? !eepDepuncture(one.data(), selSub.size, selSub.option, selSub.level, mother.data()) : !uepDepuncture(one.data(), selSub.uepIndex, mother.data())) return;
            viterbiDecode(mother.data(), info, dec.data());
            descramble(dec.data(), info);
            for (int i = 0; i < info / 8; i++) { uint8_t v = 0; for (int b = 0; b < 8; b++) v = (uint8_t)((v << 1) | dec[(size_t)i * 8 + (size_t)b]); bytes[(size_t)i] = v; }
            if (tap) tap(sel, bytes.data(), (int)bytes.size());
            if (selDmb) dmb.push(bytes.data(), (int)bytes.size());
            else audio.push(bytes.data(), (int)bytes.size());
        }
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
        t.seq = ++telSeq;
        tel = std::move(t);
    }

    void reset() {
        buf.clear(); base = 0; state = 0; mirror = false; cfo = 0; cc.reset(); missed = 0; framesDone = 0; rs.reset(); tii.reset();
        std::lock_guard<std::mutex> lk(mu);
        telSeq++;
        tel = DabTelemetry(); tel.seq = telSeq;
        hist.assign(16, {}); cifCount = 0;
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
    if (I.resample) I.rs.configure(inputRateHz, kRate);
    I.cc.configure(inputRateHz, 1.536e6, 60e3, 1.5);
    I.reset();
}

void DabReceiver::reset() { p_->reset(); }

void DabReceiver::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    // a channel away from the middle of the sample band (beyond the +-80 kHz of the search) is moved there first
    I.cin.assign(x, x + n);
    I.cc.process(I.cin.data(), n, I.state == 2 && I.fibOk > 0);
    if (I.cc.takeChanged()) { I.buf.clear(); I.base = 0; I.state = 0; I.rs.reset(); }
    x = I.cin.data();
    const size_t from = I.buf.size();
    if (I.resample) { I.rsOut.clear(); I.rs.process(x, n, I.rsOut); I.buf.insert(I.buf.end(), I.rsOut.begin(), I.rsOut.end()); }
    else I.buf.insert(I.buf.end(), x, x + n);
    if (I.mirror) for (size_t i = from; i < I.buf.size(); i++) I.buf[i] = std::conj(I.buf[i]);
    I.run();
}

bool DabReceiver::telemetry(DabTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    out.audio = p_->audio.stats();
    return true;
}

DabEnsemble DabReceiver::ensemble() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->ens;
}

void DabReceiver::select(int subId) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->selReq = subId;
    if (subId < 0) p_->applySelection();
}
int DabReceiver::selected() const { return p_->selReq; }
DabAudio& DabReceiver::audio() { return p_->audio; }
DmbDecoder& DabReceiver::dmb() { return p_->dmb; }
void DabReceiver::setPacketCallback(std::function<void(const uint8_t*, size_t, double)> cb) { p_->pktCb = std::move(cb); }
void DabReceiver::setFrameTap(std::function<void(int, const uint8_t*, int)> cb) { p_->tap = std::move(cb); }

} // namespace dect2
