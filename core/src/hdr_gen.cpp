// HD Radio test signal (see hdr_gen.h): the station's Layer 2 content, the FM MP1 or AM MA1 Layer 1 frames, the analog host, resampling
// to the radio's rate, carrier offset and noise.
#include "dect2/hdr_gen.h"
#include "dect2/exact_resampler.h"
#include "dect2/gen_util.h"
#include "dect2/hdr_fec.h"
#include "dect2/hdr_l2.h"
#include "dect2/hdr_phy.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>

namespace dect2 {

const HdrTestContent& hdrTestContent() { static const HdrTestContent c; return c; }

// ---------------------------------------------------------------- the logo: a 64 x 64 PNG with a 4-colour palette, stored (not compressed)

namespace {
uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return c;
}
void be32(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    be32(out, (uint32_t)data.size());
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    be32(out, ~crc32(td.data(), td.size()));
}
std::vector<uint8_t> makeLogo() {
    const int W = 64, H = 64;
    std::vector<uint8_t> raw;
    for (int y = 0; y < H; y++) {
        raw.push_back(0);   // no filter
        for (int x = 0; x < W; x += 4) {
            uint8_t b = 0;
            for (int k = 0; k < 4; k++) {
                const double dx = x + k - 31.5, dy = y - 31.5, r = std::sqrt(dx * dx + dy * dy);
                int c = 0;
                if (r < 8) c = 3;
                else if (r > 13 && r < 17) c = 2;
                else if (r > 22 && r < 27) c = 1;
                else if (r >= 27 && r < 29 && dx > 0) c = 2;
                b = (uint8_t)(b | (c << (6 - 2 * k)));
            }
            raw.push_back(b);
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01, 0x01, (uint8_t)(raw.size() & 0xFF), (uint8_t)(raw.size() >> 8), (uint8_t)(~raw.size() & 0xFF), (uint8_t)((~raw.size() >> 8) & 0xFF)};
    z.insert(z.end(), raw.begin(), raw.end());
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    be32(z, (b << 16) | a);
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    be32(ihdr, W); be32(ihdr, H);
    ihdr.push_back(2); ihdr.push_back(3); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(png, "IHDR", ihdr);
    chunk(png, "PLTE", {16, 28, 48, 40, 150, 200, 240, 244, 250, 250, 160, 40});
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}
}

const std::vector<uint8_t>& hdrTestLogoPng() { static const std::vector<uint8_t> v = makeLogo(); return v; }

namespace {
using namespace hdr;

// ---------------------------------------------------------------- the station's Layer 2 content

SisConfig sisConfig(bool mp3) {
    const HdrTestContent& c = hdrTestContent();
    SisConfig s;
    s.callSign = c.callSign; s.name = c.name; s.longName = c.longName; s.slogan = c.slogan; s.message = c.message;
    s.country = c.country; s.facilityId = c.facilityId;
    s.location = true; s.lat = c.lat; s.lon = c.lon; s.altM = c.altM;
    for (const auto& p : c.programs) s.audio.push_back({p.number, p.type, 0, 0});
    if (mp3) s.audio.push_back({c.extended.number, c.extended.type, 0, 0});
    s.data.push_back({65, 0, 0});   // a traffic data service
    return s;
}

const HdrTestContent::Prog& progInfo(int n) { return n < 2 ? hdrTestContent().programs[(size_t)n] : hdrTestContent().extended; }

struct Content {
    bool mp3;
    SisEncoder sis;
    std::vector<uint8_t> psd[3];
    uint16_t psdSeq[3] = {0, 0, 0};
    std::vector<uint8_t> fixedHdlc, fixedSub;
    uint16_t sigSeq = 0, lotSeq = 0;
    std::vector<uint8_t> sig;
    std::vector<std::vector<uint8_t>> frags;
    int pduSeq = 0, pktSeq[3] = {0, 0, 0}, p3Seq = 0;

