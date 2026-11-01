// FengYun geostationary receiver (fygeo_rx.h).
#include "dect2/fygeo/fygeo_rx.h"
#include "dect2/dsp/rrc.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/resampler.h"
#include "dect2/dvbs/dvbs_rx.h"
#include "dect2/dvbs/dvbs_tel.h"
#include "dect2/fygeo/fygeo_proto.h"
#include "dect2/fygeo/fygeo_tel.h"
#include "dect2/results/results_io.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
using dect2::dsp::rrc;
constexpr double kRolloff = 0.5;
constexpr int kAcqFft = 4096, kAcqBlocks = 8;
constexpr size_t kSaveBytes = 48u << 20;

struct Fir {
    std::vector<float> h;
    std::vector<cf32> buf;
    size_t pos = 0;
    void set(std::vector<float> taps) { h = std::move(taps); buf.assign(2 * h.size(), cf32(0, 0)); pos = 0; }
    void push(cf32 x) { buf[pos] = x; buf[pos + h.size()] = x; pos = (pos + 1) % h.size(); }
    cf32 out() const {
        const cf32* b = buf.data() + pos;
        float re = 0, im = 0;
        const size_t n = h.size();
        for (size_t i = 0; i < n; i++) { re += h[n - 1 - i] * b[i].real(); im += h[n - 1 - i] * b[i].imag(); }
        return cf32(re, im);
    }
    void clear() { std::fill(buf.begin(), buf.end(), cf32(0, 0)); pos = 0; }
};
inline cf32 cubic(const cf32* y, double t) {
    const int i = (int)std::floor(t);
    const float f = (float)(t - i);
    const cf32 p0 = y[i - 1], p1 = y[i], p2 = y[i + 1], p3 = y[i + 2];
    const cf32 a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
    const cf32 b = p0 - 2.5f * p1 + 2.f * p2 - 0.5f * p3;
    const cf32 c = -0.5f * p0 + 0.5f * p2;
    return ((a * f + b) * f + c) * f + p1;
}
void loopGains(double bnT, double zeta, double& alpha, double& beta) {
    const double th = bnT / (zeta + 1 / (4 * zeta)), d = 1 + 2 * zeta * th + th * th;
    alpha = 4 * zeta * th / d;
    beta = 4 * th * th / d;
}

// a product as the results file holds it (Product itself carries a mutex)
struct ProductRec {
    uint64_t id = 0;
    int kind = 0, vcid = 0, apid = 0;
    goes::Headers hdr;
    std::string name;
    double startSec = 0;
    int w = 0, h = 0;
    std::vector<uint8_t> pix, rowDone;
    int rowsDone = 0, segsDone = 0, segsTotal = 0;
    std::vector<uint8_t> data;
    std::string text, innerName;
    bool complete = false, errors = false;
    uint64_t version = 0, bytes = 0;
};

bool printable(const uint8_t* d, size_t n) {
    if (!n) return false;
    size_t bad = 0;
    for (size_t i = 0; i < n; i++) if (d[i] < 9 || (d[i] > 13 && d[i] < 32) || d[i] > 126) bad++;
    return bad * 20 < n;
}
} // namespace

struct FygeoReceiver::Impl {
    // ---- settings
    std::mutex mu;                        // pub, seq, callbacks, inbox
    FygeoTelemetry pub;
    uint64_t seq = 0;
    std::function<void(const std::string&)> logCb;
    std::function<void(const std::vector<uint8_t>&, int, int)> fileCb;
    std::function<void(const uint8_t*)> caduCb, frameCb;
    std::deque<std::vector<uint8_t>> inbox;
    std::atomic<int> link{kFy4aLrit};
    std::atomic<double> symOverride{0};
    std::atomic<double> offset{0};
    std::atomic<bool> resetReq{false};
    double rate = 0, rateSet = 0;
    int64_t nIn = 0;
    double nextReport = 0;
    std::deque<std::string> logLines;
    std::deque<float> snrH, errH, rateH;

    // ---- products
    mutable std::mutex bookMu;
    std::vector<std::shared_ptr<goes::Product>> list;      // newest first
    uint64_t nextId = 1;
    size_t maxProducts = 60;
    std::map<std::string, std::weak_ptr<goes::Product>> groups;
    std::mutex cm;                        // the chains below (receiver thread callbacks and the analysis thread)

    // ---- FY-4 chain (the DVB-S2 receiver itself is the last member: its threads call back into everything above, so it must be destroyed first)
    int s2Link = -1;
    double s2Rate = 0;
    RationalResampler up;                 // the DVB-S2 receiver wants 2 Msps or more: slower captures are interpolated by a whole factor
    bool useUp = false;
    std::vector<cf32> upOut;
    bool blocking = false;
    goes::Assembler asmb;
    fygeo::TsCaduExtractor ts;
    uint64_t imageSegments = 0, imagesDone = 0, encrypted = 0;

    // ---- S-VISSR chain
    double symbolRate = 660e3;
    double fs2 = 0, sps = 0;
    int D = 1, decPhase = 0;
    Fir lpf, mf;
    double appliedOffset = 0, cfo = 0;
    std::complex<double> nco{1, 0}, rot{1, 0};
    int ncoCount = 0;
    std::vector<cf32> y;
    double pos = 0, spsEst = 0;
    cf32 yprev{0, 0};
    float agc = 1;
    double tAlpha = 0, tBeta = 0;
    double cPhase = 0, cFreq = 0, cAlpha = 0, cBeta = 0;
    float lockM = 0, meanRe2 = 0.5f, meanIm2 = 0.1f;
    bool acq = true, carrierLock = false;
    std::vector<cf32> acqBuf;
    std::vector<float> acqSpec;
    int acqBlocks = 0;
    int64_t sinceAcq = 0, lowLock = 0;
    uint64_t okAtAcq = 0;
    std::vector<double> rejected;
    std::unique_ptr<Fft> fft;
    double levelAcc = 0;
    int64_t levelN = 0;
    std::deque<cf32> constel;
    // bits
    float prevRe = 0;
    bool havePrev = false;
    std::vector<uint8_t> hist = std::vector<uint8_t>(fygeo::kSvissrPreamble, 0);
    size_t histPos = 0;
    uint64_t pn = 0, rawSh = 0;
    int pnCount = 0;
    bool collecting = false;
    int collected = 0;
    std::vector<uint8_t> frame = std::vector<uint8_t>((size_t)fygeo::kSvissrFrameBytes + 8, 0);
    std::vector<uint8_t> preamble;
    uint64_t svOk = 0, svBad = 0, svSyncs = 0, svRej = 0, lastOk = 0, svUsable = 0;
    double berPre = -1, berData = -1;
    int svLine = -1, svImages = 0;
    double lastFrameSec = -1;
    struct SvImage { std::shared_ptr<goes::Product> p[5]; int lastLine = -1; };
    SvImage cur;
    bool svLockReported = false;
    // reporting of lock stages
    bool carrierReported = false, frameLockReported = false;
    std::unique_ptr<DvbsReceiver> s2;

