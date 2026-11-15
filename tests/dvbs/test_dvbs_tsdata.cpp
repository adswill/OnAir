// Other payloads of a transport stream: the AIT (HbbTV applications), SCTE-35 cues, EBU data in PES, DSM-CC and MPE PIDs, through the TS demultiplexer, and scrambled
// packets that are never looked into.
#include "dect2/dvbs/dvbs_ip.h"
#include "dect2/tv/ts.h"
#include "dect2/tv/ts_data.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace dect2;
#include "../common/test_util.h"

static void cat(std::vector<uint8_t>& a, const std::vector<uint8_t>& b) { a.insert(a.end(), b.begin(), b.end()); }
static std::vector<uint8_t> desc(int tag, const std::vector<uint8_t>& b) { std::vector<uint8_t> o{(uint8_t)tag, (uint8_t)b.size()}; cat(o, b); return o; }
static std::vector<uint8_t> longSection(int tid, int ext, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> s{(uint8_t)tid, 0, 0, (uint8_t)(ext >> 8), (uint8_t)ext, 0xC1, 0, 0};
    cat(s, body);
    const size_t len = s.size() - 3 + 4;
    s[1] = (uint8_t)(0xB0 | (len >> 8)); s[2] = (uint8_t)len;
    const uint32_t c = dvbsip::crc32Mpeg2(s.data(), s.size());
    for (int sh = 24; sh >= 0; sh -= 8) s.push_back((uint8_t)(c >> sh));
    return s;
}
static void feed(TsDemux& d, uint16_t pid, const std::vector<uint8_t>& sec, uint8_t& cc) { for (auto& p : dvbsip::packetizeSection(pid, cc, sec)) d.feed(p.data()); }

