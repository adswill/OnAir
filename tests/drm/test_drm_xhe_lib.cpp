// Optional xHE-AAC decoder plug-in: the symbol loader and the decode session with a stub library, and the xHE-AAC super frame parser with a
// hand built frame. A real libfdk-aac and a real DRM xHE-AAC recording are NOT used here, so the bitstream itself is not verified by this test.
#include "dect2/drm/drm_audio.h"
#include "dect2/drm/drm_fec.h"
#include "dect2/drm/drm_usac_lib.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <map>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::drm;

#include "../common/test_util.h"

static int gOpen = 0, gClose = 0, gFill = 0, gAscLen = 0;
static int gInfo[10] = {48000, 4, 2};
static void* stubOpen(int tt, unsigned layers) { if (tt != 0 || layers != 1) return nullptr; gOpen++; return &gInfo; }
static int stubConfig(void*, uint8_t** conf, const unsigned* len) { gAscLen = (int)len[0]; return (conf[0][0] & 0xF8) == 0xF8 ? 0 : 1; }   // the ASC starts with the escape audio object type
static int stubFill(void*, uint8_t**, const unsigned* size, unsigned* valid) { gFill++; *valid = size[0]; return 0; }
static int stubDecode(void*, int16_t* pcm, int n, unsigned) { for (int i = 0; i < n && i < 8; i++) pcm[i] = (int16_t)(i * 1000); return 0; }
static const int* stubInfo(void*) { return gInfo; }
static void stubClose(void*) { gClose++; }

static void testLoader() {
    std::map<std::string, void*> m = {{"aacDecoder_Open", (void*)&stubOpen}, {"aacDecoder_ConfigRaw", (void*)&stubConfig}, {"aacDecoder_Fill", (void*)&stubFill},
        {"aacDecoder_DecodeFrame", (void*)&stubDecode}, {"aacDecoder_GetStreamInfo", (void*)&stubInfo}, {"aacDecoder_Close", (void*)&stubClose}};
    auto res = [&](const char* n) -> void* { auto it = m.find(n); return it == m.end() ? nullptr : it->second; };
    UsacLib l = UsacLib::fromResolver(res);
    CHECK(l.ok(), "all six symbols resolved");
    m.erase("aacDecoder_Fill");
    CHECK(!UsacLib::fromResolver(res).ok(), "a missing symbol makes the library unusable");
    (void)UsacLib::load("/nonexistent/libfdk-aac-none.so");   // a missing library must not crash

    UsacSession s(l);
    std::vector<uint8_t> asc = {0xF9, 0x01, 0x02};
    CHECK(s.open(asc) && s.isOpen() && gOpen == 1 && gAscLen == 3, "session opens with the ASC");
    std::vector<int16_t> pcm; int rate = 0, ch = 0;
    CHECK(s.decode({0x80, 1, 2}, pcm, rate, ch) && gFill == 1, "decode ok");
    CHECK(rate == 48000 && ch == 2 && pcm.size() == 8 && pcm[3] == 3000, "pcm %zu rate %d ch %d", pcm.size(), rate, ch);
    s.close();
    CHECK(gClose == 1 && !s.isOpen(), "closed");
    CHECK(!s.decode({0x80}, pcm, rate, ch), "no decode when closed");
    CHECK(!s.open({0x00}), "the library refusing the config fails open");
}

static void testParser() {
    auto frame = [](std::vector<uint8_t> au) { const uint32_t c = crc16Bytes(au.data(), au.size()); au.push_back((uint8_t)(c >> 8)); au.push_back((uint8_t)c); return au; };
    const std::vector<uint8_t> a = frame({0x80, 0x11, 0x22}), b = frame({0x80, 0x33, 0x44});
    std::vector<uint8_t> sf;
    sf.push_back(0x20);                                                          // 2 frame borders, reservoir 0
    sf.push_back((uint8_t)crc8Bytes(sf.data(), 1));
    sf.insert(sf.end(), a.begin(), a.end());
    sf.insert(sf.end(), b.begin(), b.end());
    const int second = (int)a.size();                                            // border 2 sits right after frame a; border 1 at payload offset 0
    sf.push_back((uint8_t)((second << 4) >> 8)); sf.push_back((uint8_t)((second << 4) & 0xFF));
    sf.push_back(0); sf.push_back(0);                                            // the last directory element describes the first border
    XheSuperFrameParser p; std::vector<XheFrame> f;
    CHECK(p.parse(sf.data(), (int)sf.size(), f), "parse ok");
    CHECK(f.size() == 1 && f[0].crcOk && f[0].au.size() == 3 && f[0].au[1] == 0x11, "first frame complete with a good CRC (%zu frames)", f.size());
    std::vector<uint8_t> broken = sf; broken[1] ^= 1;
    CHECK(!p.parse(broken.data(), (int)broken.size(), f), "a bad header CRC is refused");
    SdcAudio s; s.coding = 3; s.mode = 0;
    CHECK(xheAscFromSdc(s).empty(), "no config -> no ASC");
}

// issue 266: a libfdk-aac put in the OnAir codecs folder (any usual or odd name) must be among the places tried
static void testCodecFolder() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "onair_fdk_test_home";
    fs::remove_all(root);
#if defined(__APPLE__)
    const fs::path dir = root / "Library/Application Support/OnAir/codecs";
#else
    const fs::path dir = root / "xdg/onair/codecs";
    setenv("XDG_DATA_HOME", (root / "xdg").c_str(), 1);
#endif
    setenv("HOME", root.c_str(), 1);
    fs::create_directories(dir);
    { FILE* f = fopen((dir / "Libfdk-AAC-custom.dll").c_str(), "w"); if (f) fclose(f); }
    { FILE* f = fopen((dir / "unrelated.dll").c_str(), "w"); if (f) fclose(f); }
    const auto c = UsacLib::codecFolderCandidates();
    const std::string d = dir.string() + "/";
    CHECK(std::find(c.begin(), c.end(), d + "libfdk-aac-2.dll") != c.end(), "usual name tried in the codecs folder");
    CHECK(std::find(c.begin(), c.end(), d + "Libfdk-AAC-custom.dll") != c.end(), "oddly named fdk-aac file in the codecs folder found");
    CHECK(std::find(c.begin(), c.end(), d + "unrelated.dll") == c.end(), "unrelated files are ignored");
    // issue 266/247: a file that is there but cannot load is reported with its reason, for the DRM panel
#if defined(__APPLE__)
    const std::string bad = d + "libfdk-aac.dylib";
#else
    const std::string bad = d + "libfdk-aac.so.2";
#endif
    { FILE* f = fopen(bad.c_str(), "w"); if (f) { fputs("not a library", f); fclose(f); } }
    (void)UsacLib::load();
    const UsacLib::Report r = UsacLib::lastReport();
    bool listed = false;
    for (const auto& s : r.failed) if (s.rfind(bad + ": ", 0) == 0) listed = true;
    CHECK(listed, "a broken libfdk-aac in the codecs folder is reported (%zu failures)", r.failed.size());
    const uint64_t g = UsacLib::generation();
    UsacLib::rescan();
    CHECK(UsacLib::generation() == g + 1, "rescan bumps the generation");
    fs::remove_all(root);
}

int main() {
    testLoader();
    testCodecFolder();
    testParser();
    return testSummary("drm_xhe_lib");
}
