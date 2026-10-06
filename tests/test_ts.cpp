// BB frame <-> transport stream round trips (normal and high-efficiency mode, with lost frames) and PSI/SI parsing.
#include "dect2/bbunpack.h"
#include "dect2/ts.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint8_t crc8(const uint8_t* p, int n) {
    unsigned crc = 0;
    for (int i = 0; i < n; i++) for (int b = 7; b >= 0; b--) { unsigned fb = ((crc >> 7) & 1) ^ ((p[i] >> b) & 1); crc = (crc << 1) & 0xff; if (fb) crc ^= 0xD5; }
    return crc;
}

// Pack a TS (sequence of 188-byte packets) into BB frames the way a transmitter does.
struct Packer {
    bool hem;
    int dfBytes;
    size_t pos = 0;           // next byte of the continuous user-packet stream
    std::vector<uint8_t> stream;
    uint8_t crc = 0;
    // build the byte stream of user packets
    void load(const std::vector<uint8_t>& ts) {
        stream.clear();
        for (size_t i = 0; i + 188 <= ts.size(); i += 188) {
            if (hem) stream.insert(stream.end(), ts.begin() + i + 1, ts.begin() + i + 188);
            else { stream.push_back(crc); stream.insert(stream.end(), ts.begin() + i + 1, ts.begin() + i + 188); crc = crc8(&ts[i + 1], 187); }
        }
    }
    bool next(std::vector<uint8_t>& bits, BbHeader& h) {
        if (pos + dfBytes > stream.size()) return false;
        const int stride = hem ? 187 : 188;
        h = BbHeader();
        h.hem = hem; h.dfl = dfBytes * 8; h.upl = 1504; h.issyi = hem ? 1 : 0;
        size_t rem = pos % stride;
        h.syncd = rem == 0 ? 0 : (int)((stride - rem) * 8);
        std::vector<uint8_t> hdr(80);
        buildBbHeader(h, hdr.data());
        if (hem) { // HEM: CRC mode bit
            hdr[79] ^= 1;
        }
        bits.assign(80 + dfBytes * 8, 0);
        std::copy(hdr.begin(), hdr.end(), bits.begin());
        for (int i = 0; i < dfBytes; i++) for (int b = 0; b < 8; b++) bits[80 + i * 8 + b] = (stream[pos + i] >> (7 - b)) & 1;
        pos += dfBytes;
        h.crcOk = true;
        return true;
    }
};

static std::vector<uint8_t> makeTs(int n, std::mt19937& rng) {
    std::vector<uint8_t> ts;
    for (int i = 0; i < n; i++) {
        uint8_t p[188];
        p[0] = 0x47; p[1] = 0x01; p[2] = (uint8_t)(0x00 + (i % 3)); p[3] = 0x10 | (i & 15);
        for (int k = 4; k < 188; k++) p[k] = rng() & 0xFF;
        ts.insert(ts.end(), p, p + 188);
    }
    return ts;
}

static void roundTrip(bool hem, std::mt19937& rng) {
    auto ts = makeTs(400, rng);
    Packer pk; pk.hem = hem; pk.dfBytes = 3000 + (hem ? 3 : 7);
    pk.load(ts);
    BbUnpacker u;
    std::vector<uint8_t> got;
    u.setSink([&](const uint8_t* p) { got.insert(got.end(), p, p + 188); });
    std::vector<uint8_t> bits; BbHeader h;
    int frame = 0, lostFrame = 7;
    std::vector<int> lostAt;
    while (pk.next(bits, h)) {
        if (frame == lostFrame) u.lost(); else u.push(bits, h);
        frame++;
    }
    // everything except the packets overlapping the lost frame must come out identical, in order
    size_t matched = 0;
    for (size_t i = 0; i + 188 <= got.size(); i += 188) {
        bool found = false;
        for (size_t j = matched; j + 188 <= ts.size(); j += 188) if (!memcmp(&got[i], &ts[j], 188)) { matched = j + 188; found = true; break; }
        CHECK(found, "%s: output packet %zu does not appear in order in the input", hem ? "HEM" : "NM", i / 188);
        if (!found) break;
    }
    const int stride = hem ? 187 : 188;
    size_t sentBytes = (pk.stream.size() / pk.dfBytes) * pk.dfBytes;
    size_t expectedMin = sentBytes / stride - (pk.dfBytes / stride + 3);
    CHECK(got.size() / 188 >= expectedMin, "%s: only %zu packets recovered (expected at least %zu)", hem ? "HEM" : "NM", got.size() / 188, expectedMin);
    CHECK(u.stats().crcErrors == 0, "%s: unexpected CRC-8 errors %llu", hem ? "HEM" : "NM", (unsigned long long)u.stats().crcErrors);
    printf("%s round trip: %zu/%zu packets recovered around one lost frame\n", hem ? "HEM   " : "normal", got.size() / 188, ts.size() / 188);
}

