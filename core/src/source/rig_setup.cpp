// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/source/rig_setup.h"
#include "dect2/common/str.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#endif

namespace dect2 {

namespace {

std::string lower(const std::string& s) { return dect2::str::lower(s); }
// lower case with spaces, dashes, underscores, dots and slashes removed: "IC-R8600" and "ic r8600" compare equal
std::string squash(const std::string& s) {
    std::string r;
    for (unsigned char c : s)
        if (isalnum(c)) r += (char)tolower(c);
    return r;
}
bool startsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }
bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (!isdigit(c)) return false;
    return true;
}
// number at the end of a name, 0 for none: "COM12" -> 12, "ttyUSB3" -> 3
int trailingNumber(const std::string& s) {
    size_t i = s.size();
    while (i > 0 && isdigit((unsigned char)s[i - 1])) i--;
    return i == s.size() ? 0 : atoi(s.c_str() + i);
}
// a word of the name (split at anything that is not a letter or digit) begins with sw: "IC-R8600" has "r8600"
bool wordStarts(const std::string& name, const std::string& sw) {
    std::string cur;
    for (size_t i = 0; i <= name.size(); i++) {
        if (i < name.size() && isalnum((unsigned char)name[i])) { cur += (char)tolower((unsigned char)name[i]); continue; }
        if (!cur.empty() && cur.compare(0, sw.size(), sw) == 0) return true;
        cur.clear();
    }
    return false;
}
const int kBauds[] = {300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};

} // namespace

std::string rigModelLabel(const RigModelInfo& m) {
    std::string s = m.mfg;
    if (!s.empty() && !m.model.empty()) s += ' ';
    s += m.model;
    return s + " (id " + std::to_string(m.id) + ")";
}

const char* rigStatusName(int status) {
    switch (status) {
    case 0: return "alpha";
    case 1: return "untested";
    case 2: return "beta";
    case 3: return "stable";
    case 4: return "buggy";
    default: return "?";
    }
}

int findRigModel(const std::vector<RigModelInfo>& all, int id) {
    for (size_t i = 0; i < all.size(); i++) if (all[i].id == id) return (int)i;
    return -1;
}

