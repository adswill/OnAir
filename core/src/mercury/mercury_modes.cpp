// Mercury OFDM data modes: the parameter table, the derived frame geometry, the symbol maps, the interleaver, the frame CRC and the
// modulator (see mercury_phy.h). The parameters are those of Mercury's modem/freedv/ofdm_mode.c (commit c72997be9d3c); where codec2
// computes a size in float (the carrier numbering), it is computed here the same way, so the carriers land where Mercury's do.
#include "dect2/mercury/mercury_phy.h"
#include "dect2/common/crc.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>

namespace dect2 {
namespace mercury {

namespace {

using dect2::math::kPi;

// clip and filter of the transmitter (ofdm_mode.c: amp_scale, clip_gain1, clip_gain2, the band-pass prototype's pass / stop edge)
struct TxShape { float ampScale, clipGain1, clipGain2; bool clip; float passHz, stopHz; bool centreOnCarriers; };

const OfdmMode kModes[kNumOfdmModes] = {
    {"DATAC0", 2, 9, 4, 5, 2, 0.016f, 0.006f, 32, kH128x256, -1, "broadcast and tests"},
    {"DATAC1", 0, 27, 38, 5, 2, 0.016f, 0.006f, 16, kH4096x8192, 4, "ARQ rung 4, broadcast"},
    {"DATAC3", 1, 9, 29, 5, 2, 0.016f, 0.006f, 40, kH1024x2048, 3, "ARQ rung 3, broadcast (the -m default)"},
    {"DATAC4", 3, 4, 47, 5, 2, 0.016f, 0.006f, 32, kH1024x2048, 2, "ARQ rung 2, broadcast"},
    {"DATAC13", 4, 3, 18, 5, 2, 0.016f, 0.006f, 48, kH256x512, -1, "broadcast and the monitor"},
    {"DATAC14", 5, 4, 4, 5, 2, 0.018f, 0.005f, 32, kHra56x56, -1, "tests"},
    {"DATAC15", 7, 3, 34, 5, 2, 0.016f, 0.006f, 48, kH256x768, 1, "ARQ rung 1, broadcast"},
    {"DATAC16", 8, 3, 28, 5, 2, 0.016f, 0.006f, 32, kH256x768, -2, "ARQ control: CALL, ACCEPT, CQ, polls, DISCONNECT"},
    {"DATAC17", 9, 33, 61, 5, 2, 0.016f, 0.006f, 168, kH16200x9720, 5, "ARQ rung 5, broadcast"},
    {"QAM16C2", 10, 33, 31, 5, 4, 0.016f, 0.004f, 168, kH16200x9720, 6, "ARQ rung 6, broadcast"},
};

const TxShape kShape[kNumOfdmModes] = {
    {300e3f, 2.2f, 0.85f, true, 400, 600, false},
    {145e3f, 2.7f, 0.8f, true, 900, 1100, false},
    {300e3f, 2.2f, 0.8f, true, 400, 600, false},
    {600e3f, 1.2f, 1.0f, true, 200, 400, true},
    {750e3f, 1.2f, 1.0f, true, 200, 400, true},
    {600e3f, 2.0f, 1.0f, true, 200, 400, true},
    {750e3f, 1.2f, 1.0f, true, 200, 400, true},
    {750e3f, 1.2f, 1.0f, true, 200, 400, true},
    {145e3f, 2.7f, 0.8f, true, 1100, 1300, false},
    {135e3f, 1.0f, 1.0f, false, 1100, 1300, false},
};
constexpr float kOfdmPeak = 16384.f;

// the BPSK pilot values of codec2 ("compatible with the Octave version")
const int8_t kPilotValues[64] = {
    -1, -1, 1,  1,  -1, -1, -1, 1,  -1, 1,  -1, 1,  1,  1,  1,  1,
    1,  1,  1,  -1, -1, 1,  -1, 1,  -1, 1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  -1, 1,  1,  1,  1,  1,  -1, -1, -1, -1, -1, -1, 1,
    -1, 1,  -1, 1,  -1, -1, 1,  -1, 1,  1,  1,  1,  -1, 1,  -1, 1};

// the unique word's 24-bit pattern; DATAC0 and DATAC1 use its first 16 bits
const uint8_t kUw24[24] = {1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};

const float kQam16[16][2] = {
    {4.4721e-01f, 2.7756e-17f},  {8.9443e-01f, 4.4721e-01f},  {8.9443e-01f, -4.4721e-01f},  {1.3416e+00f, 1.1102e-16f},
    {2.7756e-17f, -4.4721e-01f}, {-4.4721e-01f, -8.9443e-01f}, {4.4721e-01f, -8.9443e-01f},  {1.1102e-16f, -1.3416e+00f},
    {-2.7756e-17f, 4.4721e-01f}, {4.4721e-01f, 8.9443e-01f},  {-4.4721e-01f, 8.9443e-01f},  {-1.1102e-16f, 1.3416e+00f},
    {-4.4721e-01f, -2.7756e-17f}, {-8.9443e-01f, -4.4721e-01f}, {-8.9443e-01f, 4.4721e-01f}, {-1.3416e+00f, -1.1102e-16f}};

bool isPrime(int x) {
    if (x < 2) return false;
    for (int i = 2; (int64_t)i * i <= x; i++) if (x % i == 0) return false;
    return true;
}

cf mapBits(const uint8_t* b, int bps) {
    if (bps == 2) return qpskMap(b[1], b[0]);   // b[0] is the first (most significant) bit
    return qam16Map((b[0] << 3) | (b[1] << 2) | (b[2] << 1) | b[3]);
}

Geometry build(int id) {
    const OfdmMode& m = kModes[id];
    Geometry g;
    g.id = id; g.nc = m.nc; g.np = m.np; g.ns = m.ns; g.bps = m.bps;
    // as ofdm_create: rs, M and Ncp in float, the lowest carrier from roundf
    const float ts = m.ts, rs = 1.0f / ts;
    g.rs = rs;
    g.M = (int)std::lround(8000.0f * ts);
    g.Ncp = (int)std::lround(m.tcp * 8000.0f);
    g.sps = g.M + g.Ncp;
    const float tval = (float)m.nc / 2.0f;
    g.nlower = (int)(std::roundf((1500.0f / rs) - tval) - 1.0f);
    g.bitsPerFrame = (m.ns - 1) * m.nc * m.bps;
    g.bitsPerPacket = m.np * g.bitsPerFrame;
    g.symsPerPacket = g.bitsPerPacket / m.bps;
    g.nuwbits = m.nuwbits;
    g.nuwsyms = m.nuwbits / m.bps;
    // the unique word bits (ofdm_mode.c)
    g.uwBits.assign((size_t)m.nuwbits, 0);
    if (id == kDatac0 || id == kDatac1) {
        for (int i = 0; i < 16; i++) g.uwBits[(size_t)i] = kUw24[i];
    } else {
        for (int i = 0; i < 24 && i < m.nuwbits; i++) g.uwBits[(size_t)i] = kUw24[i];
        for (int i = 0; i < 24; i++) g.uwBits[(size_t)(m.nuwbits - 24 + i)] = kUw24[i];
    }
    // the unique word's symbol positions (ofdm_create): integer division as codec2 does it
    const int ndps = (m.ns - 1) * m.nc;
    int step = m.nc + 1;
    if ((g.nuwsyms * step) / m.bps >= m.np * ndps) step = m.nc - 1;
    g.uwSym.resize((size_t)g.nuwsyms);
    g.isUw.assign((size_t)g.symsPerPacket, 0);
    for (int i = 0; i < g.nuwsyms; i++) {
        g.uwSym[(size_t)i] = ((i + 1) * step) / m.bps;
        g.isUw[(size_t)g.uwSym[(size_t)i]] = 1;
    }
    g.uwSyms.resize((size_t)g.nuwsyms);
    for (int s = 0; s < g.nuwsyms; s++) g.uwSyms[(size_t)s] = mapBits(&g.uwBits[(size_t)(s * m.bps)], m.bps);
    const LdpcCode& c = ldpcCode(m.code);
    g.codeN = c.n; g.codeK = c.k; g.parity = c.m;
    g.dataBits = g.bitsPerPacket - g.nuwbits - c.m;
    g.codedBits = g.dataBits + c.m;
    g.codedSyms = g.codedBits / m.bps;
    g.interleaverB = interleaverStep(g.codedSyms);
    g.frameBytes = g.dataBits / 8;
    g.payloadBytes = g.frameBytes - 2;
    g.samplesPerFrame = m.ns * g.sps;
    g.burstSamples = (m.np + 2) * g.samplesPerFrame;
    g.burstSeconds = g.burstSamples / 8000.0;
    g.bandwidthHz = m.nc * rs;
    g.centreHz = (g.nlower + (m.nc + 1) * 0.5) * rs;
    g.pilots.assign((size_t)(m.nc + 2), 0.f);
    for (int k = 1; k <= m.nc; k++) g.pilots[(size_t)k] = (float)kPilotValues[k];   // edge pilots off in every data mode
    for (int which = 0; which < 2; which++) {
        std::vector<cf>& out = which ? g.postamble : g.preamble;
        out.assign((size_t)(m.ns * (m.nc + 2)), cf(0, 0));
        std::vector<uint16_t> r((size_t)g.bitsPerFrame);
        ofdmRand(r.data(), g.bitsPerFrame, which ? 3 : 2);
        std::vector<uint8_t> bits((size_t)g.bitsPerFrame);
        for (int i = 0; i < g.bitsPerFrame; i++) bits[(size_t)i] = r[(size_t)i] > 16384;
        for (int k = 0; k < m.nc + 2; k++) out[(size_t)k] = cf(g.pilots[(size_t)k], 0);
        int s = 0;
        for (int row = 1; row < m.ns; row++)
            for (int k = 1; k <= m.nc; k++, s++) out[(size_t)(row * (m.nc + 2) + k)] = mapBits(&bits[(size_t)(s * m.bps)], m.bps);
    }
    return g;
}

// one OFDM symbol: carriers X[0 .. nc + 1] -> sps samples (cyclic prefix first), as codec2's idft (1 / M scaling)
void symbolOut(const Geometry& g, const cf* X, cf* out) {
    std::vector<cf> body((size_t)g.M);
    for (int n = 0; n < g.M; n++) {
        std::complex<double> acc(0, 0);
        for (int k = 0; k < g.nc + 2; k++) {
            if (X[k] == cf(0, 0)) continue;
            const double ph = 2 * kPi * (double)(g.nlower + k) * n / g.M;
            acc += std::complex<double>(X[k].real(), X[k].imag()) * std::complex<double>(std::cos(ph), std::sin(ph));
        }
        body[(size_t)n] = cf((float)(acc.real() / g.M), (float)(acc.imag() / g.M));
    }
    for (int i = 0; i < g.Ncp; i++) out[i] = body[(size_t)(g.M - g.Ncp + i)];
    for (int i = 0; i < g.M; i++) out[g.Ncp + i] = body[(size_t)i];
}

} // namespace

const OfdmMode& ofdmMode(int id) { return kModes[std::max(0, std::min(kNumOfdmModes - 1, id))]; }

int ofdmModeByName(const std::string& name) {
    std::string u;
    for (char c : name) u += (char)std::toupper((unsigned char)c);
    for (int i = 0; i < kNumOfdmModes; i++) if (u == kModes[i].name) return i;
    return -1;
}

const char* modeName(int id) {
    if (id == kMfsk) return "MFSK";
    if (id == kMfsk500) return "MFSK500";
    return id >= 0 && id < kNumOfdmModes ? kModes[id].name : "?";
}

int modePayloadBytes(int id) { return id == kMfsk || id == kMfsk500 ? 98 : id >= 0 && id < kNumOfdmModes ? geometry(id).payloadBytes : 0; }

const Geometry& geometry(int id) {
    static std::once_flag once;
    static std::vector<Geometry> g;
    std::call_once(once, [] { for (int i = 0; i < kNumOfdmModes; i++) g.push_back(build(i)); });
    return g[(size_t)std::max(0, std::min(kNumOfdmModes - 1, id))];
}

cf qpskMap(int b0, int b1) {
    // codec2 qpsk_mod: qpsk[(bits[1] << 1) | bits[0]] with bits[1] the first bit of the pair; 00 -> 1, 01 -> j, 10 -> -j, 11 -> -1
    static const cf q[4] = {cf(1, 0), cf(0, 1), cf(0, -1), cf(-1, 0)};
    return q[((b1 & 1) << 1) | (b0 & 1)];
}

cf qam16Map(int index4) { return cf(kQam16[index4 & 15][0], kQam16[index4 & 15][1]); }

const cf* qam16Table() {
    static cf t[16];
    static std::once_flag once;
    std::call_once(once, [] { for (int i = 0; i < 16; i++) t[i] = qam16Map(i); });
    return t;
}

int interleaverStep(int n) {
    int b = (int)std::floor(n / 1.62);
    b++;
    while (!isPrime(b)) b++;
    return b;
}

void ofdmRand(uint16_t* r, int n, uint64_t seed) {
    for (int i = 0; i < n; i++) {
        seed = (1103515245ull * seed + 12345ull) % 32768ull;
        r[i] = (uint16_t)seed;
    }
}

uint16_t frameCrc(const uint8_t* p, size_t n) { return crc::crc16CcittFalse(p, n); }

std::vector<uint8_t> makeFrame(int modeId, const uint8_t* payload, size_t n, uint16_t seed) {
    const Geometry& g = geometry(modeId);
    std::vector<uint8_t> f((size_t)g.frameBytes, 0);
    if (payload) std::memcpy(f.data(), payload, std::min(n, (size_t)g.payloadBytes));
    const uint16_t c = (uint16_t)(frameCrc(f.data(), (size_t)g.payloadBytes) ^ seed);
    f[(size_t)g.payloadBytes] = (uint8_t)(c >> 8);
    f[(size_t)g.payloadBytes + 1] = (uint8_t)c;
    return f;
}

std::vector<cf> modulateBurst(int modeId, const std::vector<uint8_t>& frameBytes, const ModOptions& o) {
    const Geometry& g = geometry(modeId);
    const OfdmMode& m = kModes[g.id];
    const LdpcCode& code = ldpcCode(m.code);
    // codeword: the used information bits (the frame, MSB first), then the parity of the code with the unused bits set to 1
    std::vector<uint8_t> info((size_t)code.k, 1), par((size_t)code.m);
    for (int i = 0; i < g.dataBits; i++) {
        const size_t byte = (size_t)(i >> 3);
        info[(size_t)i] = byte < frameBytes.size() ? (uint8_t)((frameBytes[byte] >> (7 - (i & 7))) & 1) : 0;
    }
    ldpcEncode(code, info.data(), par.data());
    std::vector<uint8_t> cw((size_t)g.codedBits);
    for (int i = 0; i < g.dataBits; i++) cw[(size_t)i] = info[(size_t)i];
    for (int i = 0; i < code.m; i++) cw[(size_t)(g.dataBits + i)] = par[(size_t)i];
    std::vector<cf> syms((size_t)g.codedSyms), inter((size_t)g.codedSyms);
    for (int s = 0; s < g.codedSyms; s++) syms[(size_t)s] = mapBits(&cw[(size_t)(s * g.bps)], g.bps);
    for (int i = 0; i < g.codedSyms; i++) inter[(size_t)(((int64_t)g.interleaverB * i) % g.codedSyms)] = syms[(size_t)i];
    std::vector<cf> pkt((size_t)g.symsPerPacket);
    int u = 0, p = 0;
    for (int s = 0; s < g.symsPerPacket; s++) pkt[(size_t)s] = g.isUw[(size_t)s] ? g.uwSyms[(size_t)u++] : inter[(size_t)p++];
    // the symbol rows: preamble frame, the packet's frames (pilots first), postamble frame
    const int rows = (m.np + 2) * m.ns, W = m.nc + 2;
    std::vector<cf> grid((size_t)(rows * W), cf(0, 0));
    std::copy(g.preamble.begin(), g.preamble.end(), grid.begin());
    int s = 0;
    for (int r = 0; r < m.np * m.ns; r++) {
        cf* row = &grid[(size_t)((m.ns + r) * W)];
        if (r % m.ns == 0) for (int k = 0; k < W; k++) row[k] = cf(g.pilots[(size_t)k], 0);
        else for (int k = 1; k <= m.nc; k++) row[k] = pkt[(size_t)s++];
    }
    std::copy(g.postamble.begin(), g.postamble.end(), grid.begin() + (size_t)((m.np + 1) * m.ns * W));
    std::vector<cf> x((size_t)(rows * g.sps));
    for (int r = 0; r < rows; r++) symbolOut(g, &grid[(size_t)(r * W)], &x[(size_t)(r * g.sps)]);
    // the transmitter's level, Hilbert clipper and band-pass (ofdm_hilbert_clipper), in units of OFDM_PEAK
    const TxShape& sh = kShape[g.id];
    const bool clip = o.clip && sh.clip;
    auto clipTo1 = [](std::vector<cf>& v) { for (cf& z : v) { const float a = std::abs(z); if (a > 1.f) z /= a; } };
    for (cf& z : x) z *= sh.ampScale / kOfdmPeak * (clip ? sh.clipGain1 : 1.f);
    if (clip) clipTo1(x);
    if (o.clip) {
        // windowed-sinc low-pass prototype (cut between the pass and stop edge), moved to the carriers' centre (or 1500 Hz)
        const double fc = sh.centreOnCarriers ? g.centreHz : 1500.0;
        const double cut = 0.5 * (sh.passHz + sh.stopHz) / 8000.0;
        const int taps = (int)std::lround(4.0 * 8000.0 / (sh.stopHz - sh.passHz)) | 1;
        std::vector<cf> h((size_t)taps);
        double sum = 0;
        for (int k = 0; k < taps; k++) {
            const double t = k - (taps - 1) * 0.5;
            const double sinc = t == 0 ? 2 * cut : std::sin(2 * kPi * cut * t) / (kPi * t);
            const double w = 0.42 - 0.5 * std::cos(2 * kPi * k / (taps - 1)) + 0.08 * std::cos(4 * kPi * k / (taps - 1));
            sum += sinc * w;
            h[(size_t)k] = cf((float)(sinc * w * std::cos(2 * kPi * fc / 8000.0 * t)), (float)(sinc * w * std::sin(2 * kPi * fc / 8000.0 * t)));
        }
        for (cf& v : h) v /= (float)sum;
        std::vector<cf> y(x.size(), cf(0, 0));
        const int half = (taps - 1) / 2;
        for (size_t n = 0; n < x.size(); n++) {
            cf acc(0, 0);
            for (int k = 0; k < taps; k++) {
                const int64_t i = (int64_t)n + half - k;
                if (i >= 0 && i < (int64_t)x.size()) acc += h[(size_t)k] * x[(size_t)i];
            }
            y[n] = acc;
        }
        x.swap(y);
        if (clip) for (cf& z : x) z *= sh.clipGain2;
        clipTo1(x);
    }
    for (cf& z : x) z *= o.peak;
    return x;
}

} // namespace mercury
} // namespace dect2
