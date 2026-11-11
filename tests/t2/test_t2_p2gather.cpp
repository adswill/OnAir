// p2Gather (dect2/t2/t2l1.h) with the L1-post size of a decoded L1-pre: a valid one gives back what p2Distribute spread over the P2 symbols, an
// impossible one (more cells than the P2 symbols hold, negative, the symbols missing) must stay inside the buffers. Before the fix such a size,
// from a pre that passed its CRC under the wrong carrier-mode hypothesis, wrote past the end of the stream (iq_modes, DVB-T2 with a DC spike: heap
// corruption on Linux and Windows, ASan heap-buffer-overflow in p2Gather). Run under ASan (DECT2_SANITIZE) a write out of bounds aborts here.
#include "dect2/t2/t2l1.h"
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    std::mt19937 g(5);
    // round trip: P2 symbols and cells per P2 symbol of the FFT sizes (EN 302 755 table 45 data cells, SISO) and L1-post sizes up to the room left
    struct Shape { int nP2, cP2; };
    const Shape shapes[] = {{16, 558}, {8, 1118}, {4, 2236}, {2, 4472}, {1, 8944}, {1, 22432}, {2, 11216}};
    for (const Shape& s : shapes)
        for (int post : {0, 350, 1200, 4000}) {
            if (1840 + post > s.nP2 * s.cP2) continue;
            const int nPost = post / s.nP2 * s.nP2;
            std::vector<cf32> in((size_t)s.nP2 * s.cP2);
            for (auto& c : in) c = cf32((float)(g() % 1000), (float)(g() % 1000));
            std::vector<std::vector<cf32>> sym(s.nP2, std::vector<cf32>(s.cP2));
            p2Distribute(in, s.nP2, s.cP2, 1840, nPost, sym);
            std::vector<cf32> out;
            p2Gather(sym, s.nP2, s.cP2, 1840, nPost, out);
            CHECK(out == in, "nP2 %d cP2 %d post %d: the round trip differs", s.nP2, s.cP2, nPost);
        }
    // impossible sizes: no write or read outside, the stream keeps nP2 * cP2 cells
    for (const Shape& s : shapes)
        for (int post : {s.nP2 * s.cP2, s.nP2 * s.cP2 * 3, 65535, 1 << 20, -7}) {
            std::vector<std::vector<cf32>> sym(s.nP2, std::vector<cf32>(s.cP2, cf32(1, 0)));
            std::vector<cf32> out;
            p2Gather(sym, s.nP2, s.cP2, 1840, post, out);
            CHECK(out.size() == (size_t)s.nP2 * s.cP2, "nP2 %d post %d: %zu cells", s.nP2, post, out.size());
            p2Gather(sym, s.nP2, s.cP2, s.nP2 * s.cP2 + 100, post, out);   // even the pre larger than the symbols
            CHECK(out.size() == (size_t)s.nP2 * s.cP2, "nP2 %d pre too large: %zu cells", s.nP2, out.size());
        }
    {   // fewer symbols, or shorter ones, than nP2 / cP2 say
        std::vector<std::vector<cf32>> sym(3, std::vector<cf32>(100));
        std::vector<cf32> out;
        p2Gather(sym, 16, 558, 1840, 2000, out);
        CHECK(out.size() == (size_t)16 * 558, "missing symbols: %zu cells", out.size());
        std::vector<std::vector<cf32>> shortSym(16, std::vector<cf32>(100));
        p2Gather(shortSym, 16, 558, 1840, 2000, out);
        CHECK(out.size() == (size_t)16 * 558, "short symbols: %zu cells", out.size());
    }
    printf("t2_p2gather: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