std::vector<int> searchRigModels(const std::vector<RigModelInfo>& all, const std::string& query) {
    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : query) {
            if (c == ' ' || c == '\t') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) words.push_back(cur);
    }
    std::vector<int> out;
    if (words.empty()) {
        for (size_t i = 0; i < all.size(); i++) out.push_back((int)i);
        return out;
    }
    // score: lower is better. 0 exact id, 1 id starts with the digits, 2 model name starts with it, 3 anywhere
    std::vector<std::pair<int, int>> scored;
    for (size_t i = 0; i < all.size(); i++) {
        const auto& m = all[i];
        const std::string idS = std::to_string(m.id);
        const std::string hay = squash(m.mfg) + squash(m.model) + idS;
        const std::string mdl = squash(m.model);
        int score = 3;
        bool ok = true;
        for (const auto& w : words) {
            const std::string sw = squash(w);
            if (sw.empty()) continue;
            int s = 3;
            if (allDigits(w) && idS == w) s = 0;
            else if (allDigits(w) && startsWith(idS, w.c_str())) s = 1;
            else if (mdl.compare(0, sw.size(), sw) == 0 || wordStarts(m.model, sw)) s = 2;
            else if (hay.find(sw) != std::string::npos) s = 3;
            else { ok = false; break; }
            // a name word may match across mfg+model ("icr8600"): handled by hay; keep the best score of the words as the rank
            if (&w == &words.front() || s < score) score = s;
        }
        if (ok) scored.push_back({score, (int)i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& p : scored) out.push_back(p.second);
    return out;
}

std::vector<int> rigBaudChoices(const RigModelInfo& m) {
    std::vector<int> v;
    const bool known = m.baudMax > 0 && m.baudMax >= m.baudMin;
    for (int b : kBauds)
        if (!known || (b >= m.baudMin && b <= m.baudMax)) v.push_back(b);
    // a fixed or unusual speed the model asks for is always offered
    if (known) {
        for (int b : {m.baudMin, m.baudMax})
            if (b > 0 && std::find(v.begin(), v.end(), b) == v.end()) v.push_back(b);
        std::sort(v.begin(), v.end());
    }
    return v;
}

int rigDefaultBaud(const RigModelInfo& m) {
    if (m.network || m.baudMax <= 0) return 0;
    if (m.baudMin == m.baudMax) return m.baudMax;
    // Hamlib backends list a range; the usual real speed is 9600 when the range holds it, else the nearest end of the range
    if (m.baudMin <= 9600 && m.baudMax >= 9600) return 9600;
    return m.baudMin > 9600 ? m.baudMin : m.baudMax;
}

bool looksLikeNetworkRigPath(const std::string& path) {
    if (path.empty() || path[0] == '/' || path[0] == '\\') return false;
    if (startsWith(lower(path), "com") && allDigits(path.substr(3))) return false;
    const size_t c = path.rfind(':');
    if (c == std::string::npos || c == 0 || c + 1 >= path.size()) return false;
    if (!allDigits(path.substr(c + 1))) return false;
    const int port = atoi(path.c_str() + c + 1);
    if (port < 1 || port > 65535) return false;
    // a bare IPv6 address without brackets has colons but is not host:port
    const std::string host = path.substr(0, c);
    if (host.find(':') != std::string::npos && host.front() != '[') return false;
    return true;
}

std::vector<SerialPortInfo> filterSerialPorts(PortPlatform platform, const std::vector<std::string>& devEntries,
                                              const std::vector<std::pair<std::string, std::string>>& named) {
    std::vector<SerialPortInfo> out;
    auto addUnique = [&](SerialPortInfo p) {
        for (auto& o : out)
            if (o.path == p.path) {
                if (o.display == o.path && p.display != p.path) o.display = p.display;
                return;
            }
        out.push_back(std::move(p));
    };
    if (platform == PortPlatform::Mac) {
        for (const auto& e : devEntries) {
            if (!startsWith(e, "cu.")) continue;
            const std::string l = lower(e);
            if (l.find("bluetooth") != std::string::npos || l.find("debug-console") != std::string::npos || l.find("wlan-debug") != std::string::npos) continue;
            SerialPortInfo p;
            p.path = "/dev/" + e;
            p.display = p.path;
            p.usb = l.find("usbserial") != std::string::npos || l.find("usbmodem") != std::string::npos || l.find("slab") != std::string::npos ||
                    l.find("wchusbserial") != std::string::npos;
            addUnique(std::move(p));
        }
    } else if (platform == PortPlatform::Linux) {
        for (const auto& e : devEntries) {
            const bool usb = startsWith(e, "ttyUSB") || startsWith(e, "ttyACM");
            const bool s = startsWith(e, "ttyS") && allDigits(e.substr(4));
            if (!usb && !s) continue;
            SerialPortInfo p;
            p.path = "/dev/" + e;
            p.display = p.path;
            p.usb = usb;
            addUnique(std::move(p));
        }
        // named: (by-id name, resolved /dev path or empty)
        for (const auto& n : named) {
            SerialPortInfo p;
            p.path = "/dev/serial/by-id/" + n.first;
            p.display = n.first;
            p.usb = true;
            // the same device also appears as /dev/ttyUSBn: show the friendly name on that one and keep the stable by-id path too
            if (!n.second.empty()) {
                for (auto& o : out)
                    if (o.path == n.second) o.display = o.path + "  (" + n.first + ")";
            }
            addUnique(std::move(p));
        }
    } else {
        for (const auto& e : devEntries) {
            const std::string l = lower(e);
            if (!startsWith(l, "com") || !allDigits(e.substr(3))) continue;
            SerialPortInfo p;
            p.path = "COM" + e.substr(3);
            p.display = p.path;
            p.usb = false;
            addUnique(std::move(p));
        }
        for (const auto& n : named) {
            for (auto& o : out)
                if (lower(o.path) == lower(n.first) && !n.second.empty()) { o.display = o.path + "  (" + n.second + ")"; o.usb = lower(n.second).find("usb") != std::string::npos; }
        }
    }
    auto rank = [&](const SerialPortInfo& p) { return platform == PortPlatform::Linux && p.path.find("/dev/ttyS") == 0 ? 2 : (p.usb ? 0 : 1); };
    std::stable_sort(out.begin(), out.end(), [&](const SerialPortInfo& a, const SerialPortInfo& b) {
        const int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        if (platform == PortPlatform::Windows) return trailingNumber(a.path) < trailingNumber(b.path);
        return a.path < b.path;
    });
    return out;
}

std::vector<SerialPortInfo> listSerialPorts() {
#ifdef _WIN32
    std::vector<std::string> coms;
    std::vector<std::pair<std::string, std::string>> named;
    HKEY h;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_QUERY_VALUE, &h) == ERROR_SUCCESS) {
        for (DWORD i = 0;; i++) {
            char name[256], data[256];
            DWORD nl = sizeof name, dl = sizeof data - 1, type = 0;
            const LONG r = RegEnumValueA(h, i, name, &nl, nullptr, &type, (LPBYTE)data, &dl);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r != ERROR_SUCCESS || type != REG_SZ) continue;
            data[dl < sizeof data ? dl : sizeof data - 1] = 0;
            coms.push_back(data);
            // the value name is the driver's device: \Device\USBSER000, \Device\Silabser0, \Device\VCP0, \Device\Serial0 (a real COM port)
            const std::string l = lower(name);
            std::string friendly;
            if (l.find("usbser") != std::string::npos) friendly = "USB serial";
            else if (l.find("silabser") != std::string::npos) friendly = "USB serial (Silicon Labs)";
            else if (l.find("vcp") != std::string::npos || l.find("ftdi") != std::string::npos) friendly = "USB serial (FTDI)";
            else if (l.find("ch34") != std::string::npos || l.find("wch") != std::string::npos) friendly = "USB serial (CH340)";
            else if (l.find("bth") != std::string::npos) friendly = "Bluetooth";
            if (!friendly.empty()) named.push_back({data, friendly});
        }
        RegCloseKey(h);
    }
    return filterSerialPorts(PortPlatform::Windows, coms, named);
#else
    std::vector<std::string> entries;
    if (DIR* d = opendir("/dev")) {
        while (const dirent* e = readdir(d)) entries.push_back(e->d_name);
        closedir(d);
    }
    std::vector<std::pair<std::string, std::string>> named;
#ifdef __APPLE__
    return filterSerialPorts(PortPlatform::Mac, entries, named);
#else
    if (DIR* d = opendir("/dev/serial/by-id")) {
        while (const dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            char target[PATH_MAX];
            const std::string link = std::string("/dev/serial/by-id/") + e->d_name;
            const char* rp = realpath(link.c_str(), target);
            named.push_back({e->d_name, rp ? std::string(rp) : std::string()});
        }
        closedir(d);
    }
    return filterSerialPorts(PortPlatform::Linux, entries, named);
#endif
#endif
}

#ifndef DECT2_HAVE_HAMLIB
std::vector<RigModelInfo> hamlibModelList() { return {}; }
#endif

} // namespace dect2
