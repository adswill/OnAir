#include "dect2/session/privacy.h"
#include "dect2/mesh/mesh_crypto.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dect2::sess {

namespace fs = std::filesystem;

namespace {

void randomBytes(uint8_t* p, size_t n) {
#ifndef _WIN32
    if (FILE* f = fopen("/dev/urandom", "rb")) {
        const size_t got = fread(p, 1, n, f);
        fclose(f);
        if (got == n) return;
    }
#endif
    std::random_device rd;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(rd() ^ (uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count() >> (i % 7)));
}

fs::path pathOf(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }

}  // namespace

// ------------------------------------------------------------------ privacy

Privacy::Privacy() = default;
Privacy::~Privacy() = default;

void Privacy::ensureSaltLocked() {
    if (haveSalt_) return;
    randomBytes(salt_.data(), salt_.size());
    haveSalt_ = true;
}

bool Privacy::loadSalt(const std::string& dir) {
    std::lock_guard<std::mutex> lk(mu_);
    saltPath_ = dir + "/session_salt.bin";
    {
        std::ifstream in(pathOf(saltPath_), std::ios::binary);
        std::array<uint8_t, 16> b{};
        if (in && in.read(reinterpret_cast<char*>(b.data()), 16) && in.gcount() == 16) {
            // a salt of only zeros is a broken file
            bool nz = false;
            for (uint8_t x : b) nz = nz || x;
            if (nz) { salt_ = b; haveSalt_ = true; return true; }
        }
    }
    randomBytes(salt_.data(), salt_.size());
    haveSalt_ = true;
    if (!makeOwnerOnlyDir(dir)) return false;
    return writeOwnerOnlyFile(saltPath_, std::string(reinterpret_cast<const char*>(salt_.data()), 16));
}

void Privacy::setSalt(const std::array<uint8_t, 16>& salt) {
    std::lock_guard<std::mutex> lk(mu_);
    salt_ = salt;
    haveSalt_ = true;
}

std::array<uint8_t, 16> Privacy::salt() const {
    std::lock_guard<std::mutex> lk(mu_);
    return salt_;
}

bool Privacy::regenerateSalt() {
    std::lock_guard<std::mutex> lk(mu_);
    randomBytes(salt_.data(), salt_.size());
    haveSalt_ = true;
    if (saltPath_.empty()) return true;
    return writeOwnerOnlyFile(saltPath_, std::string(reinterpret_cast<const char*>(salt_.data()), 16));
}

std::string Privacy::hash(uint16_t mode, const std::string& id) const {
    std::array<uint8_t, 16> salt;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const_cast<Privacy*>(this)->ensureSaltLocked();
        salt = salt_;
    }
    std::string msg;
    msg += (char)(mode >> 8);
    msg += (char)(mode & 0xFF);
    msg += id;
    uint8_t out[32];
    meshcrypto::hmacSha256(salt.data(), salt.size(), reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), out);
    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02x", out[i]);
    return std::string(hex, 16);
}

std::string Privacy::shortTag(uint16_t mode, const std::string& id, const char* label) const {
    std::string h = hash(mode, id).substr(0, 4);
    for (char& c : h) c = (char)toupper((unsigned char)c);
    return std::string(label && *label ? label : "id") + " " + h;
}

void Privacy::setClass(uint16_t mode, IdClass c) { std::lock_guard<std::mutex> lk(mu_); cls_[mode] = c; }
IdClass Privacy::classOf(uint16_t mode) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cls_.find(mode);
    return it == cls_.end() ? IdClass::Open : it->second;
}
void Privacy::setKeepRealIds(uint16_t mode, bool keep) { std::lock_guard<std::mutex> lk(mu_); keep_[mode] = keep; }
bool Privacy::keepRealIds(uint16_t mode) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = keep_.find(mode);
    return it != keep_.end() && it->second;
}

bool Privacy::masks(uint16_t mode, bool hasPosition) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto k = keep_.find(mode);
    if (k != keep_.end() && k->second) return false;
    auto c = cls_.find(mode);
    if (c == cls_.end()) return false;
    return c->second == IdClass::Personal || (c->second == IdClass::WithPosition && hasPosition);
}

std::string Privacy::protect(uint16_t mode, const std::string& id, bool hasPosition, const char* label) const {
    if (id.empty() || !masks(mode, hasPosition)) return id;
    return shortTag(mode, id, label);
}

void Privacy::applyDefaultClasses() {
    // docs/MODE-NUMBERS.md family numbers
    static const uint16_t personal[] = {
        51,    // Bluetooth LE
        143,   // BLE tracker detector (the ids are never written anywhere, bletrk_book.h)
        54,    // Wi-Fi networks
        70,    // Wi-Fi HaLow
        65,    // Zigbee / 802.15.4
        98,    // Z-Wave
        153,   // Sigfox
        77,    // ANT+ (ids and heart rate)
        148,   // NFC / RFID (UIDs)
        78,    // DECT (phone identities)
        86,    // Wireless M-Bus (meter ids and readings)
        28,    // ISM sensors (TPMS, keyless entry)
        56,    // Drone Remote ID (operator and pilot location)
        68,    // DJI drones
        66,    // FLARM
        23,    // POCSAG (capcodes)
        17, 18, 19, 62, 208,   // satellite terminals: Inmarsat-C, Inmarsat Aero, Iridium, Globalstar, Inmarsat (all carriers)
    };
    static const uint16_t withPos[] = {
        9,     // DMR
        38,    // NXDN
        50, 180,   // P25 phase 1 and 2
        45,    // dPMR
        46,    // TETRA
        31,    // M17
        33,    // D-STAR
        128,   // Yaesu Fusion
    };
    std::lock_guard<std::mutex> lk(mu_);
    for (uint16_t m : personal) cls_[m] = IdClass::Personal;
    for (uint16_t m : withPos) cls_[m] = IdClass::WithPosition;
}

