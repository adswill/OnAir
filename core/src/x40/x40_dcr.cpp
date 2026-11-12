// DCR, Japan's digital convenience radio, 4-level FSK (ARIB STD-T98 Volume 3; docs/DCR-4.0.md). Receive only.
//
// The physical layer is NXDN48's: 2400 symbols/s, 6.25 kHz, the sync word 0xCDF59, a 384-bit frame of 80 ms, PN9 whitening and the same K = 5
// convolutional code, puncturing, interleaving and CRCs (CRC-6 for the SACCH, CRC-12 for the PICH and the error-corrected data TCH), so the NXDN Rust layer
// (nxdn_proto.h) decodes the channels. What differs is above that: the 8-symbol radio information channel (RICH: F, M, D and a parity bit instead of the NXDN LICH),
// the SACCH message (9-bit user code, 7-bit manufacturer number, 2-bit call type, one unit per frame, up to four units), the synchronous burst with the
// 9-digit BCD call sign (PICH), 144-bit traffic channels TCH1 / TCH2 carrying two AMBE+2 frames each, the 15-bit privacy scrambler and a deviation of
// +-315 / +-945 Hz instead of +-350 / +-1050 Hz. Frame: SW 10 symbols, RICH 8, SACCH 30, TCH1 72, TCH2 72.
// The channel is filtered, FM-demodulated, resampled to 48 kHz, low-passed; the sync word is found by correlation on every sample (its sign gives the polarity,
// its least-squares fit the level and the carrier offset), every next sync a frame later within a few samples. When the span holds two or more channels of the
// plan (dcr_plan.h) and the tuning is known, every channel is watched for energy and a decoder is started on each active one (scan).
#include "x40_dsp.h"
#include "dect2/nxdn/nxdn_proto.h"
#include "dect2/x40/dcr_plan.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <memory>

namespace dect2::x40 {

namespace {

constexpr double kFs = 48000;
constexpr int kYMask = 32767;
constexpr double kUnitHz = 315.0;          // one level step of the 4-level FSK: +-315 Hz and +-945 Hz
constexpr int kFrameSym = nxdn::kFrameSym, kFswSym = nxdn::kFswSym, kPayloadSym = nxdn::kPayloadSym;

float pearson(const float* v, const int* s, int n) {
    double mv = 0, ms = 0;
    for (int i = 0; i < n; i++) { mv += v[i]; ms += s[i]; }
    mv /= n; ms /= n;
    double c = 0, a = 0, b = 0;
    for (int i = 0; i < n; i++) { const double x = v[i] - mv, y = s[i] - ms; c += x * y; a += x * x; b += y * y; }
    return a > 0 && b > 0 ? (float)(c / std::sqrt(a * b)) : 0.f;
}
void lsFit(const float* v, const int* s, int n, double& g, double& d) {   // v = g * s + d
    double ms = 0, mv = 0;
    for (int i = 0; i < n; i++) { ms += s[i]; mv += v[i]; }
    ms /= n; mv /= n;
    double c = 0, a = 0;
    for (int i = 0; i < n; i++) { c += (s[i] - ms) * (v[i] - mv); a += (s[i] - ms) * (s[i] - ms); }
    g = a > 0 ? c / a : 0; d = mv - g * ms;
}
int nearestLevel(double v) { return v >= 2 ? 3 : v >= 0 ? 1 : v >= -2 ? -1 : -3; }

std::string hexOf(const uint8_t* bits, int n) {
    std::string s;
    char b[4];
    for (int i = 0; i < n; i += 8) {
        int v = 0;
        for (int k = 0; k < 8; k++) v = (v << 1) | (i + k < n ? bits[i + k] : 0);
        snprintf(b, sizeof b, "%02X", v);
        s += b;
    }
    return s;
}

// Everything that belongs to one channel: mixer, decimator, discriminator, the symbol chain and the frame decoder. Rows go to the sink with the channel's label.
class DcrChan {
public:
    struct Stats {
        uint64_t frames = 0, sacchOk = 0, sacchBad = 0, richBad = 0, pichOk = 0, dataOk = 0, dataBad = 0, voiceFrames = 0, calls = 0, syncs = 0, bursts = 0;
        double berPct = 0, snrDb = 0, cfoHz = 0, devHz = 0;
        bool inverted = false;
    };

    DcrChan(Sink* sink, const std::string& tag) : out_(sink), tag_(tag) {}

