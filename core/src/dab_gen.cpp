// DAB test transmitter (see dab_gen.h): the ensemble, the FIC, the CIF multiplex, the OFDM frame and the synthetic source around it.
#include "dect2/dab_gen.h"
#include "dect2/fftutil.h"
#include "dect2/gen_util.h"
#include "dect2/isdbt_resample.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>

namespace dect2 {
namespace dabgen {

namespace {

int64_t floorMod(int64_t a, int64_t b) { const int64_t r = a % b; return r < 0 ? r + b : r; }

// ---------------------------------------------------------------- the test sounds
struct Note { double hz, len; };      // frequency and length in quarter notes: the first bars of Ode to Joy, twice
const Note kTune[] = {
    {329.63, 1}, {329.63, 1}, {349.23, 1}, {392.00, 1}, {392.00, 1}, {349.23, 1}, {329.63, 1}, {293.66, 1},
    {261.63, 1}, {261.63, 1}, {293.66, 1}, {329.63, 1}, {329.63, 1.5}, {293.66, 0.5}, {293.66, 2},
    {329.63, 1}, {329.63, 1}, {349.23, 1}, {392.00, 1}, {392.00, 1}, {349.23, 1}, {329.63, 1}, {293.66, 1},
    {261.63, 1}, {261.63, 1}, {293.66, 1}, {329.63, 1}, {293.66, 1.5}, {261.63, 0.5}, {261.63, 2},
};
constexpr double kQuarter = 0.375;     // seconds: 32 quarter notes make the 12 s loop

double melody(double t, double octave) {       // t in seconds within the loop
    double start = 0;
    const Note* cur = &kTune[0];
    for (const Note& n : kTune) { if (t < start + n.len * kQuarter) { cur = &n; break; } start += n.len * kQuarter; }
    const double pos = t - start, len = cur->len * kQuarter;
    double env = std::min(1.0, pos / 0.012) * (0.7 + 0.3 * std::exp(-pos / 0.12));
    if (len - pos < 0.08) env *= std::max(0.0, (len - pos) / 0.08);
    const double ph = 2 * M_PI * cur->hz * octave * pos;
    const double v = std::sin(ph) + 0.45 * std::sin(2 * ph) + 0.2 * std::sin(3 * ph) + 0.08 * std::sin(4 * ph);
    return env * v / 1.73;
}

int loopSamples(const TxService& s) { return kLoopSeconds * s.sampleRate; }

} // namespace

float testSound(const TxService& s, int ch, int64_t i) {
    const int64_t loop = loopSamples(s);
    const int64_t n = floorMod(i, loop);
    if (s.melody) return (float)(s.amplitude * melody((double)n / s.sampleRate, ch ? 2.0 : 1.0));
    const double f = ch ? s.rightHz : s.leftHz;
    return (float)(s.amplitude * std::sin(2 * M_PI * std::fmod(f * (double)n, (double)s.sampleRate) / s.sampleRate));
}

// ---------------------------------------------------------------- carrier tables
const std::vector<int>& frequencyInterleaver() {
    static const std::vector<int> t = [] {
        std::vector<int> out;
        int p = 0;
        for (int i = 0; i < kTu; i++) {
            if (i) p = (13 * p + 511) % kTu;
            if (p >= 256 && p <= 1792 && p != 1024) out.push_back(p - 1024);
        }
        return out;
    }();
    return t;
}

const std::vector<cf32>& phaseReferenceSymbol() {
    static const std::vector<cf32> ref = [] {
        static const int h[4][32] = {
            {0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1, 0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1},
            {0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0, 0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0},
            {0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3, 0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3},
            {0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2, 0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2}};
        // table 23: carriers kmin .. kmin+31, row index i, parameter n
        static const int rows[48][3] = {
            {-768, 0, 1}, {-736, 1, 2}, {-704, 2, 0}, {-672, 3, 1}, {-640, 0, 3}, {-608, 1, 2}, {-576, 2, 2}, {-544, 3, 3},
            {-512, 0, 2}, {-480, 1, 1}, {-448, 2, 2}, {-416, 3, 3}, {-384, 0, 1}, {-352, 1, 2}, {-320, 2, 3}, {-288, 3, 3},
            {-256, 0, 2}, {-224, 1, 2}, {-192, 2, 2}, {-160, 3, 1}, {-128, 0, 1}, {-96, 1, 3}, {-64, 2, 1}, {-32, 3, 2},
            {1, 0, 3}, {33, 3, 1}, {65, 2, 1}, {97, 1, 1}, {129, 0, 2}, {161, 3, 2}, {193, 2, 1}, {225, 1, 0},
            {257, 0, 2}, {289, 3, 2}, {321, 2, 3}, {353, 1, 3}, {385, 0, 0}, {417, 3, 2}, {449, 2, 1}, {481, 1, 3},
            {513, 0, 3}, {545, 3, 3}, {577, 2, 3}, {609, 1, 0}, {641, 0, 3}, {673, 3, 0}, {705, 2, 1}, {737, 1, 1}};
        static const cf32 rot[4] = {cf32(1, 0), cf32(0, 1), cf32(-1, 0), cf32(0, -1)};     // exp(j pi/2 m)
        std::vector<cf32> z((size_t)kTu, cf32(0, 0));
        for (const auto& r : rows)
            for (int j = 0; j < 32; j++) z[(size_t)((r[0] + j + kTu) % kTu)] = rot[(h[r[1]][j] + r[2]) & 3];
        return z;
    }();
    return ref;
}

// ---------------------------------------------------------------- the services
std::vector<TxService> defaultServices() {
    std::vector<TxService> v;
    TxService a;
    a.sid = 0xCE01; a.label = "OnAir Tones 1"; a.subId = 1; a.bitrate = 48; a.sampleRate = 48000; a.leftHz = 1000; a.rightHz = 3000;
    v.push_back(a);
    TxService b;
    b.sid = 0xCE02; b.label = "OnAir Tones 2"; b.subId = 2; b.bitrate = 32; b.sampleRate = 32000; b.leftHz = 2000; b.rightHz = 500;
    v.push_back(b);
    TxService c;
    c.sid = 0xCE03; c.label = "OnAir Melody"; c.subId = 3; c.bitrate = 64; c.sampleRate = 48000; c.melody = true;
    v.push_back(c);
    if (avcodec_find_encoder_by_name("mp2")) {
        TxService d;
        d.sid = 0xCE04; d.label = "OnAir MP2"; d.subId = 4; d.bitrate = 128; d.dabPlus = false; d.sampleRate = 48000; d.leftHz = 1500; d.rightHz = 750;
        v.push_back(d);
    }
    return v;
}

// ---------------------------------------------------------------- the transmitter
struct Transmitter::Impl {
    TxConfig cfg;
    std::vector<SubLayout> lay;
    struct Sub {
        TxService svc;
        int nAu = 0, loopLen = 0, frameBytes = 0, bitsPerFrame = 0;
        std::vector<std::vector<uint8_t>> aus, mp2;
        std::vector<std::vector<uint8_t>> frames;     // the logical frames of the loop (bytes)
        std::vector<uint8_t> coded;                   // loopLen * bitsPerFrame bits, one per byte
    };
    std::vector<Sub> subs;
    uint64_t frame = 0;
    int64_t utc0 = 0;
    Fft fft{kTu};
    float scale = 0.f;