int main() {
    TsDemux d;
    uint8_t cA = 0, cS = 0, cD = 0, cM = 0;
    // AIT: HbbTV (type 0x10), two applications
    {
        std::vector<uint8_t> apps;
        auto app = [&](uint32_t org, int id, int ctl, const char* name, const char* url, const char* path) {
            apps.push_back((uint8_t)(org >> 24)); apps.push_back((uint8_t)(org >> 16)); apps.push_back((uint8_t)(org >> 8)); apps.push_back((uint8_t)org);
            apps.push_back((uint8_t)(id >> 8)); apps.push_back((uint8_t)id); apps.push_back((uint8_t)ctl);
            std::vector<uint8_t> ds;
            cat(ds, desc(0x00, {0x05, 0x00, 0x00, 0x01, 0x00, 0x00, 0xE0 | 0x1F | 0, 0x01}));   // profiles: one (0x0000, version 1.0.0), visibility 3, priority 1
            std::vector<uint8_t> nm{'e', 'n', 'g', (uint8_t)strlen(name)};
            for (const char* c = name; *c; c++) nm.push_back((uint8_t)*c);
            cat(ds, desc(0x01, nm));
            std::vector<uint8_t> tp{0x00, 0x03, 0x01, (uint8_t)strlen(url)};
            for (const char* c = url; *c; c++) tp.push_back((uint8_t)*c);
            tp.push_back(0);
            cat(ds, desc(0x02, tp));
            cat(ds, desc(0x15, std::vector<uint8_t>(path, path + strlen(path))));
            apps.push_back((uint8_t)(0xF0 | (ds.size() >> 8))); apps.push_back((uint8_t)ds.size());
            cat(apps, ds);
        };
        app(0x0000000A, 0x0001, 1, "Red Button", "http://hbbtv.example/", "index.html");
        app(0x0000000A, 0x0002, 2, "Teletext", "http://hbbtv.example/tt/", "tt.html");
        std::vector<uint8_t> body{0xF0, 0x00};   // no common descriptors
        body.push_back((uint8_t)(0xF0 | (apps.size() >> 8))); body.push_back((uint8_t)apps.size()); cat(body, apps);
        feed(d, 0x0400, longSection(0x74, 0x0010, body), cA);
    }
    // SCTE-35: splice insert (out of network, 30 s) and a time signal
    {
        auto scteSec = [&](int cmd, const std::vector<uint8_t>& command) {
            std::vector<uint8_t> s{0xFC, 0x30, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xF0, 0, (uint8_t)cmd};
            // table, section length 2 bytes, version 0, flags+pts adjustment 4 bytes (0), cw index 0xFF, tier/length filled below
            s = {0xFC, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xF0, 0x00, 0x00, (uint8_t)cmd};
            s[11] = (uint8_t)(0xF0 | ((command.size() >> 8) & 0x0F)); s[12] = (uint8_t)command.size();
            // the layout: [0]=table [1,2]=length [3]=version [4..8]=enc/alg/pts_adjustment [9]=cw [10,11,12]=tier+command length [13]=command type
            s = {0xFC, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, (uint8_t)(0xF0 | ((command.size() >> 8) & 0x0F)), (uint8_t)command.size(), (uint8_t)cmd};
            cat(s, command);
            s.push_back(0); s.push_back(0);   // descriptor loop length
            s.push_back(0); s.push_back(0); s.push_back(0); s.push_back(0);   // CRC placeholder
            const size_t len = s.size() - 3;
            s[1] = (uint8_t)(0x30 | (len >> 8)); s[2] = (uint8_t)len;
            return s;
        };
        std::vector<uint8_t> ins{0x00, 0x00, 0x12, 0x34, 0x00, 0xE0 | 0x20 | 0x10 | 0, 0};   // event id 0x1234, not cancelled, out of network, duration flag, immediate
        ins = {0x00, 0x00, 0x12, 0x34, 0x00, (uint8_t)(0x80 | 0x40 | 0x20), 0xFE, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x00, 0x29, 0x5A, 0x88};   // program splice, time specified 0x0E...
        // splice_time: time_specified(1) reserved(6) pts(33): 0xFE 0x00 0x00 0x00 0x00 -> pts 0 with bit set... build explicitly
        const uint64_t pts = 90000 * 100;   // 100 s
        std::vector<uint8_t> st{(uint8_t)(0x80 | 0x7E | ((pts >> 32) & 1)), (uint8_t)(pts >> 24), (uint8_t)(pts >> 16), (uint8_t)(pts >> 8), (uint8_t)pts};
        const uint64_t dur = 90000 * 30;
        std::vector<uint8_t> bd{(uint8_t)(0x80 | 0x7E | ((dur >> 32) & 1)), (uint8_t)(dur >> 24), (uint8_t)(dur >> 16), (uint8_t)(dur >> 8), (uint8_t)dur};
        std::vector<uint8_t> si{0x00, 0x00, 0x12, 0x34, 0x00, (uint8_t)(0x80 | 0x40 | 0x20)};
        cat(si, st); cat(si, bd);
        si.push_back(0); si.push_back(1); si.push_back(0); si.push_back(0);   // unique program id, avail num, avails expected
        feed(d, 0x0500, scteSec(0x05, si), cS);
        std::vector<uint8_t> ts = st;
        feed(d, 0x0500, scteSec(0x06, ts), cS);
    }
    // EBU teletext in a PES on its own PID, and a video PES
    {
        auto pes = [&](uint16_t pid, int sid, const std::vector<uint8_t>& payload, uint8_t& cc) {
            std::vector<uint8_t> p(188, 0xFF);
            p[0] = 0x47; p[1] = (uint8_t)(0x40 | (pid >> 8)); p[2] = (uint8_t)pid; p[3] = (uint8_t)(0x10 | (cc++ & 15));
            std::vector<uint8_t> e{0, 0, 1, (uint8_t)sid, 0, 0, 0x84, 0x80, 0x05, 0x21, 0, 1, 0, 1};
            cat(e, payload);
            memcpy(&p[4], e.data(), std::min<size_t>(e.size(), 184));
            d.feed(p.data());
        };
        pes(0x0600, 0xBD, {0x10, 0x02, 0x2C}, cD);     // data_identifier 0x10, data unit 0x02: EBU teletext
        pes(0x0601, 0xBD, {0x10, 0x03, 0x2C}, cD);     // subtitles
        pes(0x0602, 0xE0, {0, 0, 0, 1}, cD);           // video
    }
    // a DSM-CC section and an MPE section on their PIDs
    {
        feed(d, 0x0700, longSection(0x3B, 0, std::vector<uint8_t>(20, 0)), cM);
        uint8_t c2 = 0;
        const uint8_t mac[6] = {1, 0, 0x5E, 1, 1, 1}, a[4] = {10, 0, 0, 1}, b[4] = {239, 1, 1, 1};
        feed(d, 0x0701, dvbsip::buildMpeSection(mac, dvbsip::buildUdpV4(a, b, 1, 2, {1, 2, 3}, true)), c2);
    }
    // a scrambled packet on a private PID is not looked into
    {
        std::vector<uint8_t> p(188, 0x55);
        p[0] = 0x47; p[1] = 0x48; p[2] = 0x00; p[3] = 0x90;   // pid 0x800, scrambled, payload
        d.feed(p.data());
    }
    const TsSnapshot sn = d.snapshot();
    CHECK(sn.data != nullptr, "payloads recognised");
    if (!sn.data) return testSummary("dvbs tsdata");
    auto kindOf = [&](int pid) { for (const auto& p : sn.data->pids) if (p.pid == pid) return p.kind; return std::string(); };
    CHECK(kindOf(0x0400) == "AIT (application signalling)", "AIT PID: %s", kindOf(0x0400).c_str());
    CHECK(kindOf(0x0500) == "SCTE-35 (splice information)", "SCTE-35 PID: %s", kindOf(0x0500).c_str());
    CHECK(kindOf(0x0600) == "EBU teletext" && kindOf(0x0601) == "EBU teletext subtitles" && kindOf(0x0602) == "PES video", "EBU data and video: %s / %s / %s", kindOf(0x0600).c_str(), kindOf(0x0601).c_str(), kindOf(0x0602).c_str());
    CHECK(kindOf(0x0700) == "DSM-CC carousel" && kindOf(0x0701) == "MPE (IP data)", "DSM-CC and MPE PIDs");
    CHECK(kindOf(0x0800) == "scrambled (not examined)", "a scrambled PID is only labelled: %s", kindOf(0x0800).c_str());
    // AIT
    CHECK(sn.data->ait.size() == 1 && sn.data->ait[0].appType == 0x0010 && sn.data->ait[0].typeName == "HbbTV" && sn.data->ait[0].apps.size() == 2, "one HbbTV application table with two applications");
    if (sn.data->ait.size() == 1 && sn.data->ait[0].apps.size() == 2) {
        const auto& a = sn.data->ait[0].apps;
        CHECK(a[0].orgId == 10 && a[0].appId == 1 && a[0].controlName == "autostart" && a[0].name == "Red Button" && a[0].lang == "eng" && a[0].baseUrl == "http://hbbtv.example/" && a[0].initialPath == "index.html", "application 1: %s %s %s", a[0].name.c_str(), a[0].baseUrl.c_str(), a[0].initialPath.c_str());
        CHECK(a[1].appId == 2 && a[1].controlName == "present" && a[1].name == "Teletext" && !a[1].transports.empty() && a[1].transports[0] == "HTTP http://hbbtv.example/tt/", "application 2");
    }
    // SCTE-35
    CHECK(sn.data->scte.size() == 2, "two SCTE-35 command kinds: %zu", sn.data->scte.size());
    for (const auto& c : sn.data->scte) {
        if (c.command == 0x05) CHECK(c.eventId == 0x1234 && c.outOfNetwork && c.hasPts && std::abs(c.ptsSec - 100.0) < 0.01 && std::abs(c.durationSec - 30.0) < 0.01, "splice insert: event %x out %d pts %.2f duration %.2f", c.eventId, c.outOfNetwork, c.ptsSec, c.durationSec);
        if (c.command == 0x06) CHECK(c.hasPts && std::abs(c.ptsSec - 100.0) < 0.01, "time signal pts %.2f", c.ptsSec);
    }
    return testSummary("dvbs tsdata");
}