    bool configure(double fs, double offsetHz) {
        fs_ = fs; off_ = offsetHz; ph_ = 0;
        d_ = std::max(1, (int)std::floor(fs / 96000.0));
        if (!dec_.configure(fs, d_, 5000.0)) return false;
        fd_ = dec_.outRate();
        step_ = fd_ / kFs;
        resetDsp();
        return true;
    }
    void resetDsp() {
        dec_.reset(); prevZ_ = cf32(0, 0); haveF_ = false; fPrev_ = 0; inIdx_ = 0; pos_ = 1.0;
        chain_.init();
        lvl_ = 0;
    }
    void feed(const cf32* x, size_t n) {
        mix_.resize(n);
        if (off_ != 0) {
            double p = ph_; const double dp = -2 * kPi * off_ / fs_;
            for (size_t i = 0; i < n; i++) { mix_[i] = x[i] * cf32((float)std::cos(p), (float)std::sin(p)); p += dp; if (p > kPi) p -= 2 * kPi; else if (p < -kPi) p += 2 * kPi; }
            ph_ = p;
        } else std::copy(x, x + n, mix_.begin());
        dec_.process(mix_.data(), n, [&](cf32 a) { demod(a); });
        poll();
    }
    // end a call whose frames stopped
    void poll() {
        if (call_.active && out_->now() - call_.lastSec > 1.5) endCall(false);
    }
    void reset() { resetDsp(); lost(); }
    bool decoding() const { return out_->now() - lastGood_ < 6.0; }
    double lastGood() const { return lastGood_; }
    const Stats& stats() const { return st_; }
    const std::string& tag() const { return tag_; }
    float levelDb() const { return lvl_ > 0 ? toDb((float)lvl_) : -120.f; }

    // what the last call and the last call sign were, for the headline
    struct Call {
        bool active = false;
        int uc = 0, mfr = 0, comm = 0;
        double startSec = 0, lastSec = 0;
        uint32_t frames = 0, voice = 0;
        double errBits = 0, allBits = 0;
        std::string csm;
    };
    const Call& call() const { return call_; }
    int lastUc() const { return lastUc_; }
    const std::string& lastCsm() const { return lastCsm_; }

private:
    // ---------------------------------------------------------------- demodulator
    void demod(cf32 a) {
        lvl_ = 0.999 * lvl_ + 0.001 * std::norm(a);
        const cf32 dz = a * std::conj(prevZ_);
        prevZ_ = a;
        const float f = (float)(std::atan2(dz.imag(), dz.real()) * fd_ / (2 * kPi));
        if (!haveF_) { haveF_ = true; fPrev_ = f; inIdx_ = 0; return; }
        inIdx_++;
        while (pos_ <= (double)inIdx_) {
            const double frac = pos_ - (double)(inIdx_ - 1);
            chain_.push((float)(fPrev_ + (f - fPrev_) * frac));
            pos_ += step_;
            chainStep();
        }
        fPrev_ = f;
    }

    struct Chain {
        static constexpr int sps = 20;
        int sD = 2 * sps;
        std::vector<float> y = std::vector<float>(kYMask + 1, 0.f);
        int64_t yN = 0;
        enum { Search, Track } mode = Search;
        int64_t candN = -1; float candR = 0;
        int64_t fs = 0;          // the sample of the last sync symbol of the current frame
        bool decoded = false;
        double g = 1, d = 0;     // level (signed: polarity) and offset, Hz
        int miss = 0, bad = 0;
        bool fly = false;
        std::vector<float> sh, shist;
        int shPos = 0;
        void init() {
            const int n = 2 * sD + 1;
            const double lpf = 0.625 * 2400;
            sh.assign((size_t)n, 0.f);
            double sum = 0;
            for (int k = 0; k < n; k++) {
                const double t = k - sD, x = 2 * lpf / kFs * t;
                const double sc = t == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
                const double w = 0.42 + 0.5 * std::cos(kPi * t / (sD + 1)) + 0.08 * std::cos(2 * kPi * t / (sD + 1));
                sh[(size_t)k] = (float)(sc * w); sum += sh[(size_t)k];
            }
            for (float& v : sh) v = (float)(v / sum);
            shist.assign(2 * (size_t)n, 0.f); shPos = 0;
            std::fill(y.begin(), y.end(), 0.f); yN = 0;
            mode = Search; candN = -1; candR = 0; miss = 0; bad = 0; decoded = false; fly = false;
        }
        void push(float f) {
            const int n = (int)sh.size();
            shist[(size_t)shPos] = shist[(size_t)(shPos + n)] = f;
            shPos = (shPos + 1) % n;
            const float* x = shist.data() + shPos;
            float acc = 0;
            for (int k = 0; k < n; k++) acc += x[k] * sh[(size_t)(n - 1 - k)];
            y[(size_t)(yN & kYMask)] = acc;
            yN++;
        }
        float at(int64_t i) const { return y[(size_t)((i + sD) & kYMask)]; }
        void fswAt(int64_t e, float* v) const { for (int k = 0; k < kFswSym; k++) v[k] = at(e - (int64_t)(kFswSym - 1 - k) * sps); }
    };