    // ---- audio -> logical frames
    static void bytesToBits(const uint8_t* b, int n, uint8_t* bits) { for (int i = 0; i < n; i++) for (int k = 0; k < 8; k++) bits[i * 8 + k] = (b[i] >> (7 - k)) & 1; }

    void codeFrames(Sub& s) {
        s.loopLen = (int)s.frames.size();
        s.frameBytes = s.svc.bitrate * 3;
        const int size = eepSize(s.svc.bitrate, s.svc.option, s.svc.level);
        s.bitsPerFrame = size * 64;
        s.coded.assign((size_t)s.loopLen * (size_t)s.bitsPerFrame, 0);
        const int info = s.frameBytes * 8;
        std::vector<uint8_t> bits((size_t)info), mother((size_t)(4 * (info + 6)));
        for (int f = 0; f < s.loopLen; f++) {
            bytesToBits(s.frames[(size_t)f].data(), s.frameBytes, bits.data());
            scramble(bits.data(), info);
            convEncode(bits.data(), info, mother.data());
            punctureEep(mother.data(), s.svc.bitrate, s.svc.option, s.svc.level, &s.coded[(size_t)f * (size_t)s.bitsPerFrame]);
        }
    }

    static int toneFrames(const TxService& sv, int frameLen) {
        // the number of frames after which the tones repeat: the least common multiple of the periods and the frame length
        const double hz[2] = {sv.leftHz, sv.rightHz};
        int64_t per = 1;
        for (double f : hz) {
            const int64_t fi = (int64_t)std::llround(f);
            if (std::fabs(f - (double)fi) > 1e-9 || fi <= 0) return 0;
            per = std::lcm(per, (int64_t)sv.sampleRate / std::gcd((int64_t)sv.sampleRate, fi));
        }
        return (int)(std::lcm(per, (int64_t)frameLen) / frameLen);
    }

