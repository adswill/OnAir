// Bluetooth Classic discovery receiver: see btc_rx.h.
#include "dect2/btc/btc_rx.h"
#include "dect2/btc/btc_proto.h"
#include "dect2/btc/btc_tel.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <ctime>
#include <mutex>

namespace dect2 {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kOut = 4e6;               // demodulator rate: 4 samples per bit
constexpr double kChanHz = 1e6;
constexpr double kBinSpan = 450e3;         // bins within this of a channel's centre belong to it
constexpr float kThrDb = 10.f;             // a channel is active when it is this far over its floor
constexpr double kMinBurstSec = 40e-6, kCapSec = 500e-6, kMaxBurstSec = 3.3e-3;
constexpr int kWideChannels = 5;           // this many adjacent channels up at once: Wi-Fi or another wideband signal
constexpr size_t kRssiHist = 24;

} // namespace

struct BtcReceiver::Impl {
    std::mutex mu;
    double rateSet = 0, centerSet = 0;
    std::atomic<double> centerHz{0};
    bool resetReq = true, clearReq = false;
    std::function<void(const std::string&)> log;
    std::function<void(uint32_t, double, int, bool)> hitCb;

    double rate = 0;
    int N = 64;
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    std::vector<cf32> fbuf;
    int fpos = 0;
    struct Chan {
        int k = 0;                         // 0 to 78
        double fb = 0;                     // baseband frequency of its centre
        std::vector<int> bins;
        float floorP = 0;
        int state = 0;                     // 0 idle, 1 in a burst, 2 skipping until it goes quiet
        uint64_t start = 0, last = 0;
        int idle = 0;
    };
    std::vector<Chan> ch;
    std::vector<cf32> ring;
    size_t ringMask = 0;
    uint64_t total = 0, endSample = 0;
    uint64_t frames = 0;
    std::vector<float> taps;               // low-pass, time-reversed
    int L = 15;
    BtcTelemetry tel, pub;
    double lastPubSec = -1;
    double levelAcc = 0; size_t levelN = 0;
    double lastGoodSec = -100;

    // ---------------------------------------------------------------- setup

