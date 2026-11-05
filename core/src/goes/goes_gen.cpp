// GOES HRIT test signal (goes_gen.h).
#include "dect2/goes/goes_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/rrc.h"
#include "dect2/goes/goes_rx.h"
#include "dect2/hfdig/hfdig_hamdrm.h"
#include "dect2/common/inflate.h"
#include "dect2/meteor/meteor_proto.h"
#include "dect2/ccsds/ccsds.h"
#include <cmath>
#include <deque>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;

goes::Headers baseHeaders(int fileType, const std::string& annotation, int product, int sub, int param, int noaaComp) {
    goes::Headers h;
    h.fileType = fileType;
    h.annotation = annotation;
    h.hasTime = true; h.days = 25121; h.msOfDay = 43200000;     // 2026-10-12 12:00:00 UTC
    h.hasNoaa = true; h.productId = product; h.subId = sub; h.parameter = param; h.noaaCompression = noaaComp;
    return h;
}

void putLe16(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
void putLe32(std::vector<uint8_t>& v, uint32_t x) { putLe16(v, x & 0xFFFF); putLe16(v, x >> 16); }

// a ZIP archive with one stored file
std::vector<uint8_t> storedZip(const std::string& name, const std::string& body) {
    const uint32_t crc = crc32Ieee(reinterpret_cast<const uint8_t*>(body.data()), body.size());
    std::vector<uint8_t> z;
    auto common = [&](std::vector<uint8_t>& v) {
        putLe16(v, 20); putLe16(v, 0); putLe16(v, 0); putLe16(v, 0x6000); putLe16(v, 0x5B4C);   // version, flags, stored, time, date
        putLe32(v, crc); putLe32(v, (uint32_t)body.size()); putLe32(v, (uint32_t)body.size());
        putLe16(v, (uint32_t)name.size()); putLe16(v, 0);
    };
    putLe32(z, 0x04034b50); common(z);
    z.insert(z.end(), name.begin(), name.end());
    z.insert(z.end(), body.begin(), body.end());
    const uint32_t cdStart = (uint32_t)z.size();
    putLe32(z, 0x02014b50); putLe16(z, 20); common(z);
    putLe16(z, 0); putLe16(z, 0); putLe16(z, 0); putLe32(z, 0); putLe32(z, 0);   // comment, disk, attributes, offset
    z.insert(z.end(), name.begin(), name.end());
    const uint32_t cdLen = (uint32_t)z.size() - cdStart;
    putLe32(z, 0x06054b50); putLe16(z, 0); putLe16(z, 0); putLe16(z, 1); putLe16(z, 1); putLe32(z, cdLen); putLe32(z, cdStart); putLe16(z, 0);
    return z;
}

using dect2::dsp::rrc;
} // namespace

std::vector<uint8_t> goesTestFullDisk(int& w, int& h) {
    w = 100; h = 64;
    std::vector<uint8_t> p((size_t)w * h);
    std::mt19937 rng(7);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const double dx = (x - w / 2.0) / (h / 2.0 - 1), dy = (y - h / 2.0) / (h / 2.0 - 1), r = std::sqrt(dx * dx + dy * dy);
            int v = r > 1 ? 0 : (int)(60 + 140 * std::sqrt(1 - r * r) + 30 * std::sin(x * 0.3) * std::cos(y * 0.2));
            if (r <= 1) v += (int)(rng() % 9) - 4;
            if (y > 40 && y < 46 && x > 30 && x < 70) v = 255;    // a bright cloud bank
            p[(size_t)y * w + x] = (uint8_t)std::max(0, std::min(255, v));
        }
    return p;
}