    void buildAac(Sub& s) {
        const TxService& sv = s.svc;
        s.nAu = accessUnitsPerSuperframe(sv.sampleRate);
        const int K = sv.melody ? loopSamples(sv) / 960 : std::max(1, toneFrames(sv, 960) ? toneFrames(sv, 960) : loopSamples(sv) / 960);
        const int Kp = (int)std::lcm((int64_t)K, (int64_t)s.nAu);       // access units in the loop
        const int S = Kp / s.nAu;
        std::vector<int> len;
        int fs = 0;
        superframeLayout(sv.bitrate, s.nAu, &fs, &len);
        AacLcEncoder enc(sv.sampleRate, 2);
        std::vector<float> pcm(960 * 2);
        auto block = [&](int64_t j) { for (int i = 0; i < 960; i++) for (int c = 0; c < 2; c++) pcm[(size_t)i * 2 + (size_t)c] = testSound(sv, c, floorMod(j, K) * 960 + i); };
        std::vector<uint8_t> raw;
        block(Kp - 1);                                    // the block before the loop starts (the loop is periodic)
        enc.encode(pcm.data(), len[0] - 2, raw);
        s.aus.clear();
        for (int j = 0; j < Kp; j++) {
            block(j);
            const int budget = len[(size_t)(j % s.nAu)] - 2;
            enc.encode(pcm.data(), budget, raw);
            const uint16_t crc = crc16(raw.data(), budget);
            raw.push_back((uint8_t)(crc >> 8)); raw.push_back((uint8_t)crc);
            s.aus.push_back(raw);
        }
        // the AUs without the CRC for the tests
        std::vector<std::vector<uint8_t>> withCrc = s.aus;
        for (auto& a : s.aus) a.resize(a.size() - 2);
        s.frames.clear();
        const int fb = s.svc.bitrate * 3;
        for (int sf = 0; sf < S; sf++) {
            std::vector<std::vector<uint8_t>> au(withCrc.begin() + (long)sf * s.nAu, withCrc.begin() + (long)(sf + 1) * s.nAu);
            const std::vector<uint8_t> bytes = buildSuperframe(au, sv.bitrate, sv.sampleRate == 48000, true);
            for (int q = 0; q < 5; q++) s.frames.emplace_back(bytes.begin() + (long)q * fb, bytes.begin() + (long)(q + 1) * fb);
        }
    }