static std::vector<uint8_t> section(std::vector<uint8_t> body) { // body starts at table_id, CRC appended
    uint32_t crc = mpegCrc32(body.data(), (int)body.size());
    body.push_back(crc >> 24); body.push_back(crc >> 16); body.push_back(crc >> 8); body.push_back(crc);
    return body;
}
static void feedSection(TsDemux& d, int pid, const std::vector<uint8_t>& sec, int& cc) {
    uint8_t p[188]; memset(p, 0xFF, 188);
    p[0] = 0x47; p[1] = 0x40 | (pid >> 8); p[2] = pid & 0xFF; p[3] = 0x10 | (cc++ & 15); p[4] = 0;
    memcpy(p + 5, sec.data(), sec.size());
    d.feed(p);
}

int main() {
    std::mt19937 rng(4);
    roundTrip(true, rng);
    roundTrip(false, rng);
    // PSI/SI
    TsDemux d;
    int cc = 0;
    // PAT: program 100 -> PMT 0x100
    std::vector<uint8_t> pat = {0x00, 0xB0, 13, 0x00, 0x07, 0xC1, 0, 0, 0x00, 100, 0xE1, 0x00};
    feedSection(d, 0, section(pat), cc);
    // PMT: PCR 0x101, H.264 on 0x101, AAC on 0x102 with language
    std::vector<uint8_t> pmt = {0x02, 0xB0, 0, 0x00, 100, 0xC1, 0, 0, 0xE1, 0x01, 0xF0, 0x00,
                                0x1B, 0xE1, 0x01, 0xF0, 0x00,
                                0x0F, 0xE1, 0x02, 0xF0, 6, 0x0A, 4, 'e', 'n', 'g', 0};
    pmt[2] = (uint8_t)(pmt.size() - 3 + 4);
    feedSection(d, 0x100, section(pmt), cc);
    // SDT: service 100 "News One" by "Provider"
    std::vector<uint8_t> sdt = {0x42, 0xF0, 0, 0x00, 0x07, 0xC1, 0, 0, 0x23, 0x10, 0xFF,
                                0x00, 100, 0xFC, 0x80, 0, // service entry (running, free)
                                0x48, 0, 0x01, 8, 'P', 'r', 'o', 'v', 'i', 'd', 'e', 'r', 8, 'N', 'e', 'w', 's', ' ', 'O', 'n', 'e'};
    sdt[17] = (uint8_t)(sdt.size() - 18); // descriptor length field of the entry (bytes after it)
    sdt[15] = 0x80 | 0; sdt[16] = (uint8_t)(sdt.size() - 17);
    sdt[2] = (uint8_t)(sdt.size() - 3 + 4);
    // descriptor 0x48 length
    sdt[17 - 1 + 1] = 0; // placeholder overwritten below
    // rebuild the entry cleanly
    std::vector<uint8_t> desc = {0x48, (uint8_t)(3 + 8 + 8), 0x01, 8, 'P', 'r', 'o', 'v', 'i', 'd', 'e', 'r', 8, 'N', 'e', 'w', 's', ' ', 'O', 'n', 'e'};
    std::vector<uint8_t> sdt2 = {0x42, 0xF0, 0, 0x00, 0x07, 0xC1, 0, 0, 0x23, 0x10, 0xFF, 0x00, 100, 0xFC, (uint8_t)(0x80 | (desc.size() >> 8)), (uint8_t)(desc.size() & 0xFF)};
    sdt2.insert(sdt2.end(), desc.begin(), desc.end());
    sdt2[2] = (uint8_t)(sdt2.size() - 3 + 4);
    feedSection(d, 0x11, section(sdt2), cc);
    TsSnapshot sn = d.snapshot();
    CHECK(sn.services.size() == 1, "service count %zu", sn.services.size());
    if (!sn.services.empty()) {
        auto& sv = sn.services[0];
        CHECK(sv.id == 100 && sv.name == "News One" && sv.provider == "Provider", "SDT parse: id %d name '%s' provider '%s'", sv.id, sv.name.c_str(), sv.provider.c_str());
        CHECK(sv.pmtPid == 0x100 && sv.pcrPid == 0x101 && sv.streams.size() == 2, "PMT parse: pmt %x pcr %x streams %zu", sv.pmtPid, sv.pcrPid, sv.streams.size());
        if (sv.streams.size() == 2) CHECK(sv.streams[0].codec == "H.264" && sv.streams[1].codec == "AAC" && sv.streams[1].lang == "eng", "stream parse: %s / %s %s", sv.streams[0].codec.c_str(), sv.streams[1].codec.c_str(), sv.streams[1].lang.c_str());
    }
    CHECK(sn.onid == 0x2310 && sn.tsid == 7, "ids onid %x tsid %x", sn.onid, sn.tsid);
    // text decoding
    const uint8_t arabic[] = {0x02, 0xE5, 0xD1, 0xCD, 0xC8, 0xC7}; // ISO-8859-6: "مرحبا"
    CHECK(dvbText(arabic, 6) == "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7", "ISO-8859-6 decode");
    printf("%s\n", fails ? "FAILED" : "all passed");
    // EIT: two events with times, short + extended descriptions and a genre
    {
        TsDemux e;
        int cc2 = 0;
        const int64_t t0 = 1790969400;                 // 2026-10-02 19:30:00 UTC
        auto bcd = [](int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); };
        auto ev = [&](int id, int64_t start, int durMin, const char* title, const char* text, const char* ext) {
            std::vector<uint8_t> v;
            const int mjd = (int)(start / 86400) + 40587;
            const int sod = (int)(start % 86400);
            v.push_back(id >> 8); v.push_back(id & 255);
            v.push_back(mjd >> 8); v.push_back(mjd & 255);
            v.push_back(bcd(sod / 3600)); v.push_back(bcd(sod / 60 % 60)); v.push_back(bcd(sod % 60));
            v.push_back(bcd(durMin / 60)); v.push_back(bcd(durMin % 60)); v.push_back(0);
            std::vector<uint8_t> d;
            d.push_back(0x4D); d.push_back((uint8_t)(5 + strlen(title) + strlen(text)));
            d.push_back('e'); d.push_back('n'); d.push_back('g');
            d.push_back((uint8_t)strlen(title)); d.insert(d.end(), title, title + strlen(title));
            d.push_back((uint8_t)strlen(text)); d.insert(d.end(), text, text + strlen(text));
            // extended event in two descriptors (numbers 0 and 1)
            const std::string x = ext;
            const std::string pieces[2] = {x.substr(0, x.size() / 2), x.substr(x.size() / 2)};
            for (int k = 0; k < 2; k++) {
                d.push_back(0x4E); d.push_back((uint8_t)(6 + pieces[k].size()));
                d.push_back((uint8_t)(k << 4 | 1)); d.push_back('e'); d.push_back('n'); d.push_back('g');
                d.push_back(0);                                     // length_of_items
                d.push_back((uint8_t)pieces[k].size()); d.insert(d.end(), pieces[k].begin(), pieces[k].end());
            }
            d.push_back(0x54); d.push_back(2); d.push_back(0x20); d.push_back(0x00); // news
            const int dl = (int)d.size() | (4 << 13);                // running_status 4
            v.push_back((uint8_t)(dl >> 8)); v.push_back((uint8_t)(dl & 255));
            v.insert(v.end(), d.begin(), d.end());
            return v;
        };
        std::vector<uint8_t> body = {0x4E, 0xF0, 0, 0x00, 100, 0xC1, 0, 1, 0x23, 0x10, 0x00, 0x01, 1, 0x4E};
        auto e1 = ev(0x1234, t0, 60, "News at Seven", "Headlines and weather", "A longer description of the evening news programme.");
        auto e2 = ev(0x1235, t0 + 3600, 90, "The Film", "Drama", "Second programme.");
        body.insert(body.end(), e1.begin(), e1.end());
        body.insert(body.end(), e2.begin(), e2.end());
        const int slen = (int)body.size() - 3 + 4;
        body[1] = (uint8_t)(0xF0 | (slen >> 8)); body[2] = (uint8_t)(slen & 255);
        {   // a section longer than one packet is split over several, the first carrying the pointer field
            const std::vector<uint8_t> sec = section(body);
            size_t pos = 0; bool first = true;
            while (pos < sec.size()) {
                uint8_t pk[188]; memset(pk, 0xFF, 188);
                pk[0] = 0x47; pk[1] = (first ? 0x40 : 0) | 0x00; pk[2] = 0x12; pk[3] = 0x10 | (cc2++ & 15);
                size_t off = 4;
                if (first) { pk[4] = 0; off = 5; }
                const size_t n = std::min(sec.size() - pos, 188 - off);
                memcpy(pk + off, sec.data() + pos, n);
                pos += n; first = false;
                e.feed(pk);
            }
        }
        auto g = e.epg();
        CHECK(g.count(100) && g[100].size() == 2, "EIT: expected 2 events for service 100");
        if (g.count(100) && g[100].size() == 2) {
            const EpgEvent& a = g[100][0];
            CHECK(a.title == "News at Seven" && a.text == "Headlines and weather", "EIT title/text '%s' / '%s'", a.title.c_str(), a.text.c_str());
            CHECK(a.start == t0 && a.duration == 3600 && a.end() == t0 + 3600, "EIT time %lld dur %d", (long long)a.start, a.duration);
            CHECK(a.extended == "A longer description of the evening news programme.", "EIT extended '%s'", a.extended.c_str());
            CHECK(a.genre == 2 && a.running == 4 && a.lang == "eng", "EIT genre %d running %d lang %s", a.genre, a.running, a.lang.c_str());
            const EpgEvent& b2 = g[100][1];
            CHECK(b2.title == "The Film" && b2.start == t0 + 3600 && b2.duration == 5400, "EIT second event");
        }
        printf("EPG parsing checked\n");
    }
    {   // ATSC PSIP: virtual channel table gives the names and numbers
        TsDemux a;
        int c2 = 0;
        std::vector<uint8_t> pat2 = {0x00, 0xB0, 13, 0x00, 0x07, 0xC1, 0, 0, 0x00, 3, 0xE1, 0x00};
        feedSection(a, 0, section(pat2), c2);
        std::vector<uint8_t> v = {0xC8, 0xF0, 0, 0x00, 0x07, 0xC1, 0, 0, 0x00, 1};
        const char* nm = "KDEC-DT";
        for (int k = 0; k < 7; k++) { v.push_back(0); v.push_back((uint8_t)nm[k]); }
        const int major = 5, minor = 1;
        v.push_back(0xF0 | (major >> 6)); v.push_back((uint8_t)(((major & 0x3F) << 2) | (minor >> 8))); v.push_back((uint8_t)(minor & 0xFF));
        v.push_back(0x04);                       // modulation: 8-VSB
        for (int k = 0; k < 4; k++) v.push_back(0);
        v.push_back(0x00); v.push_back(0x07);    // channel TSID
        v.push_back(0x00); v.push_back(0x03);    // program number 3
        v.push_back(0x00); v.push_back(0x02);    // service type 2
        v.push_back(0x00); v.push_back(0x01);    // source id
        v.push_back(0xFC); v.push_back(0x00);    // no descriptors
        v.push_back(0xFC); v.push_back(0x00);    // additional descriptors length 0
        v[2] = (uint8_t)(v.size() - 3 + 4);
        feedSection(a, 0x1FFB, section(v), c2);
        TsSnapshot sn = a.snapshot();
        bool found = false;
        for (auto& sv : sn.services) if (sv.id == 3) { found = true; CHECK(sv.name == "KDEC-DT" && sv.provider == "channel 5.1" && sv.lcn == 501, "TVCT: name '%s' provider '%s' lcn %d", sv.name.c_str(), sv.provider.c_str(), sv.lcn); }
        CHECK(found, "TVCT service missing");
        printf("ATSC PSIP (TVCT) checked\n");
    }
    {
        // a corrupt packet that error correction missed: section start with a pointer field past the end of the payload
        TsDemux b;
        for (int ptr : {183, 184, 200, 255}) {
            uint8_t p[188];
            memset(p, 0xFF, sizeof p);
            p[0] = 0x47; p[1] = 0x40; p[2] = 0x00; p[3] = 0x10;   // PAT PID, payload unit start, payload only
            p[4] = (uint8_t)ptr;
            b.feed(p);
            p[1] = 0x00; p[3] = 0x11;                              // and a continuation packet after it
            b.feed(p);
        }
        printf("bad pointer fields survived\n");
    }
    {
        // a damaged PAT whose section_syntax_indicator reads 0: its CRC must still be checked, or it adds a phantom program
        TsDemux b;
        int c3 = 0;
        std::vector<uint8_t> bad = {0x00, 0x30, 13, 0x00, 0x07, 0xC1, 0, 0, 0xC8, 0x25, 0xE3, 0x72};   // program 51237 -> PMT 0x372
        std::vector<uint8_t> sec = section(bad);
        sec.back() ^= 0x5A;                                                                         // and a CRC that does not match
        feedSection(b, 0, sec, c3);
        feedSection(b, 0, sec, c3);
        bool phantom = false;
        for (auto& sv : b.snapshot().services) if (sv.id == 51237) phantom = true;
        CHECK(!phantom, "a PAT with a bad CRC and syntax bit 0 created program 51237");
        printf("damaged PAT without syntax bit rejected\n");
    }
    return fails ? 1 : 0;
}