void Privacy::setRedactText(bool on) { std::lock_guard<std::mutex> lk(mu_); redact_ = on; }
bool Privacy::redactText() const { std::lock_guard<std::mutex> lk(mu_); return redact_; }

std::string Privacy::text(const std::string& s) const {
    if (s.empty() || !redactText()) return s;
    size_t chars = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) chars++;
    return "[redacted, " + std::to_string(chars) + " characters]";
}

std::string Privacy::save() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::string o = std::string("redact_text=") + (redact_ ? "1" : "0") + "\n";
    for (const auto& kv : keep_) if (kv.second) o += "keep_real_ids=" + std::to_string(kv.first) + "\n";
    return o;
}

void Privacy::load(const std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    keep_.clear();
    redact_ = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "redact_text") redact_ = v == "1";
        else if (k == "keep_real_ids") { const long m = strtol(v.c_str(), nullptr, 10); if (m >= 0 && m <= 65535) keep_[(uint16_t)m] = true; }
    }
}

Privacy& privacy() {
    static Privacy* p = [] { auto* x = new Privacy; x->applyDefaultClasses(); return x; }();   // never destroyed: threads may still use it at exit
    return *p;
}

// ------------------------------------------------------------------ escaping

bool validUtf8(const std::string& s) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = p[i];
        size_t len;
        uint32_t cp;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; k++) { if ((p[i + k] & 0xC0) != 0x80) return false; cp = (cp << 6) | (p[i + k] & 0x3F); }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

namespace {
// Copies `s` with every invalid UTF-8 byte replaced by U+FFFD
std::string sanitizeUtf8(const std::string& s) {
    if (validUtf8(s)) return s;
    std::string o;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = p[i];
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (len == 1) { o += (char)c; i++; continue; }
        if (len && i + len <= n && validUtf8(s.substr(i, len))) { o.append(s, i, len); i += len; continue; }
        o += "\xEF\xBF\xBD";
        i++;
    }
    return o;
}
}  // namespace

std::string stripControl(const std::string& s) {
    const std::string u = sanitizeUtf8(s);
    std::string o;
    o.reserve(u.size());
    for (unsigned char c : u) {
        if (c == '\t') { o += ' '; continue; }
        if (c < 0x20 || c == 0x7F) continue;
        o += (char)c;
    }
    return o;
}

std::string csvCell(const std::string& s) {
    std::string t = stripControl(s);
    size_t i = 0;
    while (i < t.size() && t[i] == ' ') i++;
    if (i < t.size() && (t[i] == '=' || t[i] == '+' || t[i] == '-' || t[i] == '@')) t.insert(0, "'");
    if (t.find_first_of(",\"") == std::string::npos) return t;
    std::string o = "\"";
    for (char c : t) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

std::string jsonEscape(const std::string& s) {
    const std::string u = sanitizeUtf8(s);
    std::string o;
    o.reserve(u.size() + 8);
    for (unsigned char c : u) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7F) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += (char)c;
        }
    }
    return o;
}

std::string srtText(const std::string& s) {
    const std::string u = sanitizeUtf8(s);
    std::string o;
    for (unsigned char c : u) {
        if (c == '\r') continue;
        if (c == '\n') { if (!o.empty() && o.back() != '\n') o += '\n'; continue; }   // never an empty line inside a cue
        if (c < 0x20 || c == 0x7F) continue;
        o += (char)c;
    }
    while (!o.empty() && o.back() == '\n') o.pop_back();
    return o;
}

std::string cutUtf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;   // s[n] is the first byte that would be cut: back up to a character start
    return s.substr(0, n);
}

// ------------------------------------------------------------------ owner-only files

bool restrictToOwner(const std::string& path, bool isDir) {
#ifdef _WIN32
    // a protected DACL: full control for the owner only, nothing inherited from the parent folder
    PSECURITY_DESCRIPTOR sd = nullptr;
    const wchar_t* sddl = isDir ? L"D:P(A;OICI;FA;;;OW)" : L"D:P(A;;FA;;;OW)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    const std::wstring w = pathOf(path).wstring();
    const BOOL ok = SetFileSecurityW(w.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, sd);
    LocalFree(sd);
    return ok != 0;
#else
    return chmod(path.c_str(), isDir ? 0700 : 0600) == 0;
#endif
}

bool makeOwnerOnlyDir(const std::string& path) {
    std::error_code ec;
    const fs::path p = pathOf(path);
    if (fs::is_directory(p, ec)) return true;   // an existing folder is the user's: its permissions are not touched
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    fs::create_directory(p, ec);
    if (!fs::is_directory(p, ec)) return false;
    return restrictToOwner(path, true);
}

bool writeOwnerOnlyFile(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(pathOf(tmp), std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(data.data(), (std::streamsize)data.size());
        if (!out) return false;
    }
    if (!restrictToOwner(tmp, false)) { std::error_code ec; fs::remove(pathOf(tmp), ec); return false; }
    std::error_code ec;
    fs::rename(pathOf(tmp), pathOf(path), ec);
    if (ec) { fs::remove(pathOf(tmp), ec); return false; }
    return restrictToOwner(path, false);
}

}  // namespace dect2::sess