    bool buildMp2(Sub& s) {
        const TxService& sv = s.svc;
        const AVCodec* codec = avcodec_find_encoder_by_name("mp2");
        if (!codec) return false;
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        ctx->sample_fmt = AV_SAMPLE_FMT_S16;
        ctx->sample_rate = sv.sampleRate;
        ctx->bit_rate = (int64_t)sv.bitrate * 1000;
        av_channel_layout_default(&ctx->ch_layout, 2);
        if (avcodec_open2(ctx, codec, nullptr) < 0) { avcodec_free_context(&ctx); return false; }
        const int fl = ctx->frame_size;                   // 1152
        AVFrame* fr = av_frame_alloc();
        fr->nb_samples = fl; fr->format = AV_SAMPLE_FMT_S16; fr->sample_rate = sv.sampleRate;
        av_channel_layout_default(&fr->ch_layout, 2);
        av_frame_get_buffer(fr, 0);
        AVPacket* pk = av_packet_alloc();
        const int loopFrames = loopSamples(sv) / fl;      // 500
        std::vector<std::vector<uint8_t>> out;
        // two passes over the loop: the second one starts with the filter bank in the state the end of the loop leaves it in
        for (int t = 0; t < 2 * loopFrames; t++) {
            av_frame_make_writable(fr);
            int16_t* d = (int16_t*)fr->data[0];
            for (int i = 0; i < fl; i++) for (int c = 0; c < 2; c++) d[i * 2 + c] = (int16_t)std::lround(testSound(sv, c, (int64_t)t * fl + i) * 32767.0);
            fr->pts = (int64_t)t * fl;
            if (avcodec_send_frame(ctx, fr) < 0) break;
            while (avcodec_receive_packet(ctx, pk) == 0) { out.emplace_back(pk->data, pk->data + pk->size); av_packet_unref(pk); }
        }
        av_packet_free(&pk); av_frame_free(&fr); avcodec_free_context(&ctx);
        if ((int)out.size() < loopFrames + 2) return false;
        // the packets lag the input by the encoder delay; take loopFrames consecutive ones from the second pass
        s.mp2.assign(out.end() - loopFrames, out.end());
        s.frames.clear();
        for (auto& p : s.mp2) {
            if ((int)p.size() != sv.bitrate * 3) return false;
            if (p.size() >= 2) { p[p.size() - 2] = 0; p[p.size() - 1] = 0; }       // F-PAD: no X-PAD
            s.frames.push_back(p);
        }
        return true;
    }

