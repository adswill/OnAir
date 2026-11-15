// Yaesu Fusion against an independent implementation: three frames (header, header via VoIP, terminator) written by MMDVMHost's YSF code
// (g4klx/MMDVMHost, YSFFICH.cpp, YSFPayload.cpp, GPL-2.0-or-later, Copyright Jonathan Naylor G4KLX, credited in THIRD_PARTY.md), which interoperates with real
// radios. Our parser must read the FICH and the callsign blocks exactly as sent, and our builder must write the very same bits back. The test data are the
// frames' bytes only; no MMDVMHost code is part of OnAir. (The opposite direction, OnAir frames read by MMDVMHost, was checked when this test was written:
// FICH and header decoded for header and terminator frames.)
#include "dect2/fusion/fusion_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2::fusion;
#include "../common/test_util.h"

struct Vec { const char* name; const char* hex; };
static const Vec kVec[] = {
    {"header", "D471C9634D003CA4733C00239CA0040001D3955C003141EB800011F16570DFE35B10179E2F31C0D77363C015810F1C88B6245ABF96943584BBB41D5ABA469404B4BEA9A3376F8A451EABE675A5EEDFB8111D317604730686D527499DF43DEF0463251E49958E13660602D45581D1931373F63FA8958901B0"},
    {"header voip", "D471C9634D003F0C022F00227003F9000DD00C81003B1007B6001220066FDFE35B10179E2F31C0D77363C015810F1C88B6245ABF96943584BBB41D5ABA469404B4BEA9A3376F8A451EABE675A5EEDFB8111D317604730686D527499DF43DEF0463251E49958E13660602D45581D1931373F63FA8958901B0"},
    {"terminator", "D471C9634DCFBCA4D6524BA39C90D54FC1D374C68431419243C411F26478DFE35B10179E2F31C0D77363C015810F1C88B6245ABF96943584BBB41D5ABA469404B4BEA9A3376F8A451EABE675A5EEDFB8111D317604730686D527499DF43DEF0463251E49958E13660602D45581D1931373F63FA8958901B0"},
};

static std::vector<uint8_t> bitsOf(const char* hex) {
    std::vector<uint8_t> b;
    for (size_t i = 0; i + 1 < strlen(hex); i += 2) {
        const unsigned v = (unsigned)strtoul(std::string(hex + i, 2).c_str(), nullptr, 16);
        for (int k = 7; k >= 0; k--) b.push_back((uint8_t)((v >> k) & 1));
    }
    return b;
}

int main() {
    for (const Vec& v : kVec) {
        const std::vector<uint8_t> bits = bitsOf(v.hex);
        CHECK(bits.size() == 960, "%s: %zu bits", v.name, bits.size());
        uint64_t sync = 0;
        for (int i = 0; i < 40; i++) sync = (sync << 1) | bits[(size_t)i];
        CHECK(sync == kSyncWord, "%s: sync %llx", v.name, (unsigned long long)sync);
        FrameContent c;
        const bool ok = parseFrameBits(bits.data(), c);
        CHECK(ok && c.fichOk, "%s: FICH did not check", v.name);
        CHECK(c.units.size() == 2 && c.unitOk.size() == 2 && c.unitOk[0] && c.unitOk[1], "%s: DCH units %zu", v.name, c.units.size());
        if (c.units.size() == 2) {
            CHECK(trimField(c.units[0].data(), 10) == "CQCQCQ" && trimField(c.units[0].data() + 10, 10) == "LY2ABC", "%s: callsigns '%s' '%s'", v.name, trimField(c.units[0].data(), 10).c_str(), trimField(c.units[0].data() + 10, 10).c_str());
            CHECK(trimField(c.units[1].data(), 10) == "LY1XYZ", "%s: second block '%s'", v.name, trimField(c.units[1].data(), 10).c_str());
        }
        const std::string n = v.name;
        CHECK(c.fich.fi == (n == "terminator" ? 2 : 0), "%s: FI %d", v.name, c.fich.fi);
        CHECK(c.fich.voip == (n == "header voip" ? 1 : 0) && c.fich.ft == 6, "%s: VoIP %d FT %d", v.name, c.fich.voip, c.fich.ft);
        CHECK(c.fich.mr == (n == "header voip" ? 1 : 0), "%s: MR %d", v.name, c.fich.mr);
        std::vector<uint8_t> again;
        const bool built = buildFrameBits(c, again);
        CHECK(built && again == bits, "%s: our frame is not bit-identical to the one MMDVMHost wrote", v.name);
    }
    return testSummary("test_fusion_interop");
}
