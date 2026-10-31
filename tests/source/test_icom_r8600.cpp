// Icom IC-R8600 I/Q driver, the parts that need no radio: the marker decoder fed with synthetic USB chunks, CI-V frames and setting
// payloads, the .spt (CSPT) to RAM-write conversion with a SYNTHETIC file (the real Icom firmware is never fetched or shipped), the
// firmware import and its checks, and the detection with no hardware. NOT covered: a real IC-R8600 (needs a user test).
#include "dect2/engine/updater.h"
#include "dect2/source/icom_r8600.h"
#include "dect2/source/source.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <unistd.h>
#ifdef _WIN32
static int setenv(const char* n, const char* v, int) { return _putenv_s(n, v); }
static int unsetenv(const char* n) { return _putenv_s(n, ""); }
#endif

namespace dect2 { namespace native { void listIcom(std::vector<DeviceInfo>&); void icomDevicesFrom(const std::vector<icom::UsbSeen>&, bool, std::vector<DeviceInfo>&); } }
using namespace dect2;
using namespace dect2::icom;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static std::vector<unsigned char> pack(const std::vector<int16_t>& v) {
    std::vector<unsigned char> b;
    for (auto x : v) { b.push_back((unsigned char)((uint16_t)x & 255)); b.push_back((unsigned char)((uint16_t)x >> 8)); }
    return b;
}
using I16 = std::vector<int16_t>;

// one CSPT record: 32-byte header (magic, bmRequestType in the top byte of the dword at 12, bRequest at 16, wValue at 18, length at 28) + payload
static void addRecord(std::vector<uint8_t>& f, uint16_t wValue, const std::vector<uint8_t>& data, uint8_t bmrt = 0x40, uint8_t breq = 0xA0) {
    uint8_t h[32] = {0};
    memcpy(h, "CSPT", 4);
    h[15] = bmrt;
    h[16] = breq;
    h[18] = (uint8_t)(wValue & 255);
    h[19] = (uint8_t)(wValue >> 8);
    const uint32_t n = (uint32_t)data.size();
    h[28] = (uint8_t)n; h[29] = (uint8_t)(n >> 8); h[30] = (uint8_t)(n >> 16); h[31] = (uint8_t)(n >> 24);
    f.insert(f.end(), h, h + 32);
    f.insert(f.end(), data.begin(), data.end());
}
static std::vector<uint8_t> synthSpt() {
    std::vector<uint8_t> f;
    addRecord(f, 0xE600, {1});
    addRecord(f, 0x0000, {0x02, 0x07, 0xf3, 0xff, 0x00});
    addRecord(f, 0x07f3, std::vector<uint8_t>(300, 0x5a));
    addRecord(f, 0x3f00, {1, 2, 3, 4});
    addRecord(f, 0xE600, {0});
    return f;
}

static void decoder() {
    // two markers, I/Q pairs, two markers, a pair, as the radio sends them
    const auto b = pack({77, 88, INT16_MIN, INT16_MIN, 1, -2, 3, -4, INT16_MIN, INT16_MIN, 5, -6});
    for (size_t split = 1; split <= b.size(); split++) {   // every way of cutting the chunks, including odd byte splits
        IqDecoder d;
        I16 out;
        for (size_t p = 0; p < b.size(); p += split) d.feed(b.data() + p, std::min(split, b.size() - p), out);
        CHECK(out == I16({1, -2, 3, -4, 5, -6}));
        CHECK(d.markers == 2);
    }
    {   // a lone I before a marker is dropped and the phase corrected
        IqDecoder d;
        I16 o;
        const auto p = pack({INT16_MIN, INT16_MIN, 1, 2, 99, INT16_MIN, INT16_MIN, 3, 4});
        d.feed(p.data(), p.size(), o);
        CHECK(o == I16({1, 2, 3, 4}));
        CHECK(d.realignments == 1);
    }
    {   // nothing before the first marker; a stream that never has one gives nothing
        IqDecoder d;
        I16 o;
        const auto p = pack({5, 6, 7, 8});
        d.feed(p.data(), p.size(), o);
        CHECK(o.empty());
    }
    {   // reset forgets a half word
        IqDecoder d;
        I16 o;
        const unsigned char one[1] = {0x12};
        d.feed(one, 1, o);
        d.reset();
        const auto p = pack({INT16_MIN, 9, 10});
        d.feed(p.data(), p.size(), o);
        CHECK(o == I16({9, 10}));
    }
    {   // a long run in odd chunks equals the run in one piece
        I16 words = {INT16_MIN, INT16_MIN};
        for (int i = 0; i < 4000; i++) words.push_back((int16_t)(i * 7 - 9000));
        const auto b2 = pack(words);
        IqDecoder a, c;
        I16 oa, oc;
        a.feed(b2.data(), b2.size(), oa);
        for (size_t p = 0; p < b2.size();) { const size_t n = std::min<size_t>(1 + (p * 31) % 97, b2.size() - p); c.feed(b2.data() + p, n, oc); p += n; }
        CHECK(oa == oc && oa.size() == 4000);
    }
}