    bool syncOk(const float* v, double g, double d) const {
        if (std::fabs(g) < 0.4 * kUnitHz || std::fabs(g) > 1.8 * kUnitHz) return false;
        for (int k = 0; k < kFswSym; k++) if (nearestLevel((v[k] - d) / g) != nxdn::kFsw[k]) return false;
        return true;
    }

    void chainStep() {
        Chain& c = chain_;
        const int64_t last = c.yN - 1 - 2 * c.sD;
        const int64_t need = (int64_t)kFswSym * Chain::sps;
        if (last < need) return;
        float v[kFswSym];
        if (c.mode == Chain::Search) {
            c.fswAt(last, v);
            const float r = pearson(v, nxdn::kFsw, kFswSym);
            if (std::fabs(r) > 0.85f && std::fabs(r) > c.candR) {
                double g, d;
                lsFit(v, nxdn::kFsw, kFswSym, g, d);
                if (syncOk(v, g, d)) { c.candN = last; c.candR = std::fabs(r); }
            }
            if (c.candN >= 0 && last - c.candN > Chain::sps / 2) {
                c.fs = c.candN; c.candN = -1; c.candR = 0;
                c.fswAt(c.fs, v);
                lsFit(v, nxdn::kFsw, kFswSym, c.g, c.d);
                c.mode = Chain::Track; c.miss = 0; c.decoded = false; c.fly = false; c.bad = 0;
                st_.syncs++;
                out_->energy();
            }
            return;
        }
        if (!c.decoded && last >= c.fs + (int64_t)kPayloadSym * Chain::sps) {
            c.decoded = true;
            c.bad = decodeFrame() ? 0 : c.bad + 1;
            if (c.bad >= 3) { c.mode = Chain::Search; lost(); return; }
        }
        const int W = std::max(2, Chain::sps / 4);
        const int64_t nominal = c.fs + (int64_t)kFrameSym * Chain::sps;
        if (last < nominal + W) return;
        float best = -2; int64_t bn = nominal; double bg = c.g, bd = c.d;
        for (int64_t e = nominal - W; e <= nominal + W; e++) {
            c.fswAt(e, v);
            const float r = pearson(v, nxdn::kFsw, kFswSym) * (c.g > 0 ? 1.f : -1.f);
            if (r > best) { best = r; bn = e; }
        }
        c.fswAt(bn, v);
        lsFit(v, nxdn::kFsw, kFswSym, bg, bd);
        const double lr = std::fabs(bg / c.g);
        if (best > 0.6f && bg * c.g > 0 && (syncOk(v, bg, bd) || (best > 0.8f && lr > 0.6 && lr < 1.6))) { c.fs = bn; c.g = 0.5 * c.g + 0.5 * bg; c.d = 0.5 * c.d + 0.5 * bd; c.miss = 0; c.fly = false; }
        else if (++c.miss <= 2) { c.fs = nominal; c.fly = true; }
        else { c.mode = Chain::Search; lost(); return; }
        c.decoded = false;
    }

    // ---------------------------------------------------------------- frames
    void post(const char* kind, const std::string& id, const std::string& text) {
        out_->row(kind, tag_.empty() ? id : tag_ + " " + id, text, levelDb());
    }