    explicit Content(bool extended) : mp3(extended), sis(sisConfig(extended)) {
        const HdrTestContent& c = hdrTestContent();
        std::vector<SigService> sv;
        SigService a;
        a.audio = true; a.number = 1; a.name = c.programs[0].sigName;
        a.comps.push_back({true, 0, 0, 0, kMimeHdc, -1});
        a.comps.push_back({false, 1, c.artPort, 3, kMimePrimaryImage, 0});
        sv.push_back(a);
        SigService b;
        b.audio = true; b.number = 2; b.name = c.programs[1].sigName;
        b.comps.push_back({true, 0, 1, 0, kMimeHdc, -1});
        sv.push_back(b);
        if (mp3) {
            SigService x;
            x.audio = true; x.number = 3; x.name = c.extended.sigName;
            x.comps.push_back({true, 0, 2, 0, kMimeHdc, -1});
            sv.push_back(x);
        }
        SigService t;
        t.audio = false; t.number = 0x4001; t.name = c.trafficName;
        t.comps.push_back({false, 0, c.trafficPort, 3, kMimeTtnTraffic, 65});
        sv.push_back(t);
        sig = sigTable(sv);
        LotFile f;
        f.lot = c.artLot; f.name = c.artName; f.mime = kMimePng; f.bytes = hdrTestLogoPng();
        frags = lotFragments(f, 0);
        for (auto& p : psd) p.push_back(0x7E);
        fixedHdlc.push_back(0x7E);
    }
    void topPsd(int prog) {
        if (psd[prog].size() > 300) return;
        const HdrTestContent::Prog& p = progInfo(prog);
        const HdrTestContent& c = hdrTestContent();
        const std::vector<uint8_t> tag = id3Tag(p.title, p.artist, p.album, p.genre, prog == 0 ? c.artLot : -1, kMimePrimaryImage);
        hdlcAppend(psd[prog], aasFrame(prog == 0 ? 0x5100 : (uint16_t)(0x5200 + prog), psdSeq[prog]++, tag));
    }
    void topFixed(size_t need) {
        while (fixedSub.size() < need) {
            if (fixedHdlc.size() < 255) {   // the carousel: the guide, then the logo
                hdlcAppend(fixedHdlc, aasFrame(0x20, sigSeq++, sig));
                for (const auto& fr : frags) hdlcAppend(fixedHdlc, aasFrame((uint16_t)hdrTestContent().artPort, lotSeq++, fr));
            }
            fixedBlock(fixedHdlc, fixedSub);
        }
    }
    std::vector<uint8_t> program(int prog, int codec, int stream, int npk, int pktBytes, int psdRoom) {
        AudioPduSpec s;
        s.codecMode = codec; s.streamId = stream; s.program = prog; s.progType = progInfo(prog).type;
        s.pduSeq = pduSeq; s.seq = pktSeq[prog];
        s.packetBytes.assign((size_t)npk, pktBytes);
        s.fill = 0;
        s.psdRoom = psdRoom < 0 ? 256 - audioPduHeaderBytes(s) : psdRoom;
        if (stream == 0) { topPsd(prog); pktSeq[prog] = (pktSeq[prog] + npk) % 64; }
        std::vector<uint8_t> none;
        return audioPdu(s, stream == 0 ? psd[prog] : none);
    }
    // FM P1: HD1 and HD2 with 32 packets each, the fixed data subchannel at the end
    std::vector<uint8_t> pduFm() {
        std::vector<uint8_t> o = program(0, 0, 0, 32, 160, -1);
        const std::vector<uint8_t> b = program(1, 0, 0, 32, 80, -1);
        o.insert(o.end(), b.begin(), b.end());
        FixedSpec f;
        f.subLen = 7 * 259;
        topFixed((size_t)f.subLen);
        std::vector<uint8_t> tail;
        fixedRegion(f, fixedSub, tail);
        const size_t total = (size_t)pduBytes(kP1LenFm);
        o.resize(total - tail.size(), 0);
        o.insert(o.end(), tail.begin(), tail.end());
        pduSeq = (pduSeq + 1) % 8;
        return o;
    }
    // AM P1 (one of the 8 blocks of a frame): the core streams with 4 packets each
    std::vector<uint8_t> pduAmP1() {
        std::vector<uint8_t> o = program(0, 1, 0, 4, 24, 40);
        const std::vector<uint8_t> b = program(1, 1, 0, 4, 12, 30);
        o.insert(o.end(), b.begin(), b.end());
        FixedSpec f;
        f.subLen = 160;
        topFixed((size_t)f.subLen);
        std::vector<uint8_t> tail;
        fixedRegion(f, fixedSub, tail);
        const size_t total = (size_t)pduBytes(kP1LenAm);
        o.resize(total - tail.size(), 0);
        o.insert(o.end(), tail.begin(), tail.end());
        pduSeq = (pduSeq + 1) % 8;
        return o;
    }
    // FM P3 (MP3, one per two blocks): HD3 with 4 packets
    std::vector<uint8_t> pduFmP3() {
        std::vector<uint8_t> o = program(2, 0, 0, 4, 100, 100);
        o.resize((size_t)pduBytes(kP3LenMp3), 0);
        return o;
    }
    // AM P3: HD1's enhanced stream
    std::vector<uint8_t> pduAmP3() {
        AudioPduSpec s;
        s.codecMode = 1; s.streamId = 1; s.program = 0; s.progType = hdrTestContent().programs[0].type;
        s.seq = p3Seq; s.packetBytes.assign(32, 80); s.psdRoom = 0;
        p3Seq = (p3Seq + 32) % 64;
        std::vector<uint8_t> none;
        std::vector<uint8_t> o = audioPdu(s, none);
        o.resize((size_t)pduBytes(kP3LenMa1), 0);
        return o;
    }
};

// the PIDS frame of the SIS encoder in L1 order (the bytes go least significant bit first), scrambled
void pidsL1(Content& c, uint8_t* out) {
    uint8_t f[80];
    c.sis.next(f);
    for (int i = 0; i < 80; i++) out[((i >> 3) << 3) + 7 - (i & 7)] = f[i];
    scramble(out, 80);
}

// ---------------------------------------------------------------- the synthesizer

class HdrSynth : public ModeSynth {
public:
    HdrSynth(const SynthConfig& cfg, double rate) : rate_(rate), am_(cfg.modeOpt[0] == 1), mp3_(cfg.modeOpt[0] == 2), cfoHz_(cfg.cfoHz), c_(mp3_) {
        rs_.configure(kRateFm, rate);
        pxi_.reset(kP3LenMp3);
        if (!am_) {
            ampA_ = 0.25f;
            amp_ = ampA_ * std::sqrt(0.01f / (382.f * 2.f));       // the digital sidebands 20 dB below the analog FM
            const double psc = 2.0 * amp_ * amp_;
            sigma2_ = psc * rate / ((kRateFm / kFftFm) * std::pow(10.0, cfg.snrDb / 10));
        } else {
            ampA_ = 0.25f;
            const float A2 = ampA_ * ampA_;
            lv_.pri = std::sqrt(A2 * 1e-3f / 10.5f);              // dBc per subcarrier: primary -30, secondary -43, tertiary -44,
            lv_.sec = std::sqrt(A2 * std::pow(10.f, -4.3f) / 2.5f);   // PIDS -37, reference -26
            lv_.ter = std::sqrt(A2 * std::pow(10.f, -4.4f) / 0.5f);
            lv_.pids = std::sqrt(A2 * std::pow(10.f, -3.7f) / 2.5f);
            lv_.ref = std::sqrt(A2 * std::pow(10.f, -2.6f) / 0.25f);
            const double psc = 10.5 * lv_.pri * lv_.pri;
            sigma2_ = psc * rate / ((kRateFm / 4096.0) * std::pow(10.0, cfg.snrDb / 10));
        }
        sigma_ = (float)std::sqrt(sigma2_ / 2);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        size_t done = 0;
        while (done < n) {
            if (pos_ >= pend_.size()) { pend_.clear(); pos_ = 0; produceBlock(); continue; }
            const size_t k = std::min(n - done, pend_.size() - pos_);
            const double w = 2 * M_PI * cfoHz_ / rate_;
            for (size_t i = 0; i < k; i++) {
                out[done + i] = pend_[pos_ + i] * cf32((float)std::cos(ph_), (float)std::sin(ph_));
                ph_ += w;
                if (ph_ > M_PI) ph_ -= 2 * M_PI; else if (ph_ < -M_PI) ph_ += 2 * M_PI;
            }
            noise_.add(out + done, k, sigma_);
            done += k;
            pos_ += k;
        }
    }

private:
    void produceBlock() {
        base_.clear();
        if (!am_) {
            if (blk_ == 0) fmFrame();
            tx_.block(&pm_[(size_t)blk_ * kPmBlock], mp3_ ? &px_[(size_t)blk_ * kP3LenMp3] : nullptr, blk_, mp3_ ? 3 : 1, amp_, base_);
            for (cf32& v : base_) {   // analog FM, a 1 kHz tone at 50 kHz deviation
                v += ampA_ * cf32((float)std::cos(aph_), (float)std::sin(aph_));
                aph_ += 2 * M_PI * 50000.0 * std::sin(tone_) / kRateFm;
                tone_ += 2 * M_PI * 1000.0 / kRateFm;
                if (tone_ > 2 * M_PI) tone_ -= 2 * M_PI;
                if (aph_ > M_PI) aph_ -= 2 * M_PI; else if (aph_ < -M_PI) aph_ += 2 * M_PI;
            }
            blk_ = (blk_ + 1) % 16;
        } else {
            if (blk_ == 0) amFrame();
            const size_t e = (size_t)blk_ * 32 * kAmCols;
            amTx_.block(&pl_[e], &pu_[e], &s_[e], &t_[e], &pidsW_[(size_t)blk_ * 64], blk_, lv_, base_);
            for (cf32& v : base_) {   // analog AM, a 1 kHz tone at 30 % modulation
                v += cf32(ampA_ * (1.f + 0.3f * (float)std::sin(tone_)), 0.f);
                tone_ += 2 * M_PI * 1000.0 / kRateFm;
                if (tone_ > 2 * M_PI) tone_ -= 2 * M_PI;
            }
            blk_ = (blk_ + 1) % 8;
        }
        rs_.process(base_.data(), base_.size(), pend_);
    }