static void civ() {
    CHECK(bcdFrequency(99900000) == std::vector<unsigned char>({0, 0, 0x90, 0x99, 0}));
    CHECK(civTune(99900000) == std::vector<unsigned char>({5, 0, 0, 0x90, 0x99, 0}));
    CHECK(civTune(3e9) == std::vector<unsigned char>({5, 0, 0, 0, 0, 0x30}));
    const auto f = civFrame({0x1a, 0x13, 1, 0});
    CHECK(f == std::vector<unsigned char>({0xfe, 0xfe, 0x96, 0xe0, 0x1a, 0x13, 1, 0, 0xfd, 0xff}));   // 9 bytes padded to an even 10
    CHECK(civFrame({5}).size() % 2 == 0);
    CHECK(civParseReply({0xfe, 0xfe, 0xe0, 0x96, 0xfb, 0xfd}) == CivReply::Ack);
    CHECK(civParseReply({0x00, 0xfe, 0xfe, 0xe0, 0x96, 0xfa, 0xfd}) == CivReply::Nak);
    CHECK(civParseReply({0xfe, 0xfe, 0xe0, 0x96, 0xfb}) == CivReply::None);
    CHECK(civParseReply({}) == CivReply::None);

    std::vector<unsigned char> p;
    std::string e;
    for (int g = 0; g <= 255; g++) {
        CHECK(settingPayload("rfgain", std::to_string(g), 99.9e6, p, e) && p.size() == 4 && p[0] == 0x14 && p[1] == 2 && p[2] * 100 + (p[3] >> 4) * 10 + (p[3] & 15) == g);
    }
    for (int a : {0, 10, 20, 30}) { CHECK(settingPayload("att", std::to_string(a), 1e8, p, e) && p == std::vector<unsigned char>({0x11, (unsigned char)((a / 10) << 4)})); }
    CHECK(settingPayload("preamp", "1", 1e8, p, e) && p == std::vector<unsigned char>({0x16, 2, 1}));
    CHECK(settingPayload("ipplus", "0", 1e8, p, e) && p == std::vector<unsigned char>({0x16, 0x65, 0}));
    CHECK(settingPayload("hfbpf", "1", 7.1e6, p, e) && p == std::vector<unsigned char>({0x1a, 0x13, 2, 1}));
    for (int ant = 1; ant <= 3; ant++) {
        CHECK(settingPayload("hfant", std::to_string(ant), 29999999, p, e) && p == std::vector<unsigned char>({0x12, (unsigned char)(ant - 1)}));
        CHECK(settingPayload("hfant", std::to_string(ant), 30e6, p, e) && p.empty());   // above HF only ANT1
    }
    for (const char* k : {"att", "preamp", "ipplus", "hfant", "hfbpf"}) CHECK(settingPayload(k, "keep", 1e8, p, e) && p.empty());
    const std::pair<const char*, const char*> bad[] = {{"rfgain", "256"}, {"rfgain", "-1"}, {"rfgain", "nan"}, {"rfgain", "1.5"}, {"rfgain", "2junk"}, {"rfgain", "keep"},
                                                       {"att", "15"}, {"hfant", "4"}, {"hfant", "0"}, {"preamp", "2"}, {"ipplus", "-1"}, {"nokey", "1"}};
    for (auto& kv : bad) CHECK(!settingPayload(kv.first, kv.second, 1e8, p, e) && !e.empty());
    for (const char* k : {"rfgain", "att", "preamp", "ipplus", "hfant", "hfbpf"}) CHECK(validSettingKey(k));
    CHECK(!validSettingKey("ppm"));
    CHECK(defaultSetting("rfgain") == "255" && defaultSetting("att") == "keep" && defaultSetting("hfbpf") == "1");

    CHECK(civEnterStream().size() == 2 && civLeaveStream().size() == 2);
    CHECK(civSetRate(960000) == std::vector<unsigned char>({0x1a, 0x13, 1, 1, 0, 4}));   // code = 6 - index
    CHECK(civSetRate(240000).back() == 6 && civSetRate(5120000).back() == 1);

    CHECK(chooseRate(0) == 960000);
    CHECK(chooseRate(100000) == 240000);
    CHECK(chooseRate(240000) == 240000);
    CHECK(chooseRate(240500) == 480000 && chooseRate(240001) == 240000);
    CHECK(chooseRate(2e6) == 3840000);
    CHECK(chooseRate(9e6) == 5120000);
    CHECK(rateIndex(1920000) == 3 && rateIndex(1234) == -1);
}