    bool decodeFrame() {
        Chain& c = chain_;
        float v[kFrameSym];
        int lv[kFrameSym];
        for (int k = 0; k < kFswSym; k++) { v[k] = c.at(c.fs - (int64_t)(kFswSym - 1 - k) * Chain::sps); lv[k] = nxdn::kFsw[k]; }
        for (int k = 0; k < kPayloadSym; k++) v[kFswSym + k] = c.at(c.fs + (int64_t)(k + 1) * Chain::sps);
        for (int k = kFswSym; k < kFrameSym; k++) lv[k] = nearestLevel((v[k] - c.d) / c.g);
        double g, d;
        lsFit(v, lv, kFrameSym, g, d);
        if (g * c.g > 0 && std::fabs(g) > 0.5 * std::fabs(c.g) && std::fabs(g) < 2 * std::fabs(c.g)) { c.g = g; c.d = d; }
        double mse = 0;
        float s[kPayloadSym];
        uint8_t pn[kPayloadSym];
        nxdn::pn9(pn, kPayloadSym);
        for (int k = 0; k < kPayloadSym; k++) {
            const double x = (v[kFswSym + k] - c.d) / c.g;
            const double e = x - nearestLevel(x);
            mse += e * e;
            s[k] = (float)(pn[k] ? -x : x);
        }
        mse /= kPayloadSym;
        const float snr = (float)(10 * std::log10(5.0 / std::max(1e-4, mse)));
        st_.frames++;

        // RICH: F, two reserved bits, M (3), D, even parity; a 0 is +3 / +1, a 1 is -1 / -3
        uint8_t lb = 0;
        for (int k = 0; k < 8; k++) lb = (uint8_t)((lb << 1) | (s[k] < 0 ? 1 : 0));
        auto parityOk = [](uint8_t b) { int n = 0; for (int i = 0; i < 8; i++) n += (b >> i) & 1; return (n & 1) == 0; };
        bool richOk = parityOk(lb);
        if (!richOk) {   // one parity bit: the least reliable symbol is the one most likely wrong
            int wk = 0;
            for (int k = 1; k < 8; k++) if (std::fabs(s[k]) < std::fabs(s[wk])) wk = k;
            if (std::fabs(s[wk]) < 1.f) { lb ^= (uint8_t)(0x80 >> wk); richOk = parityOk(lb); }
        }
        if (richOk) { rich_ = lb; haveRich_ = true; }
        else { st_.richBad++; if (!haveRich_) return false; }
        const int F = (rich_ >> 7) & 1, M = (rich_ >> 2) & 7;

        float sb[348];   // soft bits of the 174 channel symbols: > 0 a 1
        for (int k = 0; k < 174; k++) {
            const float x = s[8 + k];
            sb[2 * k] = std::max(-3.f, std::min(3.f, -x));
            sb[2 * k + 1] = std::max(-1.f, std::min(1.f, std::fabs(x) - 2.f)) * 2.f;
        }
        bool any = false, sacchOk = false;
        uint8_t info[26]; int e = 0;
        if (nxdn::decode(nxdn::Ch::Sacch, sb, info, &e)) {
            st_.sacchOk++; any = sacchOk = true;
            addBer(e, 60);
            sacchUnit(info);
        } else st_.sacchBad++;

        if (F == 0) {
            // synchronous burst: PICH holds the 36-bit BCD call sign (the 144 bits after it are undefined)
            uint8_t pich[80]; int pe = 0;
            if (nxdn::decode(nxdn::Ch::Facch1, sb + 60, pich, &pe)) {
                st_.pichOk++; any = true; addBer(pe, 144);
                callSign(pich);
            }
            st_.bursts++;
        } else if (M != 5 && M != 6 && M != 7) {
            for (int h = 0; h < 2; h++) {
                const float* hb = sb + 60 + 144 * h;
                // M: 0 both non-audio 1 (no FEC), 1 TCH1 data + TCH2 audio, 2 TCH1 audio + TCH2 data, 3 both audio, 4 both data (CRC-12), 5 idle
                const bool audio = M == 3 || (M == 1 && h == 1) || (M == 2 && h == 0);
                const bool coded = M == 4 || (M == 1 && h == 0) || (M == 2 && h == 1);
                if (audio) { if (sacchOk) { st_.voiceFrames += 2; if (call_.active) call_.voice += 2; } }
                else if (coded) {
                    uint8_t di[80]; int de = 0;
                    if (nxdn::decode(nxdn::Ch::Facch1, hb, di, &de)) { st_.dataOk++; any = true; addBer(de, 144); dataBlock(di, h); }
                    else st_.dataBad++;
                }
            }
        }
        if (any) {
            lastGood_ = out_->now();
            st_.snrDb = st_.snrDb == 0 ? snr : 0.8 * st_.snrDb + 0.2 * snr;
            st_.cfoHz = c.d;
            st_.devHz = 3 * std::fabs(c.g);
            st_.inverted = c.g < 0;
            out_->good((float)st_.snrDb, c.d);
        } else out_->bad();
        if (call_.active) call_.lastSec = lastGood_ > call_.lastSec ? lastGood_ : call_.lastSec;
        return any;
    }

