// Icom IC-R8600 I/Q: the plain logic (icom_r8600.h). No libusb here.
// Protocol: ibelinp/ic-r8600-usb-iq (MIT, Pieter Ibelings 2026) and the SoapySDR module of Pawel "Topol" Topolski (MIT, "Topol in collaboration
// with AI"); see THIRD_PARTY.md.
#include "dect2/source/icom_r8600.h"
#include "dect2/engine/updater.h"
#include "native_common.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace dect2 {
namespace icom {

const double kRates[kRateCount] = {240000, 480000, 960000, 1920000, 3840000, 5120000};
const char* const kVerifiedSequenceSha256 = "148cf7641da39bba05c68e1a40b991fb338801d7c580228cb64c2817d9931b16";

static std::string gTestHash;
void setVerifiedSequenceHashForTest(const std::string& h) { gTestHash = h; }

double chooseRate(double wanted) {
    if (!(wanted > 0)) return 960000;
    for (double r : kRates) if (r >= wanted * 0.999) return r;
    return kRates[kRateCount - 1];
}
int rateIndex(double rate) {
    for (int i = 0; i < kRateCount; i++) if (std::fabs(kRates[i] - rate) < 1.0) return i;
    return -1;
}
unsigned char rateCode(double rate) {
    const int i = rateIndex(rate);
    return (unsigned char)(6 - (i < 0 ? 2 : i));
}

// ---------------------------------------------------------------------------------------------------------------- I/Q stream

void IqDecoder::reset() {
    byteCarry_ = locked_ = marker_ = haveI_ = false;
    carry_ = 0;
    pendingI_ = 0;
    markers = realignments = 0;
}
void IqDecoder::word(int16_t v, std::vector<int16_t>& o) {
    if (v == INT16_MIN) {   // Icom's marker: drop it, the next word starts a new I/Q pair
        if (!marker_) markers++;
        marker_ = true;
        return;
    }
    if (marker_) {
        marker_ = false;
        locked_ = true;
        if (haveI_) { haveI_ = false; realignments++; }   // an I without its Q before a marker: the pair was cut, drop it
    }
    if (!locked_) return;   // nothing is taken before the first marker
    if (!haveI_) { pendingI_ = v; haveI_ = true; }
    else { o.push_back(pendingI_); o.push_back(v); haveI_ = false; }
}
void IqDecoder::feed(const unsigned char* b, size_t n, std::vector<int16_t>& o) {
    size_t k = 0;
    if (byteCarry_ && n) {
        word((int16_t)(uint16_t)((uint16_t)carry_ | ((uint16_t)b[0] << 8)), o);
        k = 1;
        byteCarry_ = false;
    }
    for (; k + 1 < n; k += 2) word((int16_t)(uint16_t)((uint16_t)b[k] | ((uint16_t)b[k + 1] << 8)), o);
    if (k < n) { carry_ = b[k]; byteCarry_ = true; }
}

// ---------------------------------------------------------------------------------------------------------------- CI-V

std::vector<unsigned char> bcdFrequency(uint64_t f) {
    std::vector<unsigned char> o(5);
    for (auto& b : o) {
        const unsigned a = (unsigned)(f % 10);
        f /= 10;
        b = (unsigned char)(a | ((f % 10) << 4));
        f /= 10;
    }
    return o;
}
std::vector<unsigned char> civTune(double hz) {
    std::vector<unsigned char> p = bcdFrequency((uint64_t)std::llround(hz));
    p.insert(p.begin(), 5);
    return p;
}
std::vector<unsigned char> civFrame(const std::vector<unsigned char>& payload) {
    std::vector<unsigned char> f = {0xfe, 0xfe, 0x96, 0xe0};
    f.insert(f.end(), payload.begin(), payload.end());
    f.push_back(0xfd);
    if (f.size() % 2) f.push_back(0xff);
    return f;
}
CivReply civParseReply(const std::vector<unsigned char>& all) {
    for (size_t k = 0; k + 5 < all.size(); k++) {
        if (all[k] == 0xfe && all[k + 1] == 0xfe && all[k + 2] == 0xe0 && all[k + 3] == 0x96) {
            if (all[k + 4] == 0xfb && all[k + 5] == 0xfd) return CivReply::Ack;
            if (all[k + 4] == 0xfa && all[k + 5] == 0xfd) return CivReply::Nak;
        }
    }
    return CivReply::None;
}

bool validSettingKey(const std::string& k) {
    return k == "rfgain" || k == "att" || k == "preamp" || k == "ipplus" || k == "hfant" || k == "hfbpf";
}
std::string defaultSetting(const std::string& k) {
    if (k == "rfgain") return "255";
    if (k == "hfbpf") return "1";   // as the first working module initialises it
    return "keep";
}
bool settingPayload(const std::string& key, const std::string& value, double frequencyHz, std::vector<unsigned char>& payload, std::string& err) {
    payload.clear();
    if (!validSettingKey(key)) { err = "unknown Icom setting: " + key; return false; }
    if (value == "keep" && key != "rfgain") return true;
    size_t end = 0;
    double d = 0;
    try { d = std::stod(value, &end); } catch (...) { err = "Icom setting " + key + " needs a number"; return false; }
    if (end != value.size() || !std::isfinite(d) || d != std::floor(d)) { err = "Icom setting " + key + " needs a whole number"; return false; }
    const int n = (int)d;
    const bool bit = d == 0 || d == 1;
    if (key == "rfgain") {
        if (d < 0 || d > 255) { err = "Icom RF gain is 0 to 255"; return false; }
        payload = {0x14, 2, (unsigned char)(n / 100), (unsigned char)((((n % 100) / 10) << 4) | (n % 10))};
    } else if (key == "att") {
        if (!(d == 0 || d == 10 || d == 20 || d == 30)) { err = "Icom attenuator is 0, 10, 20 or 30 dB"; return false; }
        payload = {0x11, (unsigned char)((n / 10) << 4)};
    } else if (key == "preamp") {
        if (!bit) { err = "Icom preamplifier is 0 or 1"; return false; }
        payload = {0x16, 2, (unsigned char)n};
    } else if (key == "ipplus") {
        if (!bit) { err = "Icom IP+ is 0 or 1"; return false; }
        payload = {0x16, 0x65, (unsigned char)n};
    } else if (key == "hfbpf") {
        if (!bit) { err = "Icom HF band-pass filter is 0 or 1"; return false; }
        payload = {0x1a, 0x13, 2, (unsigned char)n};
    } else {   // hfant
        if (d < 1 || d > 3) { err = "Icom HF antenna is 1, 2 or 3"; return false; }
        if (frequencyHz >= 30e6) return true;   // above HF only ANT1 is there
        payload = {0x12, (unsigned char)(n - 1)};
    }
    return true;
}
std::vector<std::vector<unsigned char>> civEnterStream() { return {{0x1a, 0x13, 1, 0}, {0x1a, 0x13, 0, 1}}; }
std::vector<unsigned char> civSetRate(double rate) { return {0x1a, 0x13, 1, 1, 0, rateCode(rate)}; }
std::vector<std::vector<unsigned char>> civLeaveStream() { return {{0x1a, 0x13, 1, 0}, {0x1a, 0x13, 0, 0}}; }

// ---------------------------------------------------------------------------------------------------------------- firmware

namespace {
constexpr size_t kMaxRecords = 64, kMaxBytes = 64 * 1024;
constexpr uint16_t kCpucs = 0xE600;

uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

std::string hexOf(const std::vector<uint8_t>& d) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) { s += h[b >> 4]; s += h[b & 15]; }
    return s;
}
int hexVal(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
}   // namespace