    void fmFrame() {
        std::vector<uint8_t> pdu = c_.pduFm();
        std::vector<uint8_t> bits((size_t)kP1LenFm), enc((size_t)kP1LenFm * 3);
        packTransfer(pdu, kPciAudioFixed, kP1LenFm, bits.data());
        scramble(bits.data(), bits.size());
        convEncode(kCodeFm, bits.data(), bits.size(), enc.data());
        pm_.assign((size_t)16 * kPmBlock, 0);
        const std::vector<int>& pos = fmP1Pos();
        size_t k = 0;
        for (size_t i = 0; i < enc.size(); i++) if (kPunct25[i % 6]) pm_[(size_t)pos[k++]] = enc[i];
        const std::vector<int>& pp = fmPidsPos();
        for (int bc = 0; bc < 16; bc++) {
            uint8_t pb[80], pe[240];
            pidsL1(c_, pb);
            convEncode(kCodeFm, pb, 80, pe);
            k = 0;
            for (int i = 0; i < 240; i++) if (kPunct25[i % 6]) pm_[(size_t)bc * kPmBlock + (size_t)pp[k++]] = pe[i];
        }
        if (mp3_) {   // P3 on the extended partitions: a code word per two blocks, through the convolutional interleaver
            px_.assign((size_t)16 * kP3LenMp3, 0);
            std::vector<uint8_t> b3((size_t)kP3LenMp3), e3((size_t)kP3LenMp3 * 3), kept;
            for (int c = 0; c < 8; c++) {
                while (pxi_.wantsFrame()) {
                    const std::vector<uint8_t> pdu = c_.pduFmP3();
                    packTransfer(pdu, kPciAudio, kP3LenMp3, b3.data());
                    scramble(b3.data(), b3.size());
                    convEncode(kCodeFm, b3.data(), b3.size(), e3.data());
                    kept.clear();
                    for (size_t i = 0; i < e3.size(); i++) if (kPunctP3[i % 6]) kept.push_back(e3[i]);
                    pxi_.addFrame(kept.data());
                }
                pxi_.call(&px_[(size_t)c * 2 * kP3LenMp3]);
            }
        }
    }