    void init() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; centerSet = centerHz.load(); }
        N = 32;
        while (N * 2 <= 256 && (double)N * 2 <= rate * 8e-6) N *= 2;
        fft = std::make_unique<Fft>(N);
        win.assign((size_t)N, 0.f);
        for (int i = 0; i < N; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / N));
        fbuf.assign((size_t)N, cf32());
        fpos = 0;
        const double c = centerSet > 0 ? centerSet : 2441e6;
        ch.clear();
        for (int k = 0; k < 79; k++) {
            Chan q;
            q.k = k; q.fb = (2402e6 + k * kChanHz) - c;
            if (std::fabs(q.fb) > rate / 2 - 0.6e6 && rate > 2 * kChanHz * 1.2) continue;   // the channel must lie inside the capture
            if (rate <= 2 * kChanHz * 1.2 && std::fabs(q.fb) > 1) continue;
            for (int b = 0; b < N; b++) {
                const double f = (b < N / 2 ? b : b - N) * rate / N;
                if (std::fabs(f - q.fb) <= kBinSpan) q.bins.push_back(b);
            }
            if (q.bins.empty()) continue;
            ch.push_back(q);
        }
        size_t rn = 1;
        while (rn < (size_t)(rate * 0.02) && rn < ((size_t)1 << 22)) rn <<= 1;
        ring.assign(rn, cf32());
        ringMask = rn - 1;
        total = 0; endSample = 0; frames = 0;
        L = std::max(15, (int)std::ceil(3.3 * rate / 0.9e6)) | 1;
        L = std::min(L, 257);
        taps.assign((size_t)L, 0.f);
        const double fc = 600e3 / rate;
        double sum = 0;
        for (int i = 0; i < L; i++) {
            const double m = i - (L - 1) / 2.0;
            const double s = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
            const double w = 0.54 - 0.46 * std::cos(2 * kPi * i / (L - 1));
            taps[(size_t)i] = (float)(s * w);
            sum += s * w;
        }
        for (float& t : taps) t = (float)(t / sum);
        std::reverse(taps.begin(), taps.end());
        BtcTelemetry keep;
        keep.devices = std::move(tel.devices); keep.log = std::move(tel.log);
        tel = BtcTelemetry();
        tel.devices = std::move(keep.devices); tel.log = std::move(keep.log);
        tel.inputRate = rate; tel.centerMhz = centerSet / 1e6; tel.channelsCovered = (int)ch.size();
        lastPubSec = -1; levelAcc = 0; levelN = 0; lastGoodSec = -100;
    }

    double sec() const { return rate > 0 ? (double)total / rate : 0; }

    // ---------------------------------------------------------------- detection

    void frame() {
        std::vector<cf32>& x = fbuf;
        double pw = 0;
        for (int i = 0; i < N; i++) { pw += std::norm(x[(size_t)i]); x[(size_t)i] *= win[(size_t)i]; }
        levelAcc += pw; levelN += (size_t)N;
        fft->forward(x.data());
        endSample = total;   // samples up to here are in the ring
        frames++;
        const bool learn = frames < 40;
        std::vector<float> P(ch.size());
        for (size_t c = 0; c < ch.size(); c++) {
            double s = 0;
            for (int b : ch[c].bins) s += std::norm(x[(size_t)b]);
            P[c] = (float)(s / ((double)N * N));
        }
        const float thr = std::pow(10.f, kThrDb / 10.f);
        std::vector<uint8_t> over(ch.size(), 0);
        for (size_t c = 0; c < ch.size(); c++) {
            Chan& q = ch[c];
            if (frames == 1) q.floorP = P[c];
            over[c] = !learn && q.floorP > 0 && P[c] > q.floorP * thr && P[c] > 1e-9f;
        }
        // wideband: a run of adjacent channels up at once and about equally strong is not Bluetooth (Wi-Fi). Around one strong narrow signal the
        // neighbours are only its leakage: they are skipped, the strong channel itself is analysed.
        std::vector<uint8_t> wide(ch.size(), 0), realWide(ch.size(), 0);
        for (size_t c = 0; c < ch.size();) {
            if (!over[c]) { c++; continue; }
            size_t e = c;
            while (e + 1 < ch.size() && over[e + 1] && ch[e + 1].k == ch[e].k + 1) e++;
            if (e > c) {
                size_t mx = c;
                for (size_t j = c; j <= e; j++) if (P[j] > P[mx]) mx = j;
                size_t lo = mx, hi = mx;
                while (lo > c && P[lo - 1] >= P[mx] * 0.03f) lo--;
                while (hi < e && P[hi + 1] >= P[mx] * 0.03f) hi++;
                const bool isWide = (int)(hi - lo + 1) >= kWideChannels;
                for (size_t j = c; j <= e; j++) if (isWide ? true : (j != mx)) { wide[j] = 1; realWide[j] = isWide; }   // leakage skirts and wideband alike are not analysed
                if (!isWide && wide[mx]) wide[mx] = 0;
            }
            c = e + 1;
        }
        for (size_t c = 0; c < ch.size(); c++) {
            Chan& q = ch[c];
            if (wide[c]) {
                if (q.state == 1) q.state = 2;
                if (q.state == 0) { q.state = 2; if (realWide[c] && (c == 0 || !realWide[c - 1])) tel.burstsWide++; }
                q.idle = 0;
                continue;
            }
            if (q.state == 0) {
                if (over[c]) { q.state = 1; q.start = endSample - (uint64_t)N; q.last = endSample; q.idle = 0; }
                else if (!learn) q.floorP += 0.02f * (P[c] - q.floorP);
                else q.floorP = 0.8f * q.floorP + 0.2f * P[c];
            } else if (q.state == 1) {
                if (over[c]) {
                    q.last = endSample; q.idle = 0;
                    if ((double)(q.last - q.start) / rate > kCapSec) { burst(q, q.start, q.start + (uint64_t)(kCapSec * rate), P[c], q.floorP); q.state = 2; }
                } else if (++q.idle >= 2) { q.state = 0; burst(q, q.start, q.last, P[c], q.floorP); }
            } else {
                if (!over[c]) { if (++q.idle >= 3) { q.state = 0; q.idle = 0; q.floorP += 0.5f * (P[c] - q.floorP); } }
                else q.idle = 0;
            }
        }
    }

    // ---------------------------------------------------------------- one burst

    void burst(const Chan& q, uint64_t s0, uint64_t s1, float pNow, float floorP) {
        (void)pNow;
        const double dur = (double)(s1 - s0) / rate;
        if (dur < kMinBurstSec || dur > kMaxBurstSec) return;
        const uint64_t pre = (uint64_t)(6e-6 * rate), post = (uint64_t)(6e-6 * rate);
        const uint64_t a = s0 > pre + (uint64_t)L ? s0 - pre - (uint64_t)L : 0;
        const uint64_t b = std::min(total, s1 + post + (uint64_t)L);
        if (total - a > ring.size() || b <= a + (uint64_t)L * 2) return;
        tel.burstsSeen++;
        const size_t len = (size_t)(b - a);
        // mix to 0 Hz
        std::vector<cf32> m(len);
        const double w = -2 * kPi * q.fb / rate;
        std::complex<double> ph(1, 0), st = std::polar(1.0, w);
        for (size_t i = 0; i < len; i++) {
            m[i] = ring[(a + i) & ringMask] * cf32((float)ph.real(), (float)ph.imag());
            ph *= st;
            if ((i & 1023) == 1023) ph /= std::abs(ph);
        }
        // filter and resample to 4 Msps
        const double step = rate / kOut;
        std::vector<cf32> y;
        for (double pos = 0; pos + L < (double)len; pos += step) {
            const cf32* p = &m[(size_t)pos];
            float re = 0, im = 0;
            for (int t = 0; t < L; t++) { re += taps[(size_t)t] * p[t].real(); im += taps[(size_t)t] * p[t].imag(); }
            y.push_back(cf32(re, im));
        }
        if (y.size() < 8 * 4 + 72 * 4) return;
        double pw = 0;
        std::vector<float> d(y.size(), 0.f);
        for (size_t i = 1; i < y.size(); i++) { d[i] = std::arg(y[i] * std::conj(y[i - 1])); }
        const size_t e0 = 12, e1 = y.size() > 12 ? y.size() - 12 : y.size();
        for (size_t i = e0; i < e1; i++) pw += std::norm(y[i]);
        pw /= (double)std::max<size_t>(1, e1 - e0);
        const float rssi = (float)(10 * std::log10(std::max(1e-20, pw)));
        const float snr = (float)(10 * std::log10(std::max(1e-20, pw) / std::max(1e-20, (double)floorP)));
        // four sample phases: the symbol value is the mean of the four discriminator samples of it
        btc::AcHit best; bool found = false; int bestPhase = 0;
        std::vector<uint8_t> bestBits;
        double bestCfo = 0;
        for (int ph4 = 0; ph4 < 4; ph4++) {
            std::vector<float> sv;
            for (size_t i = (size_t)ph4; i + 4 <= d.size(); i += 4) sv.push_back(0.25f * (d[i] + d[i + 1] + d[i + 2] + d[i + 3]));
            if (sv.size() < 80) continue;
            double mean = 0;
            size_t n0 = 4, n1 = sv.size() - 4;
            for (size_t i = n0; i < n1; i++) mean += sv[i];
            mean /= (double)(n1 - n0);
            std::vector<uint8_t> bits(sv.size());
            for (size_t i = 0; i < sv.size(); i++) bits[i] = sv[i] > (float)mean ? 1 : 0;
            btc::AcHit h;
            if (btc::findAccessCode(bits.data(), bits.size(), 0, 2, h) && (!found || h.errors < best.errors)) {
                found = true; best = h; bestPhase = ph4; bestBits = bits;
                bestCfo = mean * kOut / (2 * kPi);
            }
        }
        (void)bestPhase;
        const double tSec = (double)s0 / rate;
        if (!found) { tel.blocksBad++; return; }
        tel.blocksOk++;
        tel.cfoHz = bestCfo; tel.snrDb = snr; tel.dataValid = true;
        lastGoodSec = tSec;
        const double mhz = 2402.0 + q.k;
        btc::Fhs f;
        bool fhs = false;
        if (best.lap == btc::kGiac && best.trailer) fhs = btc::decodeFhs(bestBits.data() + best.pos + 72, bestBits.size() - best.pos - 72, f);
        if (hitCb) hitCb(fhs ? f.lap : best.lap, mhz, best.errors, fhs);
        note(best, f, fhs, mhz, rssi, tSec, q.k);
    }

    void note(const btc::AcHit& h, const btc::Fhs& f, bool fhs, double mhz, float rssi, double tSec, int chanK) {
        const int64_t now = (int64_t)std::time(nullptr);
        const uint32_t lap = fhs ? f.lap : h.lap;
        BtcDevice* d = nullptr;
        for (auto& e : tel.devices) if (e.lap == lap) { d = &e; break; }
        if (!d) {
            BtcDevice nd;
            nd.lap = lap; nd.lapText = btc::lapText(lap); nd.firstSec = tSec; nd.firstWall = now;
            tel.devices.insert(tel.devices.begin(), nd);
            d = &tel.devices.front();
            if (tel.devices.size() > kBtcDevMax) tel.devices.pop_back();
        }
        d->packets++;
        d->rssi = rssi; d->lastSec = tSec; d->lastWall = now; d->lastMhz = mhz;
        d->rssiHist.push_back(rssi);
        if (d->rssiHist.size() > kRssiHist) d->rssiHist.erase(d->rssiHist.begin());
        if (chanK < 64) d->chanLo |= 1ull << chanK; else d->chanHi |= 1ull << (chanK - 64);
        const char* ak = btc::acKind(h.lap);
        std::string info;
        if (fhs) {
            d->hasFhs = true; d->addr = btc::bdAddr(f); d->cod = btc::codText(f.cod); d->major = btc::codMajor(f.cod); d->codRaw = f.cod; d->clk = f.clk;
            d->kind = "discoverable device";
            tel.fhsCount++;
            info = "inquiry response: " + d->addr + ", " + d->cod;
        } else if (*ak) {
            d->kind = ak;
            tel.giacIds++;
            info = std::string("ID packet, ") + ak;
        } else {
            if (!d->hasFhs) d->kind = "device or piconet master";
            info = "access code of LAP " + d->lapText;
        }
        BtcPacket p;
        p.wallTime = now; p.sec = tSec; p.mhz = mhz; p.lap = lap; p.rssi = rssi; p.errors = h.errors; p.info = info;
        tel.log.insert(tel.log.begin(), p);
        if (tel.log.size() > kBtcLogMax) tel.log.pop_back();
        if (this->log) { char b[200]; snprintf(b, sizeof b, "%.0f MHz  %s", mhz, info.c_str()); this->log(b); }
    }

    // ---------------------------------------------------------------- driver

    void report() {
        tel.seq++;
        tel.timeSec = sec();
        tel.state = tel.timeSec - lastGoodSec < 5 ? 1 : 0;
        tel.levelDb = levelN ? (float)(10 * std::log10(std::max(1e-20, levelAcc / (double)levelN))) : -200.f;
        levelAcc = 0; levelN = 0;
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }

    void feedBlock(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            ring[total & ringMask] = x[i];
            total++;
            fbuf[(size_t)fpos++] = x[i];
            if (fpos == N) { fpos = 0; frame(); }
        }
        if (lastPubSec < 0 || sec() - lastPubSec >= 0.25) { lastPubSec = sec(); report(); }
    }
};