bool sptToRecords(const std::vector<uint8_t>& d, std::vector<FwRecord>& out, std::string& err) {
    out.clear();
    if (d.size() < 32 || memcmp(d.data(), "CSPT", 4) != 0) {
        err = "this is not Icom's IC-R8600_usb_iq.spt (a binary file that starts with CSPT)";
        return false;
    }
    size_t off = 0;
    while (off < d.size()) {
        if (d.size() - off < 32 || memcmp(d.data() + off, "CSPT", 4) != 0) { err = "the .spt file is damaged (no CSPT record at byte " + std::to_string(off) + ")"; out.clear(); return false; }
        const uint8_t* h = d.data() + off;
        const uint8_t bmRequestType = (uint8_t)(le32(h + 12) >> 24), bRequest = h[16];
        const uint32_t plen = le32(h + 28);
        if (plen > d.size() - off - 32) { err = "the .spt file is damaged (a record runs past the end)"; out.clear(); return false; }
        if (bmRequestType != 0x40 || bRequest != 0xA0) {
            err = "the .spt file has a record that is not a firmware RAM write (type " + std::to_string(bmRequestType) + ", request " + std::to_string(bRequest) + "); not loading it";
            out.clear();
            return false;
        }
        if (out.size() >= kMaxRecords) { err = "the .spt file has too many records"; out.clear(); return false; }
        FwRecord r;
        r.wValue = le16(h + 18);
        r.data.assign(d.begin() + (long)(off + 32), d.begin() + (long)(off + 32 + plen));
        out.push_back(std::move(r));
        off += 32 + plen;
    }
    if (out.empty()) { err = "no records in the .spt file"; return false; }
    return true;
}

std::string recordsToJson(const std::vector<FwRecord>& recs) {
    std::string s = "[";
    for (size_t i = 0; i < recs.size(); i++) {
        if (i) s += ", ";
        s += "{\"wValue\": " + std::to_string(recs[i].wValue) + ", \"data\": \"" + hexOf(recs[i].data) + "\"}";
    }
    s += "]\n";
    return s;
}

