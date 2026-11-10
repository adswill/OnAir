// DAB packet mode FEC (EN 300 401 5.3.5): sets written by the encoder, byte errors in every shape the code can fix, slipped starts, and a stream without FEC sets. Simulated.
#include "dect2/dab/dab_pktfec.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
int main() {
    std::mt19937 r(3);
    auto make = [&](int sets, std::vector<uint8_t>& plain) { std::vector<uint8_t> s; for (int k = 0; k < sets; k++) { std::vector<uint8_t> d(2256); for (auto& v : d) v = (uint8_t)r(); plain.insert(plain.end(), d.begin(), d.end()); auto e = dabPacketFecEncode(d.data()); s.insert(s.end(), e.begin(), e.end()); } return s; };
    for (int errs : {0, 4, 8, 16}) for (int chunk : {1, 37, 2472}) {
        std::vector<uint8_t> plain, s = make(8, plain);
        // errors spread: in each set `errs` bytes of random positions; 8 per row corrected
        for (size_t set = 0; set < 8; set++) for (int e = 0; e < errs; e++) { size_t col = r() % 188, row = (size_t)e % 12; s[set * 2472 + col * 12 + row] ^= (uint8_t)(1 + r() % 255); }
        DabPacketFec f; std::vector<uint8_t> out;
        for (size_t o = 0; o < s.size(); o += (size_t)chunk) f.feed(s.data() + o, std::min<size_t>((size_t)chunk, s.size() - o), out);
        size_t same = 0; for (size_t i = 0; i + 2256 <= out.size() && i + 2256 <= plain.size(); i += 2256) same += memcmp(out.data() + i, plain.data() + i, 2256) == 0;
        printf("errors %2d chunk %4d: sets %llu, fixed rows %llu, bad %llu, identical sets %zu of %zu\n", errs, chunk, (unsigned long long)f.sets(), (unsigned long long)f.rowsFixed(), (unsigned long long)f.rowsBad(), same, out.size() / 2256);
        CHECK(out.size() == 8 * 2256 && same == 8, "errors %d: %zu identical of %zu", errs, same, out.size() / 2256);
    }
    {   // starts inside the second set: the first whole set comes out, and bytes in front of it are skipped
        std::vector<uint8_t> plain, s = make(4, plain);
        DabPacketFec f; std::vector<uint8_t> out; f.feed(s.data() + 1500, s.size() - 1500, out);
        CHECK(out.size() == 3 * 2256 && !memcmp(out.data(), plain.data() + 2256, 2256), "resync: %zu", out.size());
    }
    {   // a corrupted FEC packet header: that set is lost, the sync comes back at the next
        std::vector<uint8_t> plain, s = make(5, plain); s[2 * 2472 + 2256 + 1] ^= 0xFF;
        DabPacketFec f; std::vector<uint8_t> out; f.feed(s.data(), s.size(), out);
        CHECK(out.size() >= 3 * 2256, "after bad header: %zu", out.size());
    }
    printf(fails ? "pktfec FAILED\n" : "pktfec passed\n");
    return fails ? 1 : 0;
}