BtcReceiver::BtcReceiver() : p_(std::make_unique<Impl>()) {}
BtcReceiver::~BtcReceiver() = default;
void BtcReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void BtcReceiver::setSignalOffset(double) {}
void BtcReceiver::setCenterHz(double hz) { p_->centerHz = hz; std::lock_guard<std::mutex> lk(p_->mu); p_->resetReq = true; }
bool BtcReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= btcTuning().minSampleRate - 1; }
void BtcReceiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); p_->resetReq = true; }
void BtcReceiver::clearDevices() { clearResults(); }
void BtcReceiver::clearResults() { std::lock_guard<std::mutex> lk(p_->mu); p_->clearReq = true; }
void BtcReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void BtcReceiver::setHitCallback(std::function<void(uint32_t, double, int, bool)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->hitCb = std::move(cb); }

void BtcReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    bool rs, cl;
    { std::lock_guard<std::mutex> lk(m.mu); rs = m.resetReq; cl = m.clearReq; m.resetReq = false; m.clearReq = false; }
    if (cl) { m.tel.devices.clear(); m.tel.log.clear(); m.tel.giacIds = m.tel.fhsCount = m.tel.blocksOk = m.tel.blocksBad = 0; }
    if (rs) { m.init(); }
    if (m.rate < btcTuning().minSampleRate - 1 || n == 0 || m.ch.empty()) {
        if (m.rate > 0 && n) { m.total += n; if (m.lastPubSec < 0 || m.sec() - m.lastPubSec >= 0.25) { m.lastPubSec = m.sec(); m.report(); } }
        return;
    }
    m.feedBlock(x, n);
}

bool BtcReceiver::telemetry(BtcTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

ModeTuning btcTuning() {
    ModeTuning t;
    t.stdMode = 382; t.id = "btc"; t.name = "Bluetooth Classic";
    t.minMhz = 2400; t.maxMhz = 2484; t.defMhz = 2441;
    t.sampleRate = 20000000;
    t.basebandHz = 0;
    t.bandwidthMhz = 20;
    t.minSampleRate = 4000000;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