bool recordsFromJson(const std::string& j, std::vector<FwRecord>& out, std::string& err) {
    out.clear();
    size_t i = 0;
    auto ws = [&] { while (i < j.size() && (j[i] == ' ' || j[i] == '\n' || j[i] == '\r' || j[i] == '\t')) i++; };
    auto bad = [&](const char* m) { err = std::string("not a firmware sequence file: ") + m; out.clear(); return false; };
    ws();
    if (i >= j.size() || j[i] != '[') return bad("it does not start with [");
    i++;
    ws();
    if (i < j.size() && j[i] == ']') return bad("it has no records");
    for (;;) {
        ws();
        if (i >= j.size() || j[i] != '{') return bad("a record is missing");
        i++;
        bool haveW = false, haveD = false;
        FwRecord r;
        for (int field = 0; field < 2; field++) {
            ws();
            if (i >= j.size() || j[i] != '"') return bad("a field name is missing");
            const size_t e = j.find('"', i + 1);
            if (e == std::string::npos) return bad("a field name is not closed");
            const std::string name = j.substr(i + 1, e - i - 1);
            i = e + 1;
            ws();
            if (i >= j.size() || j[i] != ':') return bad("a colon is missing");
            i++;
            ws();
            if (name == "wValue" && !haveW) {
                size_t q = i;
                unsigned long v = 0;
                while (q < j.size() && j[q] >= '0' && j[q] <= '9' && q - i < 6) v = v * 10 + (unsigned long)(j[q++] - '0');
                if (q == i || v > 0xFFFF) return bad("wValue is not a 16-bit number");
                r.wValue = (uint16_t)v;
                i = q;
                haveW = true;
            } else if (name == "data" && !haveD) {
                if (i >= j.size() || j[i] != '"') return bad("data is not a string");
                const size_t e2 = j.find('"', i + 1);
                if (e2 == std::string::npos) return bad("data is not closed");
                const std::string hx = j.substr(i + 1, e2 - i - 1);
                if (hx.size() % 2 || hx.size() > 2 * 4096) return bad("data is not a hex string");
                for (size_t k = 0; k < hx.size(); k += 2) {
                    const int a = hexVal(hx[k]), b = hexVal(hx[k + 1]);
                    if (a < 0 || b < 0) return bad("data is not a hex string");
                    r.data.push_back((uint8_t)(a * 16 + b));
                }
                i = e2 + 1;
                haveD = true;
            } else {
                return bad("an unexpected field");
            }
            ws();
            if (field == 0) {
                if (i >= j.size() || j[i] != ',') return bad("a comma is missing");
                i++;
            }
        }
        if (!haveW || !haveD) return bad("a record lacks wValue or data");
        if (i >= j.size() || j[i] != '}') return bad("a record is not closed");
        i++;
        if (out.size() >= kMaxRecords) return bad("too many records");
        out.push_back(std::move(r));
        ws();
        if (i < j.size() && j[i] == ',') { i++; continue; }
        if (i < j.size() && j[i] == ']') { i++; break; }
        return bad("the list is not closed");
    }
    ws();
    if (i != j.size()) return bad("text after the list");
    return true;
}

std::string recordsProblem(const std::vector<FwRecord>& r) {
    if (r.size() < 3) return "too few records";
    if (r.size() > kMaxRecords) return "too many records";
    size_t total = 0;
    for (const auto& x : r) total += x.data.size();
    if (total > kMaxBytes) return "too much data";
    if (!(r.front().wValue == kCpucs && r.front().data == std::vector<uint8_t>{1})) return "the first write does not hold the processor (0xE600 = 1)";
    if (!(r.back().wValue == kCpucs && r.back().data == std::vector<uint8_t>{0})) return "the last write does not release the processor (0xE600 = 0)";
    for (size_t i = 1; i + 1 < r.size(); i++) {
        const auto& x = r[i];
        if (x.data.empty()) return "an empty write";
        // the FX2's 16 KiB of code RAM, and its 512 bytes of scratch RAM at 0xE000 that the vendor package also loads (#357). The exact-hash check that follows is what
        // tells the real firmware from any other file; this only throws out something that is not a firmware loader sequence at all.
        const size_t end = (size_t)x.wValue + x.data.size();
        const bool inCode = end <= 0x4000, inScratch = x.wValue >= 0xE000 && end <= 0xE200;
        if (!inCode && !inScratch) { char b[96]; snprintf(b, sizeof b, "a write outside the FX2 RAM (0x%04X, %zu bytes)", (unsigned)x.wValue, x.data.size()); return b; }
    }
    return "";
}