std::vector<uint8_t> goesTestMeso(int& w, int& h) {
    w = 60; h = 40;
    std::vector<uint8_t> p((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) p[(size_t)y * w + x] = (uint8_t)(((x / 6 + y / 5) & 1) ? 200 - 2 * y : 40 + 3 * x);
    return p;
}

const char* goesTestAdminText() {
    return "OnAir HRIT test signal\r\nADMINISTRATIVE MESSAGE\r\nThis is a synthetic GOES-R HRIT admin message made by OnAir; nothing is transmitted.\r\n";
}
const char* goesTestEmwinText() {
    return "ZFPOUNOK\r\nZONE FORECAST PRODUCT\r\nSYNTHETIC TEST PRODUCT MADE BY ONAIR\r\nTODAY...SUNNY. HIGHS IN THE MID 70S.\r\n";
}

std::vector<GoesTestFile> goesTestFiles() {
    std::vector<GoesTestFile> v;
    // full disk, band 13, two Rice segments
    int fw, fh;
    const std::vector<uint8_t> fd = goesTestFullDisk(fw, fh);
    const int segs = 2, segLines = fh / segs;
    for (int s = 0; s < segs; s++) {
        GoesTestFile f;
        f.vcid = 13; f.apid = 0x30D;
        goes::Headers h = baseHeaders(0, "OR_ABI-L2-CMIPF-M6C13_G16_s20262851200204_e20262851209512_c20262851209583.lrit", 16, 1, 13, 1);
        h.hasImage = true; h.bpp = 8; h.columns = fw; h.lines = segLines; h.compression = 1;
        h.hasNav = true; h.projection = "GEOS(-75.0)"; h.cfac = 20425338; h.lfac = 20425338; h.coff = fw / 2; h.loff = fh / 2 - s * segLines;
        h.hasSeg = true; h.imageId = 4711; h.segSeq = s + 1; h.startCol = 0; h.startLine = s * segLines; h.maxSeg = segs; h.maxCol = fw; h.maxRow = fh;
        h.hasRice = true; h.riceFlags = ccsds::kSzMsb | ccsds::kSzNn | ccsds::kSzAllowK13; h.ricePpb = 16; h.riceLines = 1;
        ccsds::RiceParams rp;
        rp.bitsPerSample = 8; rp.blockSize = 16; rp.rsiBlocks = (fw + 15) / 16; rp.preprocess = true;
        std::vector<uint8_t> data;
        for (int y = 0; y < segLines; y++) {
            std::vector<uint16_t> line((size_t)fw);
            for (int x = 0; x < fw; x++) line[(size_t)x] = fd[(size_t)(s * segLines + y) * fw + x];
            f.dataChunks.push_back(ccsds::riceEncode(line.data(), line.size(), rp));
            data.insert(data.end(), f.dataChunks.back().begin(), f.dataChunks.back().end());
        }
        h.dataBits = (uint64_t)data.size() * 8;
        f.h = h;
        f.file = goes::makeFile(h, data);
        v.push_back(std::move(f));
    }
    // mesoscale, plain
    {
        int mw, mh;
        const std::vector<uint8_t> m = goesTestMeso(mw, mh);
        GoesTestFile f;
        f.vcid = 2; f.apid = 0x202;
        goes::Headers h = baseHeaders(0, "OR_ABI-L2-CMIPM1-M6C02_G16_s20262851201251_e20262851201308_c20262851201365.lrit", 16, 3, 2, 0);
        h.hasImage = true; h.bpp = 8; h.columns = mw; h.lines = mh; h.compression = 0;
        for (size_t i = 0; i < m.size(); i += 1000) f.dataChunks.emplace_back(m.begin() + (std::ptrdiff_t)i, m.begin() + (std::ptrdiff_t)std::min(m.size(), i + 1000));
        h.dataBits = (uint64_t)m.size() * 8;
        f.h = h;
        f.file = goes::makeFile(h, m);
        v.push_back(std::move(f));
    }
    // a relayed JPEG picture
    {
        const std::vector<uint8_t>& j = hamdrmTestPicture();
        GoesTestFile f;
        f.vcid = 30; f.apid = 0x41E;
        goes::Headers h = baseHeaders(0, "himawari9_relay_test.jpg", 43, 0, 0, 2);
        h.hasImage = true; h.bpp = 8; h.columns = 64; h.lines = 48; h.compression = 2;
        f.dataChunks.push_back(j);
        h.dataBits = (uint64_t)j.size() * 8;
        f.h = h;
        f.file = goes::makeFile(h, j);
        v.push_back(std::move(f));
    }
    // admin message
    {
        const std::string t = goesTestAdminText();
        GoesTestFile f;
        f.vcid = 0; f.apid = 0x001;
        goes::Headers h = baseHeaders(2, "ADMIN_TEST_MESSAGE.TXT", 1, 0, 0, 0);
        h.annotation = "admin_test_message";
        const std::vector<uint8_t> d(t.begin(), t.end());
        f.dataChunks.push_back(d);
        h.dataBits = (uint64_t)d.size() * 8;
        f.h = h;
        f.file = goes::makeFile(h, d);
        v.push_back(std::move(f));
    }
    // EMWIN, zipped
    {
        const std::vector<uint8_t> z = storedZip("ZFPOUNOK.TXT", goesTestEmwinText());
        GoesTestFile f;
        f.vcid = 20; f.apid = 0x314;
        goes::Headers h = baseHeaders(214, "Z_EIHA98KWIN121200_C_KWIN_20261012120000_000001-2-ZFPOUNOK.ZIS", 9, 0, 0, 10);
        f.dataChunks.push_back(z);
        h.dataBits = (uint64_t)z.size() * 8;
        f.h = h;
        f.file = goes::makeFile(h, z);
        v.push_back(std::move(f));
    }
    return v;
}

goes::FileOut goesFileOut(const GoesTestFile& f, uint16_t counter) {
    goes::FileOut o;
    o.apid = f.apid;
    std::vector<uint8_t> first;
    first.push_back((uint8_t)(counter >> 8)); first.push_back((uint8_t)counter);
    const uint64_t bits = (uint64_t)f.file.size() * 8;
    for (int i = 7; i >= 0; i--) first.push_back((uint8_t)(bits >> (8 * i)));
    const std::vector<uint8_t> hdr = goes::buildHeaders(f.h, f.dialect);
    first.insert(first.end(), hdr.begin(), hdr.end());
    o.chunks.push_back(first);
    for (const auto& c : f.dataChunks) o.chunks.push_back(c);
    return o;
}

// ---------------------------------------------------------------- the signal
struct GoesGenerator::Impl {
    GoesGenConfig c;
    std::vector<GoesTestFile> files;
    goes::Mux mux;
    uint16_t counter = 0;
    int fillLeft = 0;
    bool once = false;
    meteor::ConvEncoder enc;
    uint8_t nrzPrev = 0;
    std::vector<float> sym, symQ; // BPSK symbols (QPSK: the I part, symQ the Q part) from index symBase
    int64_t symBase = 0, symNext = 0;
    uint64_t frames = 0;
    std::vector<float> table;     // the pulse, kOver points a symbol over +-kSpan symbols
    static constexpr int kOver = 64, kSpan = 6;
    int64_t n = 0;
    double phase0 = 0;
    std::mt19937 rng;
    dect2::normal_distribution<float> gauss{0.f, 1.f};
    float sigma = 0;

    explicit Impl(const GoesGenConfig& cfg) : c(cfg), mux(cfg.scid), rng(cfg.seed) {
        files = c.files.empty() ? goesTestFiles() : c.files;
        table.resize((size_t)(2 * kSpan * kOver + 2));
        for (size_t i = 0; i < table.size(); i++) table[i] = (float)rrc((double)i / kOver - kSpan, c.rolloff);
        sigma = c.snrDb >= 99 ? 0.f : (float)std::sqrt(c.amplitude * c.amplitude * (c.rate / c.symbolRate) / std::pow(10.0, c.snrDb / 10) / 2);
        phase0 = dect2::uniform_real_distribution<double>(0, 2 * kPi)(rng);
        sym.assign(kSpan, 0.f); symBase = -kSpan; if (c.qpsk) symQ.assign(kSpan, 0.f);    // silence before the first symbol
        addRound();
        fillLeft = std::max(0, c.fillFrames);
    }
    void addRound() { for (const auto& f : files) mux.addFile(f.vcid, goesFileOut(f, counter++)); }
    void frame() {
        uint8_t v[goes::kVcduBytes];
        if (!mux.next(v)) {
            if (once || fillLeft > 0) { mux.fill(v); if (fillLeft > 0) fillLeft--; }
            else if (c.fillFrames < 0) { once = true; mux.fill(v); }
            else { addRound(); fillLeft = c.fillFrames; mux.next(v); }
        }
        uint8_t cadu[meteor::kCaduBytes];
        meteor::encodeCadu(v, cadu, c.dualBasis);
        for (int i = 0; i < meteor::kCaduBytes; i++)
            for (int b = 7; b >= 0; b--) {
                uint8_t s0, s1;
                uint8_t in = (cadu[i] >> b) & 1;
                if (c.nrzm) { in ^= nrzPrev; nrzPrev = in; }     // NRZ-M: a 1 is a change of level
                enc.push(in, s0, s1);
                if (c.invertG2) s1 ^= 1;                       // CCSDS sends G2 inverted
                if (c.qpsk) { sym.push_back(s0 ? -0.70710678f : 0.70710678f); symQ.push_back(s1 ? -0.70710678f : 0.70710678f); }
                else { sym.push_back(s0 ? -1.f : 1.f); sym.push_back(s1 ? -1.f : 1.f); }
            }
        symNext += (c.qpsk ? 1 : 2) * 8 * meteor::kCaduBytes;
        frames++;
    }
    void generate(cf32* out, size_t cnt) {
        const double rs = c.symbolRate * (1 + c.sroPpm * 1e-6);
        const double w = 2 * kPi * c.cfoHz / c.rate;
        for (size_t i = 0; i < cnt; i++, n++) {
            const double u = (double)n / c.rate * rs;
            const int64_t k0 = (int64_t)std::floor(u);
            while (k0 + kSpan >= symNext) frame();
            // the same fractional position in the pulse table for every tap, kOver entries apart
            const double d0 = (u - (double)(k0 - kSpan + 1) + kSpan) * kOver;
            const int j0 = (int)d0;
            const float f = (float)(d0 - j0);
            const float* sp = &sym[(size_t)(k0 - kSpan + 1 - symBase)];
            const float* sq = c.qpsk ? &symQ[(size_t)(k0 - kSpan + 1 - symBase)] : nullptr;
            float acc = 0, accQ = 0;
            for (int t = 0, j = j0; t < 2 * kSpan; t++, j -= kOver)
                if (j >= 0 && j + 1 < (int)table.size()) {
                    const float pv = table[(size_t)j] + f * (table[(size_t)j + 1] - table[(size_t)j]);
                    acc += sp[t] * pv;
                    if (sq) accQ += sq[t] * pv;
                }
            const double ph = phase0 + w * (double)n;
            const double cs = std::cos(ph), sn = std::sin(ph);
            // carrier: (acc + j accQ) * e^{j ph}
            cf32 s = cf32((float)(c.amplitude * (acc * cs - accQ * sn)), (float)(c.amplitude * (acc * sn + accQ * cs)));
            if (sigma > 0) s += cf32{sigma * gauss(rng), sigma * gauss(rng)};
            out[i] = s;
        }
        // drop the symbols no longer needed
        const int64_t keep = (int64_t)std::floor((double)n / c.rate * rs) - kSpan - 4;
        if (keep - symBase > 65536) { sym.erase(sym.begin(), sym.begin() + (keep - symBase)); if (c.qpsk) symQ.erase(symQ.begin(), symQ.begin() + (keep - symBase)); symBase = keep; }
    }
};

GoesGenerator::GoesGenerator(const GoesGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
GoesGenerator::~GoesGenerator() = default;
void GoesGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }
uint64_t GoesGenerator::framesSent() const { return p_->frames; }

namespace {
class GoesSynth : public ModeSynth {
public:
    GoesSynth(const SynthConfig& s, double rate) : rate_(rate) {
        GoesGenConfig c;
        c.rate = rate; c.snrDb = s.snrDb; c.cfoHz = s.cfoHz; c.sroPpm = s.sroPpm;
        gen_ = std::make_unique<GoesGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<GoesGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeGoesSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<GoesSynth>(cfg, sampleRate);
}

} // namespace dect2
