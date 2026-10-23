// Mercury LDPC codes: the tables against codec2's own decoder test vectors (an independent check of the converted tables, the bit order
// and the encoder), the encoder against every parity check, the decoder on noisy codewords of every code, and the 16200 / 9720 code
// built from the DVB-S2 table against its size.
#include "dect2/mercury/mercury_phy.h"
#include "dect2/dsp/portable_rng.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::mercury;
#include "../common/test_util.h"

namespace {
#include "mercury_ldpc_vectors.inc"

// does the full codeword satisfy every check of the code?
int unsatisfied(const LdpcCode& c, const std::vector<uint8_t>& cw) {
    int bad = 0;
    for (int p = 0; p < c.m; p++) {
        int s = cw[(size_t)(c.k + p)] ^ (p ? cw[(size_t)(c.k + p - 1)] : 0);
        for (int e = c.rowStart[(size_t)p]; e < c.rowStart[(size_t)p + 1]; e++) s ^= cw[(size_t)c.rowCols[(size_t)e]];
        bad += s;
    }
    return bad;
}

void vector(int id, const float* in, const char* outBits) {
    const LdpcCode& c = ldpcCode(id);
    LdpcDecoder d(id);
    std::vector<uint8_t> bits((size_t)c.n);
    int it = 0;
    const bool ok = d.decode(in, 100, bits.data(), &it);
    int diff = 0;
    std::vector<uint8_t> ref((size_t)c.n);
    for (int i = 0; i < c.n; i++) { ref[(size_t)i] = (uint8_t)(outBits[i] - '0'); diff += ref[(size_t)i] != bits[(size_t)i]; }
    printf("  %-16s n %5d k %5d: decoded in %d iterations, %d bits differ from codec2's result\n", c.name, c.n, c.k, it, diff);
    CHECK(ok, "%s: the test vector did not decode", c.name);
    CHECK(diff == 0, "%s: %d bits differ", c.name, diff);
    CHECK(unsatisfied(c, ref) == 0, "%s: codec2's decoded codeword fails %d checks of the converted table", c.name, unsatisfied(c, ref));
    // the encoder gives the same parity from codec2's information bits
    std::vector<uint8_t> par((size_t)c.m);
    ldpcEncode(c, ref.data(), par.data());
    int pd = 0;
    for (int i = 0; i < c.m; i++) pd += par[(size_t)i] != ref[(size_t)(c.k + i)];
    CHECK(pd == 0, "%s: encoder parity differs in %d bits", c.name, pd);
}
} // namespace

int main() {
    vector(kH256x768, kVec256x768In, kVec256x768Out);
    vector(kH256x512, kVec256x512In, kVec256x512Out);
    vector(kH128x256, kVec128x256In, kVec128x256Out);
    vector(kHra56x56, kVecHra56In, kVecHra56Out);
    vector(kH4096x8192, kVec4096x8192In, kVec4096x8192Out);

    // every code: random information, encode, all checks hold; BPSK at an Eb/N0 well above the threshold decodes
    PortableRng rng(7);
    for (int id = 0; id < kNumLdpcCodes; id++) {
        const LdpcCode& c = ldpcCode(id);
        CHECK(c.rowStart.size() == (size_t)c.m + 1, "%s: %zu rows", c.name, c.rowStart.size() - 1);
        std::vector<uint8_t> cw((size_t)c.n);
        for (int i = 0; i < c.k; i++) cw[(size_t)i] = (uint8_t)(rng() & 1);
        ldpcEncode(c, cw.data(), cw.data() + c.k);
        CHECK(unsatisfied(c, cw) == 0, "%s: encoded word fails checks", c.name);
        const double rate = (double)c.k / c.n, ebn0 = std::pow(10.0, 3.0 / 10);
        const double sigma = std::sqrt(1.0 / (2 * rate * ebn0));
        std::vector<float> llr((size_t)c.n);
        for (int i = 0; i < c.n; i++) {
            const double x = cw[(size_t)i] ? -1.0 : 1.0, y = x + sigma * rng.gauss();
            llr[(size_t)i] = (float)(2 * y / (sigma * sigma));
        }
        LdpcDecoder d(id);
        std::vector<uint8_t> out((size_t)c.n);
        int it = 0;
        const bool ok = d.decode(llr.data(), 100, out.data(), &it);
        int e = 0;
        for (int i = 0; i < c.n; i++) e += out[(size_t)i] != cw[(size_t)i];
        printf("  %-16s 3 dB Eb/N0: %s after %d iterations, %d errors\n", c.name, ok ? "decoded" : "FAILED", it, e);
        CHECK(ok && e == 0, "%s: noisy codeword not decoded (%d errors)", c.name, e);
    }
    // DVB-S2 short 3/5: 162 addresses x 360 information bits, as many edges as codec2's H_16200_9720 has
    CHECK(ldpcCode(kH16200x9720).rowCols.size() == 58320u, "16200 / 9720: %zu edges", ldpcCode(kH16200x9720).rowCols.size());
    // every mode: the sizes Mercury lists (payload bytes per frame)
    const int bytes[kNumOfdmModes] = {14, 510, 126, 54, 14, 3, 30, 14, 1180, 1213};
    for (int id = 0; id < kNumOfdmModes; id++) {
        const Geometry& g = geometry(id);
        printf("  %-8s nc %2d np %2d M %3d Ncp %2d carriers %4.0f..%4.0f Hz, %5d coded bits, %4d payload bytes, burst %.2f s\n", ofdmMode(id).name,
               g.nc, g.np, g.M, g.Ncp, (g.nlower + 1) * g.rs, (g.nlower + g.nc) * g.rs, g.codedBits, g.payloadBytes, g.burstSeconds);
        CHECK(g.payloadBytes == bytes[id], "%s: %d payload bytes, Mercury lists %d", ofdmMode(id).name, g.payloadBytes, bytes[id]);
        CHECK(g.dataBits % 8 == 0 && g.codedBits % g.bps == 0, "%s: sizes", ofdmMode(id).name);
    }
    return testSummary("mercury ldpc");
}
