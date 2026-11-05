// GK-2A xRIT layer: KMA header records (7, 128), key file parsing, the assembler on the test files (plain and DES-encrypted pictures, the
// key arriving late), channel names.
#include "dect2/gk2a/gk2a_gen.h"
#include "dect2/goes/goes_proto.h"
#include "dect2/meteor/meteor_proto.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<std::array<uint8_t, goes::kVcduBytes>> vcdus(bool enc) {
    goes::Mux mux(0x68);
    uint16_t c = 0;
    for (const auto& f : gk2aTestFiles(enc)) mux.addFile(f.vcid, goesFileOut(f, c++));
    std::vector<std::array<uint8_t, goes::kVcduBytes>> v;
    std::array<uint8_t, goes::kVcduBytes> d{};
    while (mux.next(d.data())) v.push_back(d);
    return v;
}
static bool waitDone(goes::Assembler& a, size_t products, double s = 5) {
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < s) {
        size_t done = 0;
        for (const auto& p : a.products()) { std::lock_guard<std::mutex> l(p->mu); done += p->complete; }
        if (done >= products) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

int main() {
    CHECK(goes::gk2aChannelIndex("IMG_FD_143_IR105_20181227_030000_01.lrit") == 12 && goes::gk2aChannelIndex("IMG_FD_143_VI006_x") == 2 && goes::gk2aChannelIndex("ADD_ANT_143") == -1, "channel index");
    CHECK(std::string(goes::gk2aChannelName(0)) == "VI004" && std::string(goes::gk2aChannelName(15)) == "IR133", "channel names");
    {   // headers
        goes::Headers h;
        h.fileType = 0; h.annotation = "IMG_FD_143_IR105_20181227_030000_03.lrit";
        h.hasImage = true; h.bpp = 12; h.columns = 2200; h.lines = 220; h.compression = 1;
        h.hasSeg = true; h.segSeq = 3; h.maxSeg = 10; h.startLine = 440; h.keyNo = 0x1F2A;
        h.hasTime = true; h.days = 25141; h.msOfDay = 1234;
        const auto b = goes::buildHeaders(h, 1);
        goes::Headers r;
        CHECK(goes::parseHeaders(b.data(), b.size(), r, 1), "parse");
        CHECK(r.hasSeg && r.segSeq == 3 && r.maxSeg == 10 && r.startLine == 440 && r.keyNo == 0x1F2A && r.columns == 2200 && r.lines == 220 && r.bpp == 12 && r.compression == 1 && r.annotation == h.annotation, "fields");
        goes::Headers g;   // a GOES reader sees the same bytes as other records: nothing of the KMA ones
        goes::parseHeaders(b.data(), b.size(), g, 0);
        CHECK(g.keyNo == 0, "no key header in the GOES dialect");
    }
    {   // key files
        const auto k = gk2aTestKeyFile();
        const auto p = goes::parseKeyFile(k);
        CHECK(p.size() == 1 && p[0].first == 1 && p[0].second == gk2aTestKey(), "binary key file");
        const std::string t = "# keys\n0001:4f6e41697247 4b32\n0x2: 0011223344556677\nbad line\n";
        const auto q = goes::parseKeyFile(std::vector<uint8_t>(t.begin(), t.end()));
        CHECK(q.size() == 1 || q.size() == 2, "text key file: %zu", q.size());
        CHECK(!q.empty() && q[0].first == 1 && q[0].second == gk2aTestKey(), "text key 1");
        CHECK(goes::parseKeyFile({1, 2, 3}).empty(), "junk");
    }
    int pw, ph;
    const std::vector<uint16_t> ir = gk2aTestPicture(0, pw, ph), vi = gk2aTestPicture(1, pw, ph);
    auto checkPic = [&](const goes::Product& p, const std::vector<uint16_t>& pic, const char* what) {
        std::lock_guard<std::mutex> l(p.mu);
        bool same = p.w == pw && p.h == ph && p.pix.size() == pic.size();
        for (size_t i = 0; same && i < pic.size(); i++) same = p.pix[i] == (pic[i] >> 2);
        CHECK(same && p.complete && !p.errors && p.rowsDone == ph, "%s: picture %dx%d rows %d", what, p.w, p.h, p.rowsDone);
    };
    {   // plain
        goes::Assembler a;
        a.setDialect(1);
        for (const auto& v : vcdus(false)) a.push(v.data());
        CHECK(waitDone(a, 4), "all products complete");
        int pics = 0, texts = 0;
        for (const auto& p : a.products()) {
            if (p->channelIdx == 12) { checkPic(*p, ir, "IR105"); pics++; CHECK(p->segsTotal == 4 && p->segsDone == 4 && p->kind == goes::KFullDisk, "segments"); }
            else if (p->channelIdx == 2) { checkPic(*p, vi, "VI006"); pics++; }
            else if (p->kind == goes::KAdmin) { texts++; CHECK(p->text.find("OnAir GK-2A") != std::string::npos, "text"); }
        }
        CHECK(pics == 2 && texts == 2, "%d pictures, %d texts", pics, texts);
        CHECK(a.crcErrors == 0 && a.fileErrors == 0 && a.files == 10, "%llu files, %llu CRC, %llu errors", (unsigned long long)a.files, (unsigned long long)a.crcErrors, (unsigned long long)a.fileErrors);
    }
    {   // encrypted visible channel: locked, then the key arrives
        goes::Assembler a;
        a.setDialect(1);
        for (const auto& v : vcdus(true)) a.push(v.data());
        waitDone(a, 3);
        std::shared_ptr<goes::Product> vis;
        for (const auto& p : a.products()) if (p->channelIdx == 2) vis = p;
        CHECK(vis != nullptr, "visible product exists");
        if (vis) {
            std::lock_guard<std::mutex> l(vis->mu);
            CHECK(vis->needKey && vis->segsLocked == 4 && vis->rowsDone == 0 && vis->keyNo == 1 && vis->note.find("key 1") != std::string::npos, "locked: needKey %d locked %d note '%s'", vis->needKey, vis->segsLocked, vis->note.c_str());
        }
        a.setKeys({{2, gk2aTestKey()}});      // a key for another number does nothing
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (vis) { std::lock_guard<std::mutex> l(vis->mu); CHECK(vis->needKey && vis->rowsDone == 0, "wrong key number"); }
        a.setKeys({{1, gk2aTestKey()}});
        CHECK(waitDone(a, 4), "decoded after the key");
        if (vis) { checkPic(*vis, vi, "VI006 after the key"); std::lock_guard<std::mutex> l(vis->mu); CHECK(!vis->needKey && vis->segsLocked == 0 && vis->note.empty(), "unlocked"); }
        for (const auto& p : a.products()) if (p->channelIdx == 12) checkPic(*p, ir, "IR105");
    }
    {   // key first, then the files
        goes::Assembler a;
        a.setDialect(1);
        a.setKeys({{1, gk2aTestKey()}});
        for (const auto& v : vcdus(true)) a.push(v.data());
        CHECK(waitDone(a, 4), "decoded with the key loaded");
        for (const auto& p : a.products()) if (p->channelIdx == 2) checkPic(*p, vi, "VI006 keyed");
    }
    {   // a wrong key gives garbage, not a crash, and says so
        goes::Assembler a;
        a.setDialect(1);
        a.setKeys({{1, std::array<uint8_t, 8>{1, 2, 3, 4, 5, 6, 7, 8}}});
        for (const auto& v : vcdus(true)) a.push(v.data());
        waitDone(a, 4);
        bool noted = false;
        for (const auto& p : a.products()) if (p->channelIdx == 2) { std::lock_guard<std::mutex> l(p->mu); noted = p->errors || !p->note.empty(); }
        CHECK(noted, "a wrong key is reported as an error");
    }
    return testSummary("gk2a proto");
}