    void addBer(int e, int n) {
        errBits_ += e; allBits_ += n;
        st_.berPct = allBits_ > 0 ? 100.0 * errBits_ / allBits_ : 0;
        if (call_.active) { call_.errBits += e; call_.allBits += n; }
    }

    // one SACCH unit: M = F (leading unit), Wr (units still to come), message type (5 bits); then 18 bits
    void sacchUnit(const uint8_t* info) {
        const int lead = info[0], wr = (info[1] << 1) | info[2];
        int type = 0; for (int i = 3; i < 8; i++) type = (type << 1) | info[i];
        const uint8_t* pl = info + 8;
        if (lead) {
            asmType_ = type; asmGot_ = 1; asmRem_ = wr;
            std::memset(asmBits_, 0, sizeof asmBits_);
            std::memcpy(asmBits_, pl, 18);
            asmOn_ = true;
        } else if (asmOn_ && type == asmType_ && wr == asmRem_ - 1 && asmGot_ < 4) {
            std::memcpy(asmBits_ + 18 * asmGot_, pl, 18);
            asmGot_++; asmRem_ = wr;
        } else { asmOn_ = false; return; }
        if (asmRem_ != 0) return;
        asmOn_ = false;
        int comm = (asmBits_[0] << 1) | asmBits_[1], uc = 0, mfr = 0;
        for (int i = 0; i < 9; i++) uc = (uc << 1) | asmBits_[2 + i];
        for (int i = 0; i < 7; i++) mfr = (mfr << 1) | asmBits_[11 + i];
        message(asmType_, comm, uc, mfr, asmBits_ + 18, 18 * (asmGot_ - 1));
    }

    static const char* commName(int c) { return c == 0 ? "normal" : c == 1 ? "privacy scrambled" : "reserved"; }

    // The CRC-6 of the SACCH passes noise once in 64: a message counts when the same one comes twice in a row (a voice call repeats it in every frame)
    bool confirm(const std::string& key) {
        const double t = out_->now();
        const bool same = key == confKey_ && t - confSec_ < 1.0;
        confKey_ = key; confSec_ = t;
        return same;
    }

    void message(int type, int comm, int uc, int mfr, const uint8_t* extra, int nExtra) {
        char b[200];
        const std::string who = std::to_string(uc) + ":" + std::to_string(mfr) + ":" + std::to_string(comm);
        if (type == 1) {                                // voice communication
            if (call_.active && call_.uc == uc && call_.mfr == mfr && call_.comm == comm) { call_.frames++; call_.lastSec = out_->now(); confKey_ = "v" + who; confSec_ = out_->now(); return; }
            if (!confirm("v" + who)) return;
            touchCall(comm, uc, mfr);
        } else if (type == 0) {                         // idle
            if (!confirm("i" + who) || call_.active) return;
            snprintf(b, sizeof b, "idle, user code %d", uc);
            if (out_->now() - lastIdleSec_ > 10) { post("Idle", "UC " + std::to_string(uc), b); lastIdleSec_ = out_->now(); }
        } else if (type == 30) {                        // end of communication: only of the call that is on (one frame, it cannot be confirmed)
            if (call_.active && call_.uc == uc && call_.mfr == mfr) endCall(true);
        } else if (type >= 2 && type <= 19) {           // manufacturer defined format 1 - 18
            const std::string hx = hexOf(extra, nExtra);
            const std::string key = std::to_string(type) + ":" + who + ":" + hx;
            if (!confirm("m" + key)) return;
            if (key == lastMfrKey_ && out_->now() - lastMfrSec_ < 5) { lastMfrSec_ = out_->now(); return; }
            lastMfrKey_ = key; lastMfrSec_ = out_->now();
            snprintf(b, sizeof b, "manufacturer format %d, manufacturer %d, user code %d, %d extra bits%s%s", type - 1, mfr, uc, nExtra, nExtra ? ": " : "", hx.c_str());
            post("Manufacturer message", "UC " + std::to_string(uc), b);
        } else {
            if (!confirm("r" + std::to_string(type) + ":" + who)) return;
            snprintf(b, sizeof b, "reserved message type %d, user code %d", type, uc);
            post("Unknown message", "UC " + std::to_string(uc), b);
        }
    }