    Impl() {
        asmb.setRawFiles(true, fygeo::kZoneBytes, false);
        asmb.onLog = [this](const std::string& s) { addLog(s, false); };
        asmb.onFile = [this](const std::vector<uint8_t>& f, int vc, int ap) { onFile(f, vc, ap); };
        preamble = fygeo::svissrPreambleBits();
    }

    // ------------------------------------------------------------ logging
    void addLog(const std::string& s, bool engine) {
        {
            std::lock_guard<std::mutex> lk(mu);
            logLines.push_front(s);
            while (logLines.size() > kGoesLogMax) logLines.pop_back();
        }
        if (engine) {
            std::function<void(const std::string&)> cb;
            { std::lock_guard<std::mutex> lk(mu); cb = logCb; }
            if (cb) cb(s);
        }
    }

    // ------------------------------------------------------------ products
    void addProduct(const std::shared_ptr<goes::Product>& p) {
        std::lock_guard<std::mutex> lk(bookMu);
        list.insert(list.begin(), p);
        if (list.size() > maxProducts) list.resize(maxProducts);
    }
    std::vector<std::shared_ptr<goes::Product>> products() const { std::lock_guard<std::mutex> lk(bookMu); return list; }

    std::shared_ptr<goes::Product> newImage(const std::string& name, int w, int h, int bpp, int kind, int segsTotal, double t) {
        auto p = std::make_shared<goes::Product>();
        {
            std::lock_guard<std::mutex> lk(bookMu);
            p->id = nextId++;
        }
        p->kind = kind;
        p->name = name;
        p->startSec = t;
        p->hdr.hasImage = true; p->hdr.bpp = bpp; p->hdr.columns = w; p->hdr.lines = h; p->hdr.compression = 2;
        p->hdr.annotation = name;
        if (w > 0 && h > 0 && (int64_t)w * h <= (int64_t)200 * 1000 * 1000) {
            p->w = w; p->h = h;
            p->pix.assign((size_t)w * h, 0);
            p->rowDone.assign((size_t)h, 0);
        }
        p->segsTotal = std::max(1, segsTotal);
        addProduct(p);
        return p;
    }