static void firmware() {
    const auto spt = synthSpt();
    std::vector<FwRecord> r;
    std::string e;
    CHECK(sptToRecords(spt, r, e) && r.size() == 5);
    CHECK(r[0].wValue == 0xE600 && r[0].data == std::vector<uint8_t>({1}));
    CHECK(r[2].wValue == 0x07f3 && r[2].data.size() == 300);
    CHECK(r[4].wValue == 0xE600 && r[4].data == std::vector<uint8_t>({0}));
    CHECK(recordsProblem(r).empty());
    const std::vector<FwRecord> good = r;   // (the failing parses below clear r)

    const std::string j = recordsToJson(good);
    CHECK(j.rfind("[{\"wValue\": 58880, \"data\": \"01\"}, {\"wValue\": 0, \"data\": \"0207f3ff00\"}, ", 0) == 0);
    CHECK(j.back() == '\n' && j[j.size() - 2] == ']');
    std::vector<FwRecord> back;
    CHECK(recordsFromJson(j, back, e) && back.size() == good.size());
    for (size_t i = 0; i < good.size() && i < back.size(); i++) CHECK(back[i].wValue == good[i].wValue && back[i].data == good[i].data);
    CHECK(recordsToJson(back) == j);
    // the format other tools write: any whitespace
    CHECK(recordsFromJson("[ {\"wValue\":58880,\"data\":\"01\"},\n {\"wValue\":0,\"data\":\"00ff\"} , {\"wValue\":58880,\"data\":\"00\"} ]", back, e) && back.size() == 3 && back[1].data[1] == 0xff);

    // damaged and hostile input
    CHECK(!sptToRecords({}, r, e));
    CHECK(!sptToRecords(std::vector<uint8_t>(40, 'x'), r, e) && r.empty());
    {   auto t = spt; t.resize(t.size() - 3); CHECK(!sptToRecords(t, r, e)); }                 // cut inside the last payload
    {   auto t = spt; t[28] = 0xff; t[29] = 0xff; t[30] = 0xff; t[31] = 0x7f; CHECK(!sptToRecords(t, r, e)); }   // length past the end
    {   auto t = spt; t.push_back('Z'); CHECK(!sptToRecords(t, r, e)); }                       // trailing junk
    {   std::vector<uint8_t> t; addRecord(t, 0xE600, {1}); addRecord(t, 0, {1, 2}, 0xC0, 0xA0); CHECK(!sptToRecords(t, r, e)); }   // an IN transfer
    {   std::vector<uint8_t> t; addRecord(t, 0xE600, {1}); addRecord(t, 0, {1, 2}, 0x40, 0xA3); CHECK(!sptToRecords(t, r, e)); }   // another request
    CHECK(!recordsFromJson("", back, e));
    CHECK(!recordsFromJson("[]", back, e));
    CHECK(!recordsFromJson("[{\"wValue\": 70000, \"data\": \"01\"}]", back, e));
    CHECK(!recordsFromJson("[{\"wValue\": 1, \"data\": \"0g\"}]", back, e));
    CHECK(!recordsFromJson("[{\"wValue\": 1, \"data\": \"012\"}]", back, e));
    CHECK(!recordsFromJson("[{\"wValue\": 1, \"data\": \"01\", \"x\": 1}]", back, e));
    CHECK(!recordsFromJson("[{\"wValue\": 1, \"data\": \"01\"}] extra", back, e));
    CHECK(!recordsFromJson("[{\"wValue\": 1, \"data\": \"01\"}", back, e));
    // sanity of the RAM writes
    {   auto t = good; t.front().data = {0}; CHECK(!recordsProblem(t).empty()); }                          // does not hold the CPU first
    {   auto t = good; t.back().data = {1}; CHECK(!recordsProblem(t).empty()); }                           // does not release it last
    {   auto t = good; t[1].wValue = 0x3fff; CHECK(!recordsProblem(t).empty()); }                          // runs past the code RAM
    {   auto t = good; t[1].wValue = 0x8000; CHECK(!recordsProblem(t).empty()); }                          // outside the RAM
    {   auto t = good; t[1].wValue = 0xE000; t[1].data.assign(16, 0x55); CHECK(recordsProblem(t).empty()); }   // the 512 bytes of scratch RAM at 0xE000 (#357)
    {   auto t = good; t[1].wValue = 0xE1F0; t[1].data.assign(32, 0x55); CHECK(!recordsProblem(t).empty()); }  // past its end
    {   auto t = good; t[1].data.clear(); CHECK(!recordsProblem(t).empty()); }
    CHECK(!recordsProblem({}).empty());
    CHECK(recordsProblem(good).empty());

    // the real Icom hash is not that of a synthetic file: refused, and the message gives the hash
    CHECK(!recordsVerified(good));
    std::string json;
    std::vector<FwRecord> got;
    CHECK(!importFirmwareBytes(spt, got, json, e) && e.find("SHA-256") != std::string::npos && got.empty());
    CHECK(!importFirmwareBytes(std::vector<uint8_t>{'h', 'e', 'l', 'l', 'o'}, got, json, e));
    CHECK(!importFirmwareBytes(std::vector<uint8_t>(), got, json, e));
    // the hash is that of the canonical file (checked against sha256 of the text with and without its final newline)
    const std::string h1 = sha256Bytes(j.data(), j.size()), h2 = sha256Bytes(j.data(), j.size() - 1);
    setVerifiedSequenceHashForTest(h1);
    CHECK(recordsVerified(good));
    CHECK(importFirmwareBytes(spt, got, json, e) && json == j && got.size() == 5);
    std::vector<uint8_t> jb(j.begin(), j.end());
    CHECK(importFirmwareBytes(jb, got, json, e) && json == j);                      // spt_seq.json in
    setVerifiedSequenceHashForTest(h2);
    CHECK(importFirmwareBytes(jb, got, json, e));                                   // a hash made without the newline
    {   auto t = spt; t[66] ^= 0x10; CHECK(!importFirmwareBytes(t, got, json, e)); }   // one changed byte: different firmware
    setVerifiedSequenceHashForTest(h1);

    // store and find: through a data folder of the test
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("onair_icom_test_" + std::to_string((long)getpid()));
    fs::create_directories(dir);
    setenv("DECT2_DATA_DIR", dir.string().c_str(), 1);
    unsetenv("ONAIR_ICOM_FIRMWARE");
    const fs::path sptFile = dir / "IC-R8600_usb_iq.spt";
    { std::ofstream o(sptFile, std::ios::binary); o.write((const char*)spt.data(), (std::streamsize)spt.size()); }
    std::string where;
    CHECK(importFirmwareFile(sptFile.string(), where, e));
    CHECK(!where.empty() && fs::exists(where));
    CHECK(where == firmwareStorePath());
    { std::ifstream in(where, std::ios::binary); std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); CHECK(s == j); }
    CHECK(!fs::exists(where + ".tmp"));
    std::vector<FwRecord> loaded;
    std::string path;
    CHECK(loadFirmware(loaded, path, e) && loaded.size() == 5 && path == where);
    CHECK(firmwareInstalled());
    CHECK(!importFirmwareFile((dir / "missing.spt").string(), where, e));
    {   // a text file is not the firmware, and the stored one is left alone
        const fs::path txt = dir / "notes.txt";
        { std::ofstream o(txt); o << "hello"; }
        CHECK(!importFirmwareFile(txt.string(), where, e));
        CHECK(firmwareInstalled());
    }
    // no firmware anywhere: the plain message
    fs::remove(where);
    setenv("ONAIR_ICOM_NO_CODEC_SEARCH", "1", 1);
    CHECK(!firmwareInstalled());
    CHECK(!loadFirmware(loaded, path, e) && e.find("import") != std::string::npos);
    setenv("ONAIR_ICOM_FIRMWARE", sptFile.string().c_str(), 1);   // the environment file is found first
    CHECK(firmwareInstalled());
    unsetenv("ONAIR_ICOM_FIRMWARE");
    unsetenv("ONAIR_ICOM_NO_CODEC_SEARCH");
    unsetenv("DECT2_DATA_DIR");
    setVerifiedSequenceHashForTest("");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