    explicit Impl(const TxConfig& c) : cfg(c) {
        if (cfg.services.empty()) cfg.services = defaultServices();
        scale = (float)(0.2 / std::sqrt((double)kCarriers));
        utc0 = cfg.utcSeconds >= 0 ? cfg.utcSeconds : (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        int start = 0;
        std::vector<TxService> kept;
        for (const TxService& sv : cfg.services) {
            Sub s;
            s.svc = sv;
            const int size = eepSize(sv.bitrate, sv.option, sv.level);
            if (!size || start + size > kCifCu) continue;
            if (sv.dabPlus) { if (sv.sampleRate != 48000 && sv.sampleRate != 32000) continue; buildAac(s); }
            else if (!buildMp2(s)) continue;
            codeFrames(s);
            lay.push_back({sv.subId, start, size, sv.bitrate, sv.option, sv.level});
            start += size;
            kept.push_back(sv);
            subs.push_back(std::move(s));
        }
        cfg.services = kept;
    }

    // ---- FIC
    static void put16(std::vector<uint8_t>& v, unsigned x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
    static std::vector<uint8_t> fig(int type, const std::vector<uint8_t>& body) { std::vector<uint8_t> f; f.push_back((uint8_t)((type << 5) | (int)body.size())); f.insert(f.end(), body.begin(), body.end()); return f; }
    static void labelBytes(std::vector<uint8_t>& b, const std::string& l) {
        for (int i = 0; i < 16; i++) b.push_back(i < (int)l.size() ? (uint8_t)l[(size_t)i] : (uint8_t)' ');
        b.push_back(0xFF); b.push_back(0x00);               // character flag field: the first 8 characters make the short label
    }

    std::vector<uint8_t> fig00(uint64_t cif) const {
        std::vector<uint8_t> b{0x00};
        put16(b, cfg.eid);
        const unsigned c = (unsigned)(cif % 5000);
        b.push_back((uint8_t)(c / 250));                    // change flags 00, alarm flag 0, CIF count high (0..19)
        b.push_back((uint8_t)(c % 250));                    // CIF count low (0..249)
        return fig(0, b);
    }
    std::vector<uint8_t> fig07() const {
        std::vector<uint8_t> b{0x07};
        put16(b, ((unsigned)subs.size() << 10) | 0);        // services (6 bits), reconfiguration count (10 bits)
        return fig(0, b);
    }
    std::vector<uint8_t> fig010(uint64_t frameIdx) const {
        const int64_t ms = utc0 * 1000 + (int64_t)frameIdx * 96;
        const int64_t sec = ms / 1000;
        const int msec = (int)(ms % 1000);
        const int64_t days = sec / 86400, rem = sec % 86400;
        const uint32_t mjd = (uint32_t)(days + 40587);
        const int hh = (int)(rem / 3600), mm = (int)(rem / 60 % 60), ss = (int)(rem % 60);
        // rfa, MJD (17), LSI, confidence, UTC flag (long form), hours (5), minutes (6), seconds (6), milliseconds (10)
        uint64_t v = 0;
        v = (v << 1) | 0; v = (v << 17) | mjd; v = (v << 1) | 0; v = (v << 1) | 0; v = (v << 1) | 1;
        v = (v << 5) | (uint64_t)hh; v = (v << 6) | (uint64_t)mm; v = (v << 6) | (uint64_t)ss; v = (v << 10) | (uint64_t)msec;
        std::vector<uint8_t> b{0x0A};
        for (int i = 5; i >= 0; i--) b.push_back((uint8_t)(v >> (8 * i)));
        return fig(0, b);
    }
    std::vector<std::vector<uint8_t>> fig01() const {       // sub-channel organisation, long form, 7 per FIG
        std::vector<std::vector<uint8_t>> out;
        for (size_t i = 0; i < lay.size(); i += 7) {
            std::vector<uint8_t> b{0x01};
            for (size_t k = i; k < std::min(lay.size(), i + 7); k++) {
                const SubLayout& l = lay[k];
                b.push_back((uint8_t)((l.subId << 2) | (l.start >> 8))); b.push_back((uint8_t)l.start);
                b.push_back((uint8_t)(0x80 | (l.option << 4) | (l.level << 2) | (l.size >> 8))); b.push_back((uint8_t)l.size);
            }
            out.push_back(fig(0, b));
        }
        return out;
    }
    std::vector<std::vector<uint8_t>> fig02() const {       // service organisation, one audio stream component per service, 5 per FIG
        std::vector<std::vector<uint8_t>> out;
        for (size_t i = 0; i < subs.size(); i += 5) {
            std::vector<uint8_t> b{0x02};
            for (size_t k = i; k < std::min(subs.size(), i + 5); k++) {
                const TxService& sv = subs[k].svc;
                put16(b, sv.sid);
                b.push_back(0x01);                                                  // local flag 0, 1 service component
                b.push_back((uint8_t)(sv.dabPlus ? 63 : 0));                        // TMID 0 (MSC stream audio), ASCTy
                b.push_back((uint8_t)((sv.subId << 2) | 0x02));                     // SubChId, primary, no conditional access
            }
            out.push_back(fig(0, b));
        }
        return out;
    }
    std::vector<uint8_t> figLabel(size_t idx) const {       // 0: the ensemble, 1..: the services
        std::vector<uint8_t> b;
        if (idx == 0) { b.push_back(0x00); put16(b, cfg.eid); labelBytes(b, cfg.ensembleLabel); }
        else { b.push_back(0x01); put16(b, subs[idx - 1].svc.sid); labelBytes(b, subs[idx - 1].svc.label); }
        return fig(1, b);
    }

    void makeFibs(uint64_t m, uint8_t out[12][32]) const {
        std::vector<uint8_t> fib[12];
        auto place = [&](int from, const std::vector<uint8_t>& f) {
            for (int i = from; i < 12; i++) if (fib[i].size() + f.size() <= 30) { fib[i].insert(fib[i].end(), f.begin(), f.end()); return true; }
            return false;
        };
        place(0, fig00(4 * m));
        place(0, fig07());
        place(0, fig010(m));
        for (const auto& f : fig01()) place(1, f);
        for (const auto& f : fig02()) place(1, f);
        const size_t nl = subs.size() + 1;
        for (size_t k = 0; k < 3; k++) place(1, figLabel((size_t)((m * 3 + k) % nl)));
        for (int i = 0; i < 12; i++) {
            std::memset(out[i], 0, 32);
            std::memcpy(out[i], fib[i].data(), fib[i].size());
            if (fib[i].size() < 30) out[i][fib[i].size()] = 0xFF;
            const uint16_t c = crc16(out[i], 30);
            out[i][30] = (uint8_t)(c >> 8); out[i][31] = (uint8_t)c;
        }
    }

    // ---- one frame in the frequency domain: kSymbols x kTu bins
    void makeSymbols(uint64_t m, std::vector<cf32>& z) {
        // bits of the 75 data symbols
        std::vector<uint8_t> bits((size_t)(kSymbols - 1) * 3072, 0);
        uint8_t fibs[12][32];
        makeFibs(m, fibs);
        for (int c = 0; c < 4; c++) {                                  // the FIC: 3 FIBs per CIF, scrambled, coded, punctured
            uint8_t in[768], mother[4 * (768 + 6)], enc[2304];
            for (int i = 0; i < 96; i++) for (int k = 0; k < 8; k++) in[i * 8 + k] = (fibs[3 * c + i / 32][i % 32] >> (7 - k)) & 1;
            scramble(in, 768);
            convEncode(in, 768, mother);
            punctureFic(mother, enc);
            std::memcpy(&bits[(size_t)c * 2304], enc, 2304);
        }
        static const int delay[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};     // table 21
        static const std::vector<uint8_t> fill = [] { std::vector<uint8_t> s(511); prbs(s.data(), 511); return s; }();
        uint8_t* msc = &bits[(size_t)3 * 3072];
        for (int c = 0; c < 4; c++) {
            const int64_t n = (int64_t)(4 * m) + c;
            uint8_t* cif = msc + (size_t)c * kCifBits;
            for (int i = 0; i < kCifBits; i++) cif[i] = fill[(size_t)((i + 37 * (n % 511)) % 511)];   // capacity not used by a service: noise-like padding
            for (size_t si = 0; si < subs.size(); si++) {
                const Sub& s = subs[si];
                uint8_t* dst = cif + (size_t)lay[si].start * 64;
                for (int i = 0; i < s.bitsPerFrame; i++) {
                    const int64_t f = floorMod(n - delay[i & 15], s.loopLen);
                    dst[i] = s.coded[(size_t)f * (size_t)s.bitsPerFrame + (size_t)i];
                }
            }
        }
        // DQPSK: the phase reference symbol, then every symbol is the one before times the QPSK symbol of its bits
        z.assign((size_t)kSymbols * kTu, cf32(0, 0));
        const auto& ref = phaseReferenceSymbol();
        std::copy(ref.begin(), ref.end(), z.begin());
        const auto& fi = frequencyInterleaver();
        const float r = (float)M_SQRT1_2;
        for (int l = 1; l < kSymbols; l++) {
            const uint8_t* p = &bits[(size_t)(l - 1) * 3072];
            const cf32* prev = &z[(size_t)(l - 1) * kTu];
            cf32* cur = &z[(size_t)l * kTu];
            for (int n = 0; n < kCarriers; n++) {
                const int bin = (fi[(size_t)n] + kTu) % kTu;
                const cf32 q(r * (1 - 2 * (int)p[n]), r * (1 - 2 * (int)p[n + kCarriers]));
                cur[bin] = prev[bin] * q;
            }
        }
    }

    void nextFrame(std::vector<cf32>& out) {
        std::vector<cf32> z;
        makeSymbols(frame, z);
        out.assign((size_t)kFrame, cf32(0, 0));
        std::vector<cf32> t((size_t)kTu);
        for (int l = 0; l < kSymbols; l++) {
            std::copy(z.begin() + (long)l * kTu, z.begin() + (long)(l + 1) * kTu, t.begin());
            fft.inverse(t.data());
            cf32* dst = &out[(size_t)kTnull + (size_t)l * kTs];
            for (int i = 0; i < kTg; i++) dst[i] = t[(size_t)(kTu - kTg + i)] * scale;
            for (int i = 0; i < kTu; i++) dst[kTg + i] = t[(size_t)i] * scale;
        }
        frame++;
    }
};

Transmitter::Transmitter(const TxConfig& cfg) : p_(new Impl(cfg)) {}
Transmitter::~Transmitter() = default;
void Transmitter::nextFrame(std::vector<cf32>& out) { p_->nextFrame(out); }
uint64_t Transmitter::frameIndex() const { return p_->frame; }
const TxConfig& Transmitter::config() const { return p_->cfg; }
const std::vector<SubLayout>& Transmitter::layout() const { return p_->lay; }
void Transmitter::fibs(uint64_t m, uint8_t out[12][32]) const { p_->makeFibs(m, out); }
void Transmitter::logicalFrame(int service, int64_t f, std::vector<uint8_t>& out) const {
    const Impl::Sub& s = p_->subs[(size_t)service];
    out = s.frames[(size_t)floorMod(f, s.loopLen)];
}
const std::vector<std::vector<uint8_t>>& Transmitter::accessUnits(int service) const { return p_->subs[(size_t)service].aus; }
const std::vector<std::vector<uint8_t>>& Transmitter::mp2Frames(int service) const { return p_->subs[(size_t)service].mp2; }
const uint8_t* Transmitter::codedBits(int service, int64_t f) const {
    const Impl::Sub& s = p_->subs[(size_t)service];
    return &s.coded[(size_t)floorMod(f, s.loopLen) * (size_t)s.bitsPerFrame];
}
void Transmitter::symbols(uint64_t m, std::vector<cf32>& z) const { p_->makeSymbols(m, z); }

} // namespace dabgen

// ---------------------------------------------------------------- the synthetic source
namespace {

// One stage of the sample rate conversion: the rational polyphase resampler when the ratio allows exact phases, else the general one (which can also
// follow a clock error). Both take ratios up to 4, longer chains double the rate first.
struct Stage {
    bool poly = false;
    isdbt::PolyResampler p;
    isdbt::TrackingResampler t;
    void process(const cf32* in, size_t n, std::vector<cf32>& out) { if (poly) p.process(in, n, out); else t.process(in, n, out); }
};

class DabSynth : public ModeSynth {
public:
    DabSynth(const dabgen::TxConfig& tc, const SynthConfig& c, double rate) : rate_(rate), cfo_(c.cfoHz), noise_(0xDAB) {
        dph_ = 2 * M_PI * cfo_ / rate_;
        for (int k = 0; k < 128; k++) { tabR_[k] = (float)std::cos(dph_ * k); tabI_[k] = (float)std::sin(dph_ * k); }
        tx_ = std::make_unique<dabgen::Transmitter>(tc);
        // noise relative to the signal power of the active symbols (rms 0.2): C/N in the 2.048 MHz band
        sigma_ = c.snrDb >= 90 ? 0.f : (float)std::sqrt(0.04 * std::pow(10.0, -c.snrDb / 10.0) / 2.0);
        // A clock error stretches the time axis: it is applied where it is cheap, on the 2.048 Msps signal (a filter of 32 taps at a ratio of 1), and then
        // the exact rational conversion to the nominal output rate follows. The output sample j then sits at j / (rate * (1 + ppm)) of the original signal.
        if (c.sroPpm != 0) {
            auto st = std::make_unique<Stage>();
            st->t.configure(dabgen::kRate, dabgen::kRate * (1 + 1e-7));         // an exact ratio of 1 would be a plain copy
            st->t.scaleStep((1.0 / (1.0 + c.sroPpm * 1e-6)) / st->t.step());
            stages_.push_back(std::move(st));
        }
        double cur = dabgen::kRate;
        for (;;) {
            const bool last = rate / cur <= 4.0;
            const double target = last ? rate : cur * 2;
            if (last && std::fabs(target - cur) < 1e-6) break;
            auto st = std::make_unique<Stage>();
            st->poly = st->p.configure(cur, target);
            if (!st->poly) st->t.configure(cur, target);
            stages_.push_back(std::move(st));
            cur = target;
            if (last) break;
        }
    }
    double sampleRate() const override { return rate_; }
    // GCC fuses the multiply-adds of the carrier rotation differently in its vector loop and in the scalar remainder (on arm64),
    // which made the output depend on the chunk size: no fusing here (clang only fuses within an expression, the same way in both)
#if defined(__GNUC__) && !defined(__clang__)
    __attribute__((optimize("fp-contract=off")))
#endif
    void generate(cf32* out, size_t n) override {
        while (pending_.size() - pos_ < n) produce();
        const cf32* src = &pending_[pos_];
        const float lim2 = 0.9f * 0.9f;
        size_t i = 0;
        while (i < n) {
            // blocks that end at multiples of 128 samples of the stream, so that the chunk size does not matter
            const size_t run = std::min<size_t>(n - i, 128 - (size_t)(count_ & 127));
            if (cfo_ != 0) {
                // carrier offset: the phasor of the block start, computed exactly, times the table of the 128 steps
                const double ph = std::remainder(dph_ * (double)(count_ & ~(uint64_t)127), 2 * M_PI);
                const float br = (float)std::cos(ph), bi = (float)std::sin(ph);
                const size_t k0 = (size_t)(count_ & 127);
                for (size_t k = 0; k < run; k++) {
                    const float pr = br * tabR_[k0 + k] - bi * tabI_[k0 + k], pi = br * tabI_[k0 + k] + bi * tabR_[k0 + k];
                    const float sr = src[i + k].real(), si = src[i + k].imag();
                    out[i + k] = cf32(sr * pr - si * pi, sr * pi + si * pr);
                }
            } else std::copy(src + i, src + i + run, out + i);
            for (size_t k = 0; k < run; k++) {         // the rare peaks of the OFDM signal
                const float p = std::norm(out[i + k]);
                if (p > lim2) out[i + k] *= 0.9f / std::sqrt(p);
            }
            i += run;
            count_ += run;
        }
        pos_ += n;
        if (pos_ > (1u << 20)) { pending_.erase(pending_.begin(), pending_.begin() + (long)pos_); pos_ = 0; }
    }
private:
    void produce() {
        tx_->nextFrame(a_);
        if (sigma_ > 0) noise_.add(a_.data(), a_.size(), sigma_);
        std::vector<cf32>* cur = &a_;
        std::vector<cf32>* oth = &b_;
        for (auto& st : stages_) {
            oth->clear();
            st->process(cur->data(), cur->size(), *oth);
            std::swap(cur, oth);
        }
        pending_.insert(pending_.end(), cur->begin(), cur->end());
    }
    double rate_, cfo_;
    float sigma_ = 0;
    std::unique_ptr<dabgen::Transmitter> tx_;
    std::vector<std::unique_ptr<Stage>> stages_;
    genutil::NoiseSource noise_;
    std::vector<cf32> a_, b_, pending_;
    size_t pos_ = 0;
    uint64_t count_ = 0;
    double dph_ = 0;
    float tabR_[128], tabI_[128];
};

} // namespace

std::unique_ptr<ModeSynth> makeDabSynth(const dabgen::TxConfig& tx, const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 2e6 || sampleRate > 21e6) return nullptr;
    return std::make_unique<DabSynth>(tx, cfg, sampleRate);
}

std::unique_ptr<ModeSynth> makeDabSynth(const SynthConfig& cfg, double sampleRate) { return makeDabSynth(dabgen::TxConfig(), cfg, sampleRate); }

} // namespace dect2