    // ------------------------------------------------------------ FY-4 files
    static const char* regionName(const std::string& r) {
        if (r == "DISK") return "full disk";
        if (r == "REGC") return "China";
        if (r == "REGE") return "East Asia";
        return nullptr;
    }
    void onFile(const std::vector<uint8_t>& f, int vcid, int apid) {
        std::function<void(const std::vector<uint8_t>&, int, int)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = fileCb; }
        if (cb) cb(f, vcid, apid);
        fygeo::Fy4Headers h;
        if (!fygeo::parseFy4Headers(f.data(), f.size(), h)) { asmb.fileErrors++; addLog("FY-4: a file with damaged headers", false); return; }
        const double now = (double)nIn / std::max(1.0, rate);
        const size_t hl = h.headerLength;
        const size_t len = std::min<size_t>(f.size() - hl, (size_t)((h.dataBits + 7) / 8));
        const uint8_t* body = f.data() + hl;
        const fygeo::Fy4Name fn = fygeo::parseFy4Name(h.annotation);
        if (h.hasKey && h.key != 0) {
            encrypted++;
            auto p = newImage(h.annotation.empty() ? "encrypted file" : h.annotation, 0, 0, 0, goes::KOther, 1, now);
            std::lock_guard<std::mutex> lk(p->mu);
            p->vcid = vcid; p->apid = apid; p->hdr.hasImage = false;
            p->data.assign(body, body + len);
            p->bytes = f.size(); p->complete = true; p->errors = true;
            p->text = "Encrypted file (key " + std::to_string(h.key) + "): kept as it came, it can not be decoded here.";
            p->version++;
            addLog("FY-4: encrypted file " + h.annotation + " kept as received", false);
            return;
        }
        if (h.fileType == 0 && h.hasImage) { imageSegment(h, fn, body, len, vcid, apid, now, f.size()); return; }
        auto p = newImage(h.annotation.empty() ? "file" : h.annotation, 0, 0, 0, printable(body, len) ? goes::KText : goes::KOther, 1, now);
        std::lock_guard<std::mutex> lk(p->mu);
        p->vcid = vcid; p->apid = apid; p->hdr.hasImage = false;
        p->data.assign(body, body + len);
        if (printable(body, len)) p->text.assign(body, body + len);
        p->bytes = f.size(); p->complete = true; p->version++;
        addLog("FY-4: file " + p->name + " (" + std::to_string(f.size()) + " bytes)", false);
    }

    void imageSegment(const fygeo::Fy4Headers& h, const fygeo::Fy4Name& fn, const uint8_t* body, size_t len, int vcid, int apid, double now, size_t fileBytes) {
        imageSegments++;
        // the code stream starts at the SOC and SIZ markers
        size_t off = len;
        for (size_t i = 0; i + 4 <= len; i++)
            if (body[i] == 0xFF && body[i + 1] == 0x4F && body[i + 2] == 0xFF && body[i + 3] == 0x51) { off = i; break; }
        int dw = 0, dh = 0, bpp = 0;
        std::vector<uint16_t> px;
        bool ok = off < len && fygeo::j2kDecode(body + off, len - off, dw, dh, bpp, px);
        if (!ok && off >= len && h.bpp == 8 && len >= (size_t)h.columns * h.lines && h.columns > 0 && h.lines > 0) {   // not compressed
            dw = h.columns; dh = h.lines; bpp = 8;
            px.assign(body, body + (size_t)dw * dh);
            ok = true;
        }
        const int segs = std::max(1, h.totalSeg);
        const int channel = h.channel;
        std::string key = (fn.ok ? fn.group : h.satName + "/" + h.instName) + "#" + std::to_string(channel) + "#" + std::to_string(segs);
        const int segW = ok ? dw : h.columns, segH = ok ? dh : h.lines;
        std::shared_ptr<goes::Product> prod;
        {
            auto it = groups.find(key);
            if (it != groups.end()) {
                prod = it->second.lock();
                if (prod) {
                    std::lock_guard<std::mutex> lk(prod->mu);
                    // a segment that is already in: this is the next picture of the series
                    const int y0 = segLine0(h, segH, prod->h);
                    if (prod->complete || prod->w != segW || (y0 >= 0 && y0 < prod->h && prod->rowDone[(size_t)y0])) prod.reset();
                }
            }
        }
        if (!prod) {
            char t[64] = "";
            const double ts0 = fn.ok ? fn.startSec : fygeo::fy4HeaderTime(h);
            if (ts0 > 0) { std::time_t tt = (std::time_t)ts0; std::tm g{};
#ifdef _WIN32
                gmtime_s(&g, &tt);
#else
                gmtime_r(&tt, &g);
#endif
                strftime(t, sizeof t, " %Y-%m-%d %H:%MZ", &g); }
            const char* rn = fn.ok ? regionName(fn.region) : nullptr;
            char nm[200];
            snprintf(nm, sizeof nm, "%s %s C%02d%s%s%s", h.satName.empty() ? "FY-4" : h.satName.c_str(), h.instName.empty() ? "AGRI" : h.instName.c_str(), channel,
                     rn ? " " : "", rn ? rn : "", t);
            const int kind = fn.ok && fn.region == "DISK" ? goes::KFullDisk : fn.ok && (fn.region == "REGC" || fn.region == "REGE") ? goes::KMeso : goes::KImage;
            prod = newImage(nm, segW, segH * segs, bpp ? bpp : h.bpp, kind, segs, now);
            prod->vcid = vcid; prod->apid = apid;
            prod->hdr.hasNav = h.hasNav; prod->hdr.projection = h.projection;
            groups[key] = prod;
            if (groups.size() > 64) { for (auto i = groups.begin(); i != groups.end();) i = i->second.expired() ? groups.erase(i) : std::next(i); }
        }
        std::lock_guard<std::mutex> lk(prod->mu);
        prod->bytes += fileBytes;
        prod->segsDone++;
        if (!ok) { prod->errors = true; addLog("FY-4: a picture segment could not be decoded (" + h.annotation + ")", false); }
        else if (prod->w > 0) {
            const int y0 = std::max(0, segLine0(h, segH, prod->h));
            const int shift = std::max(0, bpp - 8);
            for (int r = 0; r < segH && y0 + r < prod->h; r++) {
                uint8_t* d = prod->pix.data() + (size_t)(y0 + r) * prod->w;
                const uint16_t* s = px.data() + (size_t)r * dw;
                for (int x = 0; x < prod->w && x < dw; x++) d[x] = (uint8_t)std::min(255, s[x] >> shift);
                if (!prod->rowDone[(size_t)(y0 + r)]) { prod->rowDone[(size_t)(y0 + r)] = 1; prod->rowsDone++; }
            }
        }
        if (prod->segsDone >= prod->segsTotal) { prod->complete = true; imagesDone++; }
        prod->version++;
        char b[200];
        snprintf(b, sizeof b, "FY-4: %s segment %d of %d (%d x %d, %d bit)%s", prod->name.c_str(), h.segNo, segs, dw, dh, bpp, ok ? "" : ", damaged");
        addLog(b, false);
    }
    // first line of a segment in the picture
    static int segLine0(const fygeo::Fy4Headers& h, int segH, int totalH) {
        if (h.segLine > 0 && h.segLine + segH <= totalH + 1) return h.segLine;
        if (h.segPos >= 1) return (h.segPos - 1) * segH;
        return std::max(0, (h.segNo - 1) * segH);
    }

    // ------------------------------------------------------------ FY-4 chain
    void ensureS2() {
        const int l = link.load();
        if (!s2) {
            s2 = std::make_unique<DvbsReceiver>();
            s2->setLogCallback([this](const std::string& s) { addLog(s, false); });
            s2->setPacketCallback([this](const uint8_t* p, size_t n, double) { onTs(p, n); });
            s2->setBlocking(blocking);
            s2->setStandardHint(2);
        }
        const double sr = symOverride.load() > 0 ? symOverride.load() : fygeoSymbolRate(l);
        s2->setSymbolRate(sr);
        if (s2Rate != rateSet || s2Link != l) {
            double r = rateSet;
            useUp = false;
            if (r < 2e6 - 1) {
                const int f = (int)std::ceil(2e6 / r);
                useUp = up.configure(r, r * f);
                if (useUp) r *= f;
            }
            s2->configure(r);
            s2Rate = rateSet; s2Link = l;
        }
    }
    void onTs(const uint8_t* p, size_t n) {
        std::lock_guard<std::mutex> lk(cm);
        const double now = (double)nIn / std::max(1.0, rate);
        ts.push(p, n, [&](const uint8_t* cadu, int) {
            std::function<void(const uint8_t*)> cb;
            { std::lock_guard<std::mutex> l2(mu); cb = caduCb; }
            if (cb) cb(cadu);
            asmb.push(cadu + 4, now);
        });
    }
    void drainInbox() {
        for (;;) {
            std::vector<uint8_t> d;
            { std::lock_guard<std::mutex> lk(mu); if (inbox.empty()) return; d = std::move(inbox.front()); inbox.pop_front(); }
            if (link.load() == kFy2Svissr) {
                size_t n = 0;
                for (size_t o = 0; o + fygeo::kSvissrFrameBytes <= d.size(); o += fygeo::kSvissrFrameBytes, n++) svFrame(d.data() + o, true);
                char b[96]; snprintf(b, sizeof b, "S-VISSR: file with %zu frames", n);
                addLog(b, true);
            } else {
                size_t n = 0;
                std::lock_guard<std::mutex> lk(cm);
                for (size_t o = 0; o + fygeo::kCaduBytes <= d.size(); o += fygeo::kCaduBytes) {
                    const uint8_t* c = d.data() + o;
                    if (c[0] != 0x1A || c[1] != 0xCF || c[2] != 0xFC || c[3] != 0x1D) continue;
                    asmb.push(c + 4, 0.0); n++; ts.caduCount++;
                }
                char b[96]; snprintf(b, sizeof b, "FY-4: CADU file, %zu frames", n);
                addLog(b, true);
            }
        }
    }

    // ------------------------------------------------------------ S-VISSR front end
    double minInputRate() const { return std::max(200000.0, 1.4 * symbolRate); }
    double carrierHz() const { return cfo + cFreq / (2 * kPi) * symbolRate; }
    void setRot() { const double w = -2 * kPi * (appliedOffset + cfo) / rate; rot = std::complex<double>(std::cos(w), std::sin(w)); }
    void resetCarrier() { cPhase = 0; cFreq = 0; lockM = 0; double a, b; loopGains(0.01, 0.707, a, b); cAlpha = a; cBeta = b; }
    void resetBits() {
        havePrev = false; prevRe = 0;
        std::fill(hist.begin(), hist.end(), 0); histPos = 0;
        pn = 0; rawSh = 0; pnCount = 0; collecting = false; collected = 0; bitsLocked = 0;
    }
    void svSetup(double r) {
        rate = r;
        D = std::max(1, (int)std::floor(r / (2.3 * symbolRate)));
        fs2 = r / D;
        sps = fs2 / symbolRate;
        if (D > 1) {
            const int n = 8 * D + 1;
            std::vector<float> h((size_t)n);
            const double fc = 0.85 * symbolRate / r;
            double sum = 0;
            for (int i = 0; i < n; i++) {
                const double m = i - (n - 1) / 2.0, w = 0.54 - 0.46 * std::cos(2 * kPi * i / (n - 1));
                h[(size_t)i] = (float)(w * (std::fabs(m) < 1e-9 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m)));
                sum += h[(size_t)i];
            }
            for (auto& v : h) v = (float)(v / sum);
            lpf.set(h);
        }
        const int half = (int)std::ceil(6 * sps);
        std::vector<float> h((size_t)(2 * half + 1));
        double e = 0;
        for (int i = -half; i <= half; i++) { h[(size_t)(i + half)] = (float)rrc(i / sps, kRolloff); e += h[(size_t)(i + half)] * h[(size_t)(i + half)]; }
        for (auto& v : h) v = (float)(v / std::sqrt(e));
        mf.set(h);
        fft = std::make_unique<Fft>(kAcqFft);
        loopGains(0.004, 0.707, tAlpha, tBeta);
        svReset();
    }
    void svReset() {
        cfo = 0; appliedOffset = offset.load(); setRot(); nco = 1;
        lpf.clear(); mf.clear(); decPhase = 0;
        y.clear(); pos = 2 + sps; spsEst = sps; yprev = 0; agc = 1;
        resetCarrier();
        rejected.clear();
        acq = true; carrierLock = false; acqBuf.clear(); acqSpec.assign(kAcqFft, 0.f); acqBlocks = 0; sinceAcq = 0; lowLock = 0;
        meanRe2 = 0.5f; meanIm2 = 0.1f;
        resetBits();
        carrierReported = svLockReported = false;
    }

    void acquire(cf32 v) {
        acqBuf.push_back(v * v);
        if ((int)acqBuf.size() < kAcqFft) return;
        fft->forward(acqBuf.data());
        for (int i = 0; i < kAcqFft; i++) acqSpec[(size_t)i] += std::norm(acqBuf[(size_t)i]);
        acqBuf.clear();
        if (++acqBlocks < kAcqBlocks) return;
        if (rejected.size() > 6) rejected.clear();
        const double binHz = fs2 / kAcqFft / 2;
        int best = -1;
        float bv = 0;
        double mean = 0;
        for (int i = 0; i < kAcqFft; i++) {
            mean += acqSpec[(size_t)i];
            const double cand = cfo + (i < kAcqFft / 2 ? i : i - kAcqFft) * binHz;
            bool skip = false;
            for (double r : rejected) skip |= std::fabs(cand - r) < 4 * binHz;
            if (skip) continue;
            if (acqSpec[(size_t)i] > bv) { bv = acqSpec[(size_t)i]; best = i; }
        }
        mean /= kAcqFft;
        if (best >= 0 && bv > 12 * mean) {
            const float l = acqSpec[(size_t)((best + kAcqFft - 1) % kAcqFft)], c = acqSpec[(size_t)best], r = acqSpec[(size_t)((best + 1) % kAcqFft)];
            const float den = l - 2 * c + r;
            const double frac = std::fabs(den) > 1e-20f ? 0.5 * (l - r) / den : 0;
            const double k = (best < kAcqFft / 2 ? best : best - kAcqFft) + frac;
            cfo += k * fs2 / kAcqFft / 2;
            setRot();
            resetCarrier();
            acq = false; sinceAcq = 0; okAtAcq = svOk;
        }
        std::fill(acqSpec.begin(), acqSpec.end(), 0.f);
        acqBlocks = 0;
    }

    void svSymbol(cf32 v) {
        const cf32 z = v * cf32((float)std::cos(-cPhase), (float)std::sin(-cPhase));
        const float re = z.real(), im = z.imag();
        const float e = (re >= 0 ? im : -im);
        cPhase += cAlpha * e + cFreq;
        cFreq += cBeta * e;
        if (cPhase > kPi) cPhase -= 2 * kPi; else if (cPhase < -kPi) cPhase += 2 * kPi;
        const float p = re * re + im * im + 1e-12f;
        lockM += 0.002f * ((re * re - im * im) / p - lockM);
        meanRe2 += 0.002f * (re * re - meanRe2);
        meanIm2 += 0.002f * (im * im - meanIm2);
        constel.push_back(z);
        if (constel.size() > 512) constel.pop_front();
        if (!acq) {
            sinceAcq++;
            if (sinceAcq == 20000) { double a, b; loopGains(0.004, 0.707, a, b); cAlpha = a; cBeta = b; }
            if (sinceAcq > 30000) {
                const bool l = lockM > 0.35f;
                if (l != carrierLock) {
                    carrierLock = l;
                    if (l && !carrierReported) { carrierReported = true; char b[96]; snprintf(b, sizeof b, "S-VISSR: carrier locked, offset %.1f kHz", carrierHz() / 1000); addLog(b, true); }
                }
                lowLock = lockM < 0.2f ? lowLock + 1 : 0;
                if (lowLock > 60000) { acq = true; carrierLock = false; lowLock = 0; resetBits(); }
                if (!carrierLock && svOk == okAtAcq && sinceAcq > 160000) { rejected.push_back(cfo); acq = true; resetBits(); }
            }
        }
        // NRZ-M: a bit is 1 when the level changed
        if (acq || sinceAcq < 3000) bitsLocked = 0; else bitsLocked++;
        if (havePrev) svBit((uint8_t)((re >= 0) != (prevRe >= 0)));
        prevRe = re;
        havePrev = true;
    }

    // one decoded bit: the PN sequence follower (a free running copy of the preamble's generator that joins the stream when the stream stops
    // agreeing with it), the sync window, and the 354848 bits of a frame behind it
    void svBit(uint8_t bit) {
        hist[histPos] = bit;
        histPos = histPos + 1 == hist.size() ? 0 : histPos + 1;
        rawSh = rawSh << 1 | bit;
        const uint8_t pb = (uint8_t)(((pn >> 14) ^ (pn >> 13)) & 1);
        pn = pn << 1 | pb;
        if (bit == pb) { if (pnCount < 5000) pnCount++; }
        else { pnCount -= 2; if (pnCount <= 0) { pnCount = 1; pn = rawSh; } }
        if (collecting) {
            frame[(size_t)(collected >> 3)] = (uint8_t)((frame[(size_t)(collected >> 3)] << 1) | bit);
            if (++collected == fygeo::kSvissrFrameBits) { collecting = false; collected = 0; svFrame(frame.data(), false); }
            return;
        }
        if (pn != fygeo::kSvissrMarker || pnCount <= 1000) return;
        // a candidate: compare the 10000 bits behind it with the known preamble
        // (only the bits since the carrier loop had settled count: a frame that starts while it was still pulling in must not count its first bits)
        const size_t nb = std::min<size_t>(hist.size(), (size_t)std::max<int64_t>(0, bitsLocked));
        if (nb < 1000) { pnCount = 0; return; }
        size_t mism = 0;
        for (size_t i = hist.size() - nb; i < hist.size(); i++) mism += hist[(histPos + i) % hist.size()] != preamble[i];
        const double ber = (double)mism / (double)nb;
        pnCount = 0;
        svSyncs++;
        if (ber > 0.3) { svRej++; svBad++; return; }
        berPre = berPre < 0 ? ber : 0.7 * berPre + 0.3 * ber;
        lastPreBer = ber;
        collecting = true; collected = 0;
        memset(frame.data(), 0, frame.size());
        if (!svLockReported) { svLockReported = true; addLog("S-VISSR: frame sync found", true); }
    }
    double lastPreBer = 0;
    int64_t bitsLocked = 0;           // decoded bits since the carrier loop settled

    void svFrame(const uint8_t* rx, bool descrambled) {
        uint8_t f[fygeo::kSvissrFrameBytes];
        memcpy(f, rx, sizeof f);
        if (!descrambled) fygeo::svissrDescramble(f);
        std::function<void(const uint8_t*)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = frameCb; }
        if (cb) cb(f);
        const int c = fygeo::svissrLineCounter(f);
        int tot = 0;
        const int se = fygeo::svissrSpareErrors(f, &tot);
        const double dber = (double)se / std::max(1, tot);
        berData = berData < 0 ? dber : 0.8 * berData + 0.2 * dber;
        if (c >= fygeo::kSvissrMaxLines || (descrambled ? false : lastPreBer > 0.3)) { svBad++; return; }
        svOk++;
        if (descrambled || lastPreBer < 0.02) svUsable++;
        svLine = c;
        svLine2(f, c);
    }

    void svLine2(const uint8_t* f, int c) {
        const double now = (double)nIn / std::max(1.0, rate);
        if (!cur.p[0] || (cur.lastLine >= 0 && c + 100 < cur.lastLine) || (lastFrameSec >= 0 && now - lastFrameSec > 300 && c < cur.lastLine)) startImage(now);
        lastFrameSec = now;
        cur.lastLine = c;
        static thread_local std::vector<uint8_t> ir[4], vis;
        for (auto& v : ir) v.resize(fygeo::kSvissrIrWidth);
        vis.resize((size_t)4 * fygeo::kSvissrVisWidth);
        fygeo::svissrUnpack(f, ir[0].data(), ir[1].data(), ir[2].data(), ir[3].data(), vis.data());
        for (int k = 0; k < 4; k++) {
            auto& p = cur.p[k];
            std::lock_guard<std::mutex> lk(p->mu);
            if (c >= p->h) continue;
            memcpy(p->pix.data() + (size_t)c * p->w, ir[(size_t)k].data(), fygeo::kSvissrIrWidth);
            if (!p->rowDone[(size_t)c]) { p->rowDone[(size_t)c] = 1; p->rowsDone++; }
            p->version++;
        }
        {
            auto& p = cur.p[4];
            std::lock_guard<std::mutex> lk(p->mu);
            // half resolution: two lines of 2 x 2 pixel means a frame
            for (int r = 0; r < 2; r++) {
                const int row = 2 * c + r;
                if (row >= p->h) break;
                uint8_t* d = p->pix.data() + (size_t)row * p->w;
                const uint8_t* a = vis.data() + (size_t)(2 * r) * fygeo::kSvissrVisWidth;
                const uint8_t* b = a + fygeo::kSvissrVisWidth;
                for (int x = 0; x < p->w; x++) d[x] = (uint8_t)((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
                if (!p->rowDone[(size_t)row]) { p->rowDone[(size_t)row] = 1; p->rowsDone++; }
            }
            p->version++;
        }
    }
    void startImage(double now) {
        for (auto& p : cur.p) if (p) { std::lock_guard<std::mutex> lk(p->mu); p->complete = true; }
        static const char* const names[5] = {"IR1 10.8 um", "IR2 12 um", "IR3 6.75 um", "IR4 3.75 um", "VIS 0.725 um (half resolution)"};
        char t[48];
        std::time_t tt = std::time(nullptr);
        std::tm g{};
#ifdef _WIN32
        gmtime_s(&g, &tt);
#else
        gmtime_r(&tt, &g);
#endif
        strftime(t, sizeof t, "%Y-%m-%d %H:%MZ", &g);
        svImages++;
        for (int k = 0; k < 5; k++) {
            const bool vis = k == 4;
            const int w = vis ? fygeo::kSvissrVisWidth / 2 : fygeo::kSvissrIrWidth, h = vis ? fygeo::kSvissrMaxLines * 2 : fygeo::kSvissrMaxLines;
            char nm[160];
            snprintf(nm, sizeof nm, "FY-2 S-VISSR %s %s", names[k], t);
            cur.p[k] = newImage(nm, w, h, k == 3 ? 10 : k == 4 ? 6 : 8, goes::KFullDisk, 1, now);
            cur.p[k]->hdr.compression = 0;
        }
        cur.lastLine = -1;
        addLog("S-VISSR: a new picture starts", true);
    }

    // ------------------------------------------------------------ S-VISSR timing and input
    void timing() {
        const double half = spsEst / 2;
        while (pos + 3 < (double)y.size()) {
            if (pos - half - 2 < 0) { pos += 1; continue; }
            const cf32 yk = cubic(y.data(), pos), ym = cubic(y.data(), pos - half);
            agc += 0.001f * (std::norm(yk) - agc);
            const float g = 1.f / std::sqrt(std::max(1e-12f, agc));
            const cf32 a = yk * g, m = ym * g;
            const float e = ((yprev - a) * std::conj(m)).real();
            yprev = a;
            spsEst += tBeta * e;
            spsEst = std::max(sps * 0.995, std::min(sps * 1.005, spsEst));
            pos += spsEst + tAlpha * e * sps;
            svSymbol(a);
        }
        const int drop = (int)std::floor(pos - spsEst - 4);
        if (drop > 0 && drop <= (int)y.size()) { y.erase(y.begin(), y.begin() + drop); pos -= drop; }
    }
    void svFeed(const cf32* x, size_t n) {
        y.reserve(y.size() + n / D + 8);
        const double off = offset.load();
        if (off != appliedOffset) { appliedOffset = off; setRot(); }
        for (size_t i = 0; i < n; i++) {
            nco *= rot;
            if (++ncoCount >= 1024) { nco /= std::abs(nco); ncoCount = 0; }
            const cf32 v = x[i] * cf32((float)nco.real(), (float)nco.imag());
            levelAcc += std::norm(x[i]); levelN++;
            cf32 d = v;
            if (D > 1) {
                lpf.push(v);
                if (++decPhase < D) continue;
                decPhase = 0;
                d = lpf.out();
            }
            mf.push(d);
            const cf32 o = mf.out();
            y.push_back(o);
            if (acq) acquire(o * (1.f / std::sqrt(std::max(1e-12f, agc))));
        }
        timing();
    }

    // ------------------------------------------------------------ reporting
    std::string hintFor(const FygeoTelemetry& t, bool rateOk) const {
        const int l = t.link;
        if (!rateOk) {
            char b[160];
            snprintf(b, sizeof b, "The sample rate is too low for %s: use at least %.1f Msps.", fygeoLinkName(l), (l == kFy2Svissr ? minInputRate() : 1.4 * fygeoSymbolRate(l) * 1.3) / 1e6);
            return b;
        }
        if (t.state == 0)
            return "No carrier yet. Point a dish (about 1 m or more) at the satellite, check the LNA / filter power and the 1.7 GHz band, and tune to the link's frequency.";
        if (l == kFy2Svissr) {
            if (t.state == 1) return "Carrier found but no frame sync: the signal is too weak (Es/N0 below about 3 dB) or this is not S-VISSR (660 ksym/s BPSK). Dish too small or mispointed?";
            if (t.svPreambleBer > 0.12) return "Frames are found but the bit error ratio is high (the pictures will be noisy): more dish gain, a better LNA or a re-pointed dish would help.";
            return "";
        }
        if (t.state == 1) return "Carrier found but no DVB-S2 frames: wrong symbol rate for this link (try another FengYun-4 entry) or the signal is too weak.";
        if (t.state == 2 && t.caduCount == 0)
            return "DVB-S2 frames decode, but no LRIT / HRIT data units are in the transport stream: wrong link for this satellite, or the stream is encrypted or uses another PID layout.";
        if (t.state >= 3 && t.products.empty()) return "Data units arrive; waiting for the first complete file (a picture segment takes a while).";
        return "";
    }

    void report() {
        FygeoTelemetry t;
        const int l = link.load();
        t.link = l;
        t.inputRate = rate;
        t.timeSec = (double)nIn / std::max(1.0, rate);
        t.levelDb = levelN ? (float)(10 * std::log10(std::max(1e-20, levelAcc / levelN))) : -200.f;
        levelAcc = 0; levelN = 0;
        bool rateOk = true;
        if (l == kFy2Svissr) {
            const bool decoding = svOk > 0 && collectedRecent();
            t.state = decoding ? 3 : (collecting || svSyncs > svRej) ? 2 : carrierLock ? 1 : 0;
            t.cfoHz = carrierHz();
            const float a2 = std::max(1e-9f, meanRe2 - meanIm2);
            t.snrDb = (float)(10 * std::log10(std::max(1e-6f, a2 / (2 * std::max(1e-9f, meanIm2)))));
            t.dataValid = t.state >= 3;
            t.blocksOk = svOk; t.blocksBad = svBad;
            t.symbolRate = fs2 / std::max(1e-9, spsEst);
            t.carrierLock = carrierLock || decoding;
            t.frameLock = t.state >= 2;
            t.lockMetric = std::max(0.f, lockM);
            t.frames = svOk + svBad;
            t.constellation.assign(constel.begin(), constel.end());
            t.svFrames = svOk; t.svSyncs = svSyncs; t.svRejected = svRej; t.svUsable = svUsable;
            t.svPreambleBer = berPre; t.svDataBer = berData; t.svLine = svLine; t.svImages = svImages;
            t.signalInfo = "BPSK NRZ-M " + std::to_string((int)std::lround(symbolRate / 1e3)) + " ksym/s";
            rateOk = rateSet >= minInputRate() - 1;
            const uint64_t dok = svOk - lastOk;
            lastOk = svOk;
            auto push = [](std::deque<float>& h, float v) { h.push_back(v); if (h.size() > kGoesHist) h.pop_front(); };
            push(snrH, t.snrDb); push(errH, (float)std::max(0.0, berPre) * 100.f); push(rateH, (float)dok * 4.f);
        } else {
            DvbsTelemetry d;
            bool have = false;
            if (s2) { have = s2->telemetry(d, 0); rateOk = s2->ready(); }
            uint64_t cadus, tsp, tse;
            std::vector<std::pair<int, uint64_t>> pids;
            {
                std::lock_guard<std::mutex> lk(cm);
                cadus = ts.caduCount; tsp = ts.packets; tse = ts.tsErrors; pids = ts.pids();
                t.vcdus = asmb.vcdus; t.fillVcdus = asmb.fillVcdus; t.vcduGaps = asmb.vcduGaps; t.packets = asmb.packets; t.crcErrors = asmb.crcErrors;
                t.files = asmb.files; t.fileErrors = asmb.fileErrors; t.scid = asmb.scid;
                for (int v = 0; v < 63; v++) if (asmb.perVcid[v]) t.vcids.emplace_back(v, asmb.perVcid[v]);
            }
            t.caduCount = cadus; t.tsPackets = tsp; t.tsErrors = tse; t.pids = std::move(pids);
            t.encryptedFiles = encrypted; t.imageSegments = imageSegments; t.imagesDone = imagesDone;
            if (have) {
                t.s2Spectrum = d.lockSpectrum; t.s2Timing = d.lockTiming; t.s2Carrier = d.lockCarrier; t.s2Frame = d.lockFrame; t.s2Fec = d.lockFec; t.s2Ts = d.tsLock;
                t.s2Modulation = d.modulationName; t.s2CodeRate = d.codeRate; t.s2Modcod = d.modcod; t.s2SymbolRate = d.symbolRate; t.s2MerDb = d.merDb; t.s2PreFecBer = d.preFecBer;
                t.cfoHz = d.cfoHz; t.snrDb = d.snrDb;
                t.blocksOk = d.blocksOk; t.blocksBad = d.blocksBad;
                t.symbolRate = d.symbolRate;
                t.constellation = d.cells;
                if (t.constellation.size() > 512) t.constellation.erase(t.constellation.begin(), t.constellation.end() - 512);
                char b[120];
                if (d.standard == 2) snprintf(b, sizeof b, "DVB-S2 %s %s, %.1f ksym/s", d.modulationName.c_str(), d.codeRate.c_str(), d.symbolRate / 1e3);
                else snprintf(b, sizeof b, "searching, %.0f ksym/s expected", (symOverride.load() > 0 ? symOverride.load() : fygeoSymbolRate(l)) / 1e3);
                t.signalInfo = b;
            }
            t.carrierLock = t.s2Carrier;
            t.frameLock = t.s2Fec;
            t.state = cadus > 0 ? 3 : t.s2Fec ? 2 : t.s2Carrier ? 1 : 0;
            t.dataValid = t.state >= 3;
            t.lockMetric = t.s2Carrier ? 1.f : 0.f;
            const uint64_t dok = t.blocksOk - lastOk;
            lastOk = t.blocksOk;
            auto push = [](std::deque<float>& h, float v) { h.push_back(v); if (h.size() > kGoesHist) h.pop_front(); };
            push(snrH, t.snrDb); push(errH, std::max(0.f, t.s2PreFecBer) * 100.f); push(rateH, (float)dok * 4.f);
        }
        t.snrHist.assign(snrH.begin(), snrH.end());
        t.errHist.assign(errH.begin(), errH.end());
        t.rateHist.assign(rateH.begin(), rateH.end());
        t.products = products();
        {
            std::lock_guard<std::mutex> lk(mu);
            t.log.assign(logLines.begin(), logLines.end());
        }
        t.hint = hintFor(t, rateOk);
        std::lock_guard<std::mutex> lk(mu);
        t.seq = ++seq;
        pub = std::move(t);
    }
    bool collectedRecent() const { return svOk > 0 && lastFrameSec >= 0 && (double)nIn / std::max(1.0, rate) - lastFrameSec < 4.0; }

    void feed(const cf32* x, size_t n) {
        if (rateSet <= 0 || n == 0) return;
        if (resetReq.exchange(false)) doReset();
        drainInbox();
        const int l = link.load();
        if (l == kFy2Svissr) svFeed(x, n);
        else {
            ensureS2();
            if (s2) {
                if (useUp) { upOut.clear(); up.process(x, n, upOut); if (!upOut.empty()) s2->feed(upOut.data(), upOut.size()); }
                else s2->feed(x, n);
            }
        }
        nIn += (int64_t)n;
        if ((double)nIn >= nextReport) { nextReport += 0.25 * rate; report(); }
    }
    void doReset() {
        const int l = link.load();
        if (l == kFy2Svissr) svReset();
        else if (s2) { s2->reset(); up.reset(); }
        std::lock_guard<std::mutex> lk(cm);
        asmb.reset();
        ts = fygeo::TsCaduExtractor();
        carrierReported = frameLockReported = false;
    }
};

FygeoReceiver::FygeoReceiver() : p_(std::make_unique<Impl>()) {}
FygeoReceiver::~FygeoReceiver() = default;

void FygeoReceiver::configure(double r) {
    Impl& m = *p_;
    { std::lock_guard<std::mutex> lk(m.mu); m.rateSet = r; }
    if (r <= 0) return;
    m.rate = r;
    m.nIn = 0; m.nextReport = 0.25 * r;
    m.symbolRate = m.symOverride.load() > 0 ? m.symOverride.load() : fygeoSymbolRate(kFy2Svissr);
    if (m.link.load() == kFy2Svissr) m.svSetup(r);
    else {
        m.s2Rate = 0;
        m.ensureS2();
    }
}
void FygeoReceiver::setLink(int link) {
    link = std::max(0, std::min(kFygeoLinks - 1, link));
    if (p_->link.exchange(link) == link) return;
    const double r = p_->rateSet;
    if (r > 0) configure(r);
}
int FygeoReceiver::link() const { return p_->link.load(); }
void FygeoReceiver::setSymbolRate(double hz) {
    p_->symOverride = hz > 1000 ? hz : 0;
    const double r = p_->rateSet;
    if (r > 0) configure(r);
}
double FygeoReceiver::symbolRate() const {
    const double o = p_->symOverride.load();
    return o > 0 ? o : fygeoSymbolRate(p_->link.load());
}
void FygeoReceiver::setSignalOffset(double hz) { p_->offset = hz; }
bool FygeoReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->link.load() == kFy2Svissr) return p_->rateSet >= p_->minInputRate() - 1;
    return p_->s2 ? p_->s2->ready() : p_->rateSet > 0;
}
void FygeoReceiver::reset() { p_->resetReq = true; }
void FygeoReceiver::clearProducts() {
    {
        std::lock_guard<std::mutex> lk(p_->bookMu);
        p_->list.clear();
        p_->groups.clear();
    }
    p_->cur = Impl::SvImage();
    { std::lock_guard<std::mutex> lk(p_->mu); p_->logLines.clear(); p_->pub.products.clear(); p_->pub.log.clear(); p_->pub.seq++; p_->seq = p_->pub.seq; }
}
void FygeoReceiver::feed(const cf32* x, size_t n) { p_->feed(x, n); }
bool FygeoReceiver::telemetry(FygeoTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void FygeoReceiver::clearResults() { clearProducts(); }
std::string FygeoReceiver::saveResults() const {
    std::vector<ProductRec> v;
    size_t total = 0;
    for (const auto& q : p_->products()) {
        if (!q) continue;
        ProductRec r;
        r.id = q->id; r.kind = q->kind; r.vcid = q->vcid; r.apid = q->apid; r.hdr = q->hdr; r.name = q->name; r.startSec = q->startSec;
        {
            std::lock_guard<std::mutex> pl(q->mu);
            r.w = q->w; r.h = q->h; r.pix = q->pix; r.rowDone = q->rowDone; r.rowsDone = q->rowsDone; r.segsDone = q->segsDone; r.segsTotal = q->segsTotal;
            r.data = q->data; r.text = q->text; r.innerName = q->innerName; r.complete = q->complete; r.errors = q->errors; r.version = q->version; r.bytes = q->bytes;
        }
        total += r.pix.size() + r.data.size() + r.text.size();
        if (total > kSaveBytes && !v.empty()) break;
        v.push_back(std::move(r));
    }
    return results::pack(v);
}
bool FygeoReceiver::loadResults(const std::string& blob) {
    std::vector<ProductRec> v;
    if (!results::unpack(blob, v)) return false;
    std::vector<std::shared_ptr<goes::Product>> list;
    uint64_t mx = 0;
    for (auto& r : v) {
        auto q = std::make_shared<goes::Product>();
        q->id = r.id; q->kind = r.kind; q->vcid = r.vcid; q->apid = r.apid; q->hdr = r.hdr; q->name = r.name; q->startSec = r.startSec;
        q->w = r.w; q->h = r.h; q->pix = std::move(r.pix); q->rowDone = std::move(r.rowDone); q->rowsDone = r.rowsDone; q->segsDone = r.segsDone; q->segsTotal = r.segsTotal;
        q->data = std::move(r.data); q->text = std::move(r.text); q->innerName = std::move(r.innerName); q->complete = r.complete; q->errors = r.errors; q->version = r.version; q->bytes = r.bytes;
        mx = std::max(mx, q->id);
        list.push_back(std::move(q));
    }
    {
        std::lock_guard<std::mutex> lk(p_->bookMu);
        p_->list = std::move(list);
        p_->nextId = std::max(p_->nextId, mx + 1);
        p_->groups.clear();
    }
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pub.seq++;
    p_->seq = p_->pub.seq;
    p_->pub.products = p_->products();
    return true;
}
void FygeoReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = std::move(cb); }
void FygeoReceiver::setFileCallback(std::function<void(const std::vector<uint8_t>&, int, int)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->fileCb = std::move(cb); }
void FygeoReceiver::setCaduCallback(std::function<void(const uint8_t*)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->caduCb = std::move(cb); }
void FygeoReceiver::setSvissrFrameCallback(std::function<void(const uint8_t*)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->frameCb = std::move(cb); }
void FygeoReceiver::importFrames(std::vector<uint8_t> data) { std::lock_guard<std::mutex> lk(p_->mu); p_->inbox.push_back(std::move(data)); }
void FygeoReceiver::flush() { if (p_->s2) p_->s2->flush(); }
void FygeoReceiver::setBlocking(bool on) { p_->blocking = on; if (p_->s2) p_->s2->setBlocking(on); }