    void touchCall(int comm, int uc, int mfr) {
        lastUc_ = uc;
        if (call_.active && call_.uc == uc && call_.mfr == mfr && call_.comm == comm) { call_.frames++; call_.lastSec = out_->now(); return; }
        if (call_.active) endCall(false);
        const std::string keepCsm = pendingCsm_;
        call_ = Call();
        call_.active = true; call_.uc = uc; call_.mfr = mfr; call_.comm = comm; call_.startSec = call_.lastSec = out_->now(); call_.frames = 1;
        call_.csm = keepCsm;
        st_.calls++;
        char b[240];
        snprintf(b, sizeof b, "user code %d%s, manufacturer %d%s, %s%s%s", uc, uc == 0 ? " (any)" : "", mfr, mfr == 0 ? " (common)" : "", commName(comm),
                 comm == 1 ? " (privacy key needed for the voice)" : "", call_.csm.empty() ? "" : (", call sign " + call_.csm).c_str());
        post("Call", "UC " + std::to_string(uc), b);
    }

    void endCall(bool released) {
        if (!call_.active) return;
        call_.active = false;
        char b[240];
        snprintf(b, sizeof b, "user code %d, %.1f s, %u frames, %u voice frames, bit errors %.1f %%, %s", call_.uc, call_.lastSec - call_.startSec, call_.frames, call_.voice,
                 call_.allBits > 0 ? 100.0 * call_.errBits / call_.allBits : 0.0, released ? "end-of-communication message" : "signal lost");
        post("Call end", "UC " + std::to_string(call_.uc), b);
        pendingCsm_.clear();
    }

    void callSign(const uint8_t* pich) {
        std::string s;
        for (int i = 0; i < 9; i++) {
            int dgt = 0; for (int k = 0; k < 4; k++) dgt = (dgt << 1) | pich[4 * i + k];
            if (dgt > 9) return;   // not a BCD digit: the PICH is wrong
            s += (char)('0' + dgt);
        }
        pendingCsm_ = s; lastCsm_ = s;
        if (call_.active) call_.csm = s;
        if (s == shownCsm_ && out_->now() - shownCsmSec_ < 10) return;
        shownCsm_ = s; shownCsmSec_ = out_->now();
        post("Call sign", s, "call sign " + s + " (9-digit BCD, licence or registration number)");
    }

    void dataBlock(const uint8_t* bits, int half) {
        const std::string hx = hexOf(bits, 80);
        if (hx == lastData_ && out_->now() - lastDataSec_ < 5) { lastDataSec_ = out_->now(); return; }
        lastData_ = hx; lastDataSec_ = out_->now();
        post("Data", std::string("TCH") + (half ? "2" : "1"), "80-bit data block (CRC-12 good): " + hx);
    }

    void lost() {
        endCall(false);
        haveRich_ = false;
        asmOn_ = false;
    }