bool recordsVerified(const std::vector<FwRecord>& recs, std::string* sha) {
    const std::string j = recordsToJson(recs);
    const std::string h1 = sha256Bytes(j.data(), j.size()), h2 = sha256Bytes(j.data(), j.size() - 1);
    if (sha) *sha = h1;
    const std::string want = gTestHash.empty() ? std::string(kVerifiedSequenceSha256) : gTestHash;
    return h1 == want || h2 == want;
}

bool importFirmwareBytes(const std::vector<uint8_t>& file, std::vector<FwRecord>& recs, std::string& json, std::string& err) {
    recs.clear();
    json.clear();
    if (file.size() > 1024 * 1024) { err = "the file is too big to be the Icom firmware"; return false; }
    if (file.size() >= 4 && memcmp(file.data(), "CSPT", 4) == 0) {
        if (!sptToRecords(file, recs, err)) return false;
    } else {
        const std::string text(file.begin(), file.end());
        if (text.find_first_not_of(" \t\r\n") == std::string::npos || text[text.find_first_not_of(" \t\r\n")] != '[') {
            err = "this is neither IC-R8600_usb_iq.spt nor spt_seq.json";
            return false;
        }
        if (!recordsFromJson(text, recs, err)) return false;
    }
    const std::string bad = recordsProblem(recs);
    if (!bad.empty()) { err = "this does not look like the IC-R8600 firmware: " + bad; recs.clear(); return false; }
    std::string sha;
    if (!recordsVerified(recs, &sha)) {
        err = "this firmware differs from the one OnAir was written for (the IC-R8600 USB I/Q package of Icom, firmware 1.01). Its SHA-256 is " + sha +
              ". Install Icom's \"USB I/Q Package for HDSDR\" and import the file from it; if it is the package from Icom's site, tell us the hash above.";
        recs.clear();
        return false;
    }
    json = recordsToJson(recs);
    return true;
}

std::vector<std::string> firmwarePaths() {
    std::vector<std::string> v;
    if (const char* e = getenv("ONAIR_ICOM_FIRMWARE")) if (*e) v.push_back(e);
    if (const char* n = getenv("ONAIR_ICOM_NO_CODEC_SEARCH")) if (*n) return v;   // (tests)
    for (const auto& dir : native::codecFolders()) {
        v.push_back(dir + "icr8600_spt_seq.json");
        v.push_back(dir + "IC-R8600_usb_iq.spt");
    }
    return v;
}
std::string firmwareStorePath() {
    const std::string d = codec::userCodecDir();
    if (d.empty()) return {};
    return codec::joinPath(d, "icr8600_spt_seq.json");
}

static bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool loadFirmware(std::vector<FwRecord>& recs, std::string& path, std::string& err) {
    std::string firstErr;
    for (const auto& p : firmwarePaths()) {
        std::vector<uint8_t> bytes;
        if (!readFile(p, bytes)) continue;
        std::string json, e;
        if (importFirmwareBytes(bytes, recs, json, e)) { path = p; return true; }
        if (firstErr.empty()) firstErr = p + ": " + e;
    }
    err = firstErr.empty() ? std::string("the Icom firmware file was not found; import it in Radio settings (Import Icom firmware)") : firstErr;
    return false;
}
bool firmwareInstalled() {
    std::vector<FwRecord> r;
    std::string p, e;
    return loadFirmware(r, p, e);
}

bool importFirmwareFile(const std::string& path, std::string& where, std::string& err) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes)) { err = "cannot read " + path; return false; }
    std::vector<FwRecord> recs;
    std::string json;
    if (!importFirmwareBytes(bytes, recs, json, err)) return false;
    where = firmwareStorePath();
    if (where.empty()) { err = "OnAir has no folder for the firmware on this computer"; return false; }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(where.c_str())).parent_path(), ec);
    const std::string tmp = where + ".tmp";
    {
        std::ofstream o(std::filesystem::path(reinterpret_cast<const char8_t*>(tmp.c_str())), std::ios::binary | std::ios::trunc);
        o.write(json.data(), (std::streamsize)json.size());
        if (!o) { err = "cannot write " + tmp; return false; }
    }
    std::filesystem::rename(std::filesystem::path(reinterpret_cast<const char8_t*>(tmp.c_str())), std::filesystem::path(reinterpret_cast<const char8_t*>(where.c_str())), ec);
    if (ec) { err = "cannot store " + where + ": " + ec.message(); return false; }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------- finding the radio

bool isIcomIq(uint16_t vid, uint16_t pid) { return vid == kVid && (pid == kPidLoader || pid == kPidStream); }
bool isLoader(const UsbSeen& u) { return u.pid == kPidLoader; }
std::string usbIdentity(const UsbSeen& u) { return "usb-" + std::to_string(u.bus) + (u.ports.empty() ? std::string() : "-" + u.ports); }

} // namespace icom
} // namespace dect2