// ---------------------------------------------------------------- tunings
namespace {
ModeTuning fyTuning(int stdMode, const char* id, const char* name, double mhz, double sampleRate, double bwMhz, double minRate) {
    ModeTuning t;
    t.stdMode = stdMode; t.id = id; t.name = name;
    t.minMhz = 1670; t.maxMhz = 1710; t.defMhz = mhz;
    t.sampleRate = sampleRate;
    t.basebandHz = 0;
    t.bandwidthMhz = bwMhz;
    t.minSampleRate = minRate;
    t.tuneOffsetHz = 0;
    return t;
}
} // namespace
ModeTuning fy4aLritTuning() { return fyTuning(234, "fy4alrit", "FY-4A LRIT", 1697.0, 2000000, 0.3, 250000); }       // family 232 + 2
ModeTuning fy4bLritTuning() { return fyTuning(235, "fy4blrit", "FY-4B LRIT", 1697.0, 2000000, 0.3, 250000); }       // family 233 + 2
ModeTuning fy4aHritTuning() { return fyTuning(236, "fy4ahrit", "FY-4A HRIT-II/III", 1679.0, 4000000, 1.5, 1500000); }   // family 234 + 2
ModeTuning fy2SvissrTuning() { return fyTuning(237, "fy2svissr", "FY-2 S-VISSR", 1687.5, 2400000, 1.0, 1000000); }   // family 235 + 2

} // namespace dect2