    Sink* out_;
    std::string tag_;
    double fs_ = 0, off_ = 0, ph_ = 0, fd_ = 0, step_ = 1, pos_ = 1.0, lvl_ = 0;
    int d_ = 1;
    Decimator dec_;
    std::vector<cf32> mix_;
    cf32 prevZ_{0, 0};
    bool haveF_ = false;
    float fPrev_ = 0;
    int64_t inIdx_ = 0;
    Chain chain_;
    Stats st_;
    double lastGood_ = -1e9, errBits_ = 0, allBits_ = 0;
    uint8_t rich_ = 0; bool haveRich_ = false;
    uint8_t asmBits_[72] = {}; int asmType_ = 0, asmGot_ = 0, asmRem_ = 0; bool asmOn_ = false;
    Call call_;
    int lastUc_ = -1;
    std::string lastCsm_, pendingCsm_, shownCsm_, lastMfrKey_, lastData_, confKey_;
    double confSec_ = -1e9, shownCsmSec_ = -1e9, lastIdleSec_ = -1e9, lastMfrSec_ = -1e9, lastDataSec_ = -1e9;
};

// A sink that puts a channel's rows and counts into the receiver's, so scan mode can run several channels
class DcrDec : public Decoder {
public:
    bool configure(double fs) override {
        fs_ = fs;
        build();
        return true;
    }
    void setCenterHz(double hz) override {
        if (hz == centre_) return;
        centre_ = hz;
        if (fs_ > 0) build();
    }
    void reset() override { for (auto& c : chans_) c->reset(); fftN_ = 0; std::fill(acc_.begin(), acc_.end(), 0.f); accBlocks_ = 0; }
    void feed(const cf32* x, size_t n) override {
        if (scan_) detect(x, n);
        for (auto& c : chans_) c->feed(x, n);
        if (scan_) prune();
    }
    void fill(X40Telemetry& t) const override {
        int active = 0, total = 0;
        DcrChan::Stats sum;
        const DcrChan* best = nullptr;
        for (auto& c : chans_) {
            total++;
            if (c->decoding()) active++;
            const auto& s = c->stats();
            sum.frames += s.frames; sum.sacchOk += s.sacchOk; sum.sacchBad += s.sacchBad; sum.richBad += s.richBad; sum.pichOk += s.pichOk; sum.dataOk += s.dataOk;
            sum.voiceFrames += s.voiceFrames; sum.calls += s.calls; sum.syncs += s.syncs;
            if (!best || c->lastGood() > best->lastGood()) best = c.get();
        }
        sum.calls += retiredCalls_; sum.voiceFrames += retiredVoice_; sum.frames += retiredFrames_;
        if (!best || sum.sacchOk + sum.pichOk + sum.dataOk == 0) {
            t.headline = scan_ ? fmt("scanning %d channels of the plan, no DCR frame yet", planInSpan_) : "searching for a DCR frame sync (4-level FSK, 2400 Bd)";
        } else {
            const DcrChan::Call& c = best->call();
            if (c.active) t.headline = fmt("%scall: user code %d, manufacturer %d, %s, %u frames", scan_ && !best->tag().empty() ? (best->tag() + ": ").c_str() : "", c.uc, c.mfr, c.comm == 1 ? "privacy scrambled" : "normal", c.frames);
            else t.headline = fmt("DCR heard on %d channel%s: %llu calls, no call now", total, total == 1 ? "" : "s", (unsigned long long)sum.calls);
        }
        if (centre_ > 0) {
            const dcr::Channel* ch = dcr::findChannel(centre_);
            t.facts.push_back({"channel", ch ? ch->label + fmt(" (%.5f MHz)", ch->hz / 1e6) : fmt("%.5f MHz (not on the DCR plan)", centre_ / 1e6)});
        }
        t.facts.push_back({"mode", scan_ ? fmt("scan: %d plan channels in the span, %d with a decoder, %d decoding", planInSpan_, total, active) : std::string("single channel")});
        if (best) {
            const auto& s = best->stats();
            if (s.frames) {
                t.facts.push_back({"deviation / polarity", fmt("%.0f Hz outer, %s", s.devHz, s.inverted ? "inverted" : "normal")});
                t.facts.push_back({"carrier offset", fmt("%+.0f Hz", s.cfoHz)});
                t.facts.push_back({"bit errors", fmt("%.2f %%", s.berPct)});
            }
            if (best->lastUc() >= 0) t.facts.push_back({"last user code", fmt("%d", best->lastUc())});
            if (!best->lastCsm().empty()) t.facts.push_back({"last call sign", best->lastCsm()});
        }
        t.facts.push_back({"frames", fmt("%llu seen, SACCH %llu good / %llu bad, RICH parity bad %llu", (unsigned long long)sum.frames, (unsigned long long)sum.sacchOk, (unsigned long long)sum.sacchBad, (unsigned long long)sum.richBad)});
        t.facts.push_back({"voice frames", fmt("%llu counted (AMBE+2, 20 ms each); this mode shows them, it does not play audio", (unsigned long long)sum.voiceFrames)});
    }

private:
    void build() {
        chans_.clear(); startOf_.clear(); scan_ = false; planInSpan_ = 0; cand_.clear(); retiredCalls_ = retiredVoice_ = retiredFrames_ = 0;
        if (fs_ <= 0) return;
        if (centre_ > 0) {
            for (const dcr::Channel* c : dcr::channelsOfBand(351)) {
                const double off = c->hz - centre_;
                if (std::fabs(off) < 0.42 * fs_) cand_.push_back({c, off});
            }
            if (cand_.size() < 2) {   // other bands: the tuned plan channel only; both the 467 and the 154 MHz plans, as one list
                cand_.clear();
                for (const dcr::Channel& c : dcr::plan()) {
                    if (c.band == 351) continue;
                    const double off = c.hz - centre_;
                    if (std::fabs(off) < 0.42 * fs_) cand_.push_back({&c, off});
                }
            }
        }
        planInSpan_ = (int)cand_.size();
        if (planInSpan_ >= 2 && fs_ >= 100000) {
            scan_ = true;
            fftN_ = 4096;
            acc_.assign((size_t)fftN_, 0.f); accBlocks_ = 0; blk_.clear();
            win_.resize((size_t)fftN_);
            for (int i = 0; i < fftN_; i++) win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / fftN_));
            return;
        }
        auto c = std::make_unique<DcrChan>(out, "");
        if (c->configure(fs_, 0)) chans_.push_back(std::move(c));
    }

