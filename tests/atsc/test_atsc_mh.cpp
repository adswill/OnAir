// The ATSC-M/H presence detector on simulated transport streams: every slot pattern, lost packets, and streams that must not trigger it.
#include "dect2/atsc/atsc_mh.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static void pkt(std::vector<uint8_t>& v, int pid, bool err = false) { size_t n = v.size(); v.resize(n + 188, 0xA5); v[n] = 0x47; v[n + 1] = (uint8_t)((err ? 0x80 : 0) | (pid >> 8)); v[n + 2] = (uint8_t)pid; }
static std::vector<uint8_t> stream(int slots, int period, std::mt19937& r, int mhePid = 0x1FF6) {
    std::vector<uint8_t> v;
    const int pids[3] = {0x31, 0x34, 0x1FFB};
    for (int s = 0; s < slots; s++) {
        const bool g = period && s % period == 0;
        for (int i = 0; i < 156; i++) pkt(v, g && i < 118 ? mhePid : pids[r() % 3]);
    }
    return v;
}
int main() {
    std::mt19937 r(5);
    for (int per : {1, 2, 3, 4, 8, 16}) for (int pid : {0x1FF6, 0x1234}) for (size_t chunk : {1u, 7u, 156u, 4096u}) {
        auto v = stream(320, per, r, pid);
        AtscMhDetector d;
        for (size_t i = 0; i < v.size() / 188; i += chunk) d.feed(v.data() + i * 188, std::min(chunk, v.size() / 188 - i));
        CHECK(d.status().present && d.status().pid == pid, "period %d pid %x not found", per, pid);
        CHECK(std::abs(d.status().slotShare - 1.0 / per) < 0.02, "period %d share %.3f", per, d.status().slotShare);
        CHECK(d.status().groups >= (uint64_t)(320 / per - 1), "period %d groups %llu", per, (unsigned long long)d.status().groups);
    }
    {   // packet errors break single runs, the rest still count
        auto v = stream(320, 2, r);
        for (int k = 0; k < 20; k++) v[(r() % (v.size() / 188)) * 188 + 1] |= 0x80;
        AtscMhDetector d; d.feed(v.data(), v.size() / 188);
        CHECK(d.status().present, "not found with packet errors");
    }
    {   // no M/H: ordinary streams with long runs of one PID, nulls, and 118-packet runs off the slot grid
        auto v = stream(320, 0, r);
        std::vector<uint8_t> w;
        for (int n = 0; n < 40; n++) { for (int i = 0; i < 118; i++) pkt(w, 0x101); for (int i = 0; i < 37 + (n & 3); i++) pkt(w, 0x34); }
        std::vector<uint8_t> nulls; for (int s = 0; s < 40; s++) { for (int i = 0; i < 118; i++) pkt(nulls, 0x1FFF); for (int i = 0; i < 38; i++) pkt(nulls, 0x34); }
        std::vector<uint8_t> longrun; for (int i = 0; i < 20000; i++) pkt(longrun, 0x31);
        for (auto* s : {&v, &w, &nulls, &longrun}) { AtscMhDetector d; d.feed(s->data(), s->size() / 188); CHECK(!d.status().present, "false detection (pid %x)", d.status().pid); }
    }
    printf(fails ? "atsc m/h FAILED\n" : "atsc m/h passed\n");
    return fails ? 1 : 0;
}