    std::vector<uint8_t> amGroup() {
        std::vector<uint8_t> g;
        g.reserve(72000);
        std::vector<uint8_t> bits((size_t)kP1LenAm), enc((size_t)kP1LenAm * 3);
        for (int b = 0; b < 8; b++) {
            const std::vector<uint8_t> pdu = c_.pduAmP1();
            packTransfer(pdu, kPciAudioFixed, kP1LenAm, bits.data());
            scramble(bits.data(), bits.size());
            convEncode(kCodeE1, bits.data(), bits.size(), enc.data());
            for (size_t i = 0; i < enc.size(); i++) if (kPunctE1[i % 15]) g.push_back(enc[i]);
        }
        return g;
    }

    void amFrame() {
        const AmMaps& M = amMaps();
        while (groups_.size() < 4) groups_.push_back(amGroup());
        const std::vector<uint8_t>& gB = groups_[0];
        const std::vector<uint8_t>& gM = groups_[3];
        const size_t words = (size_t)8 * 32 * kAmCols;
        pl_.assign(words, 0); pu_.assign(words, 0); s_.assign(words, 0); t_.assign(words, 0);
        auto set = [](std::vector<uint8_t>& m, const AmBit& b, uint8_t v) { if (v) m[(size_t)b.elem] |= (uint8_t)(1 << b.bit); };
        for (int n = 0; n < 18000; n++) {
            set(pl_, M.bl[(size_t)n], gB[(size_t)((n / 3) * 12 + kBlDelay[n % 3])]);
            set(pl_, M.ml[(size_t)n], gM[(size_t)((n / 3) * 12 + kMlDelay[n % 3])]);
            set(pu_, M.bu[(size_t)n], gB[(size_t)((n / 3) * 12 + kBuDelay[n % 3])]);
            set(pu_, M.mu[(size_t)n], gM[(size_t)((n / 3) * 12 + kMuDelay[n % 3])]);
        }
        {   // P3
            const std::vector<uint8_t> pdu = c_.pduAmP3();
            std::vector<uint8_t> bits((size_t)kP3LenMa1), enc((size_t)kP3LenMa1 * 3), p3;
            packTransfer(pdu, kPciAudio, kP3LenMa1, bits.data());
            scramble(bits.data(), bits.size());
            convEncode(kCodeE2, bits.data(), bits.size(), enc.data());
            for (size_t i = 0; i < enc.size(); i++) if (kPunctE2[i % 6]) p3.push_back(enc[i]);
            for (int n = 0; n < 12000; n++) set(t_, M.el[(size_t)n], p3[(size_t)((n / 2) * 6 + kElDelay[n % 2])]);
            for (int n = 0; n < 24000; n++) set(s_, M.eu[(size_t)n], p3[(size_t)((n / 4) * 6 + kEuDelay[n % 4])]);
        }
        for (int b = 0; b < 8; b++)
            for (int col = 0; col < kAmCols; col++)
                for (int row : M.trainRow[col]) {
                    const size_t e = ((size_t)b * 32 + (size_t)row) * kAmCols + (size_t)col;
                    pl_[e] = pu_[e] = 0x25; s_[e] = 0x9; t_[e] = 0x2;
                }
        pidsW_.assign((size_t)8 * 64, 0);
        for (int b = 0; b < 8; b++) {
            uint8_t pb[80], pe[240];
            pidsL1(c_, pb);
            convEncode(kCodeE2, pb, 80, pe);
            uint8_t* w = &pidsW_[(size_t)b * 64];
            for (int n = 0; n < 120; n++) {
                const int i = n / 12, j = n % 12;
                if (pe[i * 24 + kPidsIlDelay[j]]) w[M.pidsRowL[(size_t)n] * 2] |= (uint8_t)(1 << M.pidsBitL[(size_t)n]);
                if (pe[i * 24 + kPidsIuDelay[j]]) w[M.pidsRowU[(size_t)n] * 2 + 1] |= (uint8_t)(1 << M.pidsBitU[(size_t)n]);
            }
            for (int row : {8, 24}) { w[row * 2] = 0x9; w[row * 2 + 1] = 0x9; }
        }
        groups_.pop_front();
    }

    double rate_;
    bool am_, mp3_;
    double cfoHz_;
    float ampA_ = 0.25f, amp_ = 0;
    AmLevels lv_;
    double sigma2_ = 0;
    float sigma_ = 0;
    Content c_;
    FmTx tx_;
    AmTx amTx_;
    std::vector<uint8_t> pm_, px_;
    PxInterleaver pxi_;
    std::vector<uint8_t> pl_, pu_, s_, t_, pidsW_;
    std::deque<std::vector<uint8_t>> groups_;
    int blk_ = 0;
    std::vector<cf32> base_, pend_;
    size_t pos_ = 0;
    ExactResampler rs_;
    double ph_ = 0, aph_ = 0, tone_ = 0;
    genutil::NoiseSource noise_{23};
};
} // namespace

std::unique_ptr<ModeSynth> makeHdrSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<HdrSynth>(cfg, sampleRate);
}

} // namespace dect2