    // scan: one spectrum of the span, the power in each plan channel (+-2.5 kHz) against the quietest fifth; an active channel gets a decoder
    void detect(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            blk_.push_back(x[i]);
            if ((int)blk_.size() == fftN_) {
                for (int k = 0; k < fftN_; k++) blk_[(size_t)k] *= win_[(size_t)k];
                fftx(blk_);
                for (int k = 0; k < fftN_; k++) acc_[(size_t)k] += std::norm(blk_[(size_t)k]);
                blk_.clear();
                accBlocks_++;
                if (accBlocks_ * fftN_ >= 0.25 * fs_) { evaluate(); std::fill(acc_.begin(), acc_.end(), 0.f); accBlocks_ = 0; }
            }
        }
    }
    void evaluate() {
        const double bin = fs_ / fftN_;
        std::vector<double> p(cand_.size());
        for (size_t i = 0; i < cand_.size(); i++) {
            double s = 0;
            for (int k = 0; k < fftN_; k++) {
                const double f = (k < fftN_ / 2 ? k : k - fftN_) * bin;
                if (std::fabs(f - cand_[i].second) <= 2500.0) s += acc_[(size_t)k];
            }
            p[i] = s;
        }
        std::vector<double> q = p;
        std::nth_element(q.begin(), q.begin() + (long)(q.size() / 5), q.end());
        const double floorP = std::max(q[q.size() / 5], 1e-18);
        for (size_t i = 0; i < cand_.size(); i++) {
            const bool strong = p[i] > 10.0 * floorP;
            bool have = false;
            for (auto& c : chans_) if (c->tag() == cand_[i].first->label) { have = true; break; }
            auto it = startOf_.find(cand_[i].first->label);
            const bool cooled = it == startOf_.end() || out->now() - it->second > 20.0;   // a strong carrier that is not DCR is not tried again at once
            if (strong && !have && cooled && chans_.size() < 8) {
                auto c = std::make_unique<DcrChan>(out, cand_[i].first->label);
                if (c->configure(fs_, cand_[i].second)) { startOf_[cand_[i].first->label] = out->now(); chans_.push_back(std::move(c)); }
            }
        }
    }
    // a decoder that found nothing for 8 s goes (its counts stay in the totals)
    void prune() {
        for (size_t i = 0; i < chans_.size();) {
            DcrChan& c = *chans_[i];
            const bool idle = out->now() - c.lastGood() > 8.0 && out->now() - startOf_[c.tag()] > 8.0;
            if (idle && !c.call().active) {
                retiredCalls_ += c.stats().calls; retiredVoice_ += c.stats().voiceFrames; retiredFrames_ += c.stats().frames;
                chans_.erase(chans_.begin() + (long)i); continue;
            }
            i++;
        }
    }

    double fs_ = 0, centre_ = 0;
    bool scan_ = false;
    int planInSpan_ = 0, fftN_ = 0, accBlocks_ = 0;
    std::vector<std::pair<const dcr::Channel*, double>> cand_;
    std::vector<std::unique_ptr<DcrChan>> chans_;
    std::vector<float> acc_, win_;
    std::vector<cf32> blk_;
    std::map<std::string, double> startOf_;
    uint64_t retiredCalls_ = 0, retiredVoice_ = 0, retiredFrames_ = 0;
};

} // namespace

std::unique_ptr<Decoder> makeDcr() { return std::make_unique<DcrDec>(); }

} // namespace dect2::x40