static void detection() {
    CHECK(isIcomIq(0x0c26, 0x22) && isIcomIq(0x0c26, 0x23));
    CHECK(!isIcomIq(0x0c26, 0x24) && !isIcomIq(0x04b4, 0x00f1) && !isIcomIq(0x1d50, 0x6089));
    UsbSeen a;
    a.vid = 0x0c26; a.pid = 0x22; a.bus = 3; a.address = 9; a.ports = "1-4";
    UsbSeen b = a;
    b.pid = 0x23; b.address = 11;   // after the firmware: same route, new address
    CHECK(usbIdentity(a) == "usb-3-1-4" && usbIdentity(a) == usbIdentity(b));
    CHECK(isLoader(a) && !isLoader(b));

    std::vector<DeviceInfo> out;
    native::icomDevicesFrom({}, false, out);   // no hardware: nothing is listed
    CHECK(out.empty());
    UsbSeen other;
    other.vid = 0x04b4; other.pid = 0x00f1;
    native::icomDevicesFrom({other}, true, out);
    CHECK(out.empty());

    native::icomDevicesFrom({a}, false, out);   // in the loader, without the firmware
    CHECK(out.size() == 1);
    if (out.size() == 1) {
        const DeviceInfo& d = out[0];
        CHECK(d.kind == DeviceInfo::Native && d.board == "icr8600" && d.serial == "usb-3-1-4" && d.nativeArgs == d.serial);
        CHECK(d.name.find("needs Icom firmware, see ?") != std::string::npos);
        CHECK(!d.canTransmit && !d.txFullDuplex);
        CHECK(d.rateRanges.size() == 6 && d.minRateHz == 240000 && d.maxRateHz == 5120000);
        CHECK(d.minFreqHz == 10e3 && d.maxFreqHz == 3e9);
        CHECK(d.settings.size() == 6);
        for (const auto& s : d.settings) CHECK(validSettingKey(s.key));
    }
    out.clear();
    native::icomDevicesFrom({a}, true, out);   // with it
    CHECK(out.size() == 1 && out[0].name.find("needs Icom firmware") == std::string::npos && out[0].name.find("needs a user test") != std::string::npos);
    out.clear();
    native::icomDevicesFrom({b}, false, out);   // already streaming (loaded by another program): no firmware needed
    CHECK(out.size() == 1 && out[0].name.find("needs Icom firmware") == std::string::npos);

    // the real USB scan on this machine (only with ONAIR_TEST_REAL_USB=1: CI runners crashed in the USB library, the injected lists above cover the logic)
    if (!getenv("ONAIR_TEST_REAL_USB")) return;
    const auto seen = findUsb();
    for (const auto& u : seen) CHECK(isIcomIq(u.vid, u.pid));
    std::vector<DeviceInfo> radios;
    native::listIcom(radios);   // (the real list: nothing, or only receive-only Icom entries)
    for (const auto& d : radios) CHECK(d.board == "icr8600" && !d.canTransmit);
    if (seen.empty()) CHECK(radios.empty());
}

int main() {
    decoder();
    civ();
    firmware();
    detection();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    puts("PASS: marker decoder (every split), CI-V frames and all setting payloads, CSPT conversion, firmware import checks, detection with no radio");
    return 0;
}
