// Which modes the mode lists show: the rules are plain strings and sets (no ImGui, no mode table), so they are unit-tested.
// Modes and groups are named by their text, never by number. Hidden modes stay reachable: by search (tagged "hidden"), --mode, --pick and deep links.
#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <string>
#include <vector>

namespace modevis {

struct Rules {
    std::set<std::string> hiddenModes, hiddenGroups, favs;
    bool favsOnly = false;
};

// "a|b|c" <-> set (names never hold a |)
inline std::set<std::string> parseList(const std::string& s) {
    std::set<std::string> out;
    size_t b = 0;
    while (b <= s.size()) {
        size_t e = s.find('|', b);
        if (e == std::string::npos) e = s.size();
        if (e > b) out.insert(s.substr(b, e - b));
        b = e + 1;
    }
    return out;
}
inline std::string joinList(const std::set<std::string>& v) {
    std::string out;
    for (const std::string& s : v) { if (!out.empty()) out += "|"; out += s; }
    return out;
}

// hidden by the rules, ignoring that the mode may be the active one
inline bool hiddenByRules(const Rules& r, const std::string& group, const std::string& mode) {
    if (r.hiddenModes.count(mode) || r.hiddenGroups.count(group)) return true;
    return r.favsOnly && !r.favs.count(mode);
}
// hidden from the lists now: the active mode is never hidden
inline bool hidden(const Rules& r, const std::string& group, const std::string& mode, bool active) {
    return !active && hiddenByRules(r, group, mode);
}
// listed in a menu, picker or palette: a search lists the hidden ones too (the caller tags them)
inline bool listed(const Rules& r, const std::string& group, const std::string& mode, bool active, bool searching) {
    return searching || !hidden(r, group, mode, active);
}

// a group's tick in the settings tree: shown, hidden, or some of its modes hidden
enum class GroupState { Shown, Hidden, Mixed };
inline GroupState groupState(const Rules& r, const std::string& group, const std::vector<std::string>& modes) {
    int shown = 0;
    for (const std::string& m : modes) if (!hiddenByRules(r, group, m)) shown++;
    return shown == 0 ? GroupState::Hidden : shown == (int)modes.size() ? GroupState::Shown : GroupState::Mixed;
}
// tick or untick a whole group: the group flag goes and the single hides of its modes with it
inline void setGroupShown(Rules& r, const std::string& group, const std::vector<std::string>& modes, bool on) {
    for (const std::string& m : modes) r.hiddenModes.erase(m);
    if (on) r.hiddenGroups.erase(group); else r.hiddenGroups.insert(group);
}
// tick or untick one mode; ticking one in a hidden group opens the group and hides its other modes singly
inline void setModeShown(Rules& r, const std::string& group, const std::string& mode, const std::vector<std::string>& siblings, bool on) {
    if (on) {
        if (r.hiddenGroups.erase(group)) for (const std::string& s : siblings) if (s != mode) r.hiddenModes.insert(s);
        r.hiddenModes.erase(mode);
    } else r.hiddenModes.insert(mode);
}
inline void showAll(Rules& r) { r.hiddenModes.clear(); r.hiddenGroups.clear(); r.favsOnly = false; }
inline void hideAll(Rules& r, const std::vector<std::string>& groups) { r.hiddenModes.clear(); r.hiddenGroups.insert(groups.begin(), groups.end()); }
inline void reset(Rules& r) { showAll(r); r.favs.clear(); }
inline bool isDefault(const Rules& r) { return r.hiddenModes.empty() && r.hiddenGroups.empty() && !r.favsOnly; }

inline std::string lowerAscii(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

// presets are lists of words found in group names, so groups other agents add are covered by their names
inline const std::vector<std::string>& presetNames() {
    static const std::vector<std::string> n = {"Ham", "Broadcast", "Aviation", "Marine", "Everything"};
    return n;
}
inline std::vector<std::string> presetWords(const std::string& preset) {
    if (preset == "Ham") return {"amateur", "general", "plugins", "kris"};
    if (preset == "Broadcast") return {"tv", "broadcast", "general", "plugins"};
    if (preset == "Aviation") return {"aviation", "general", "plugins"};
    if (preset == "Marine") return {"maritime", "satellite", "weather", "general", "plugins"};
    return {};
}
// the preset's hidden groups, among all group names; Everything (or an unknown preset) hides none
inline std::set<std::string> presetHiddenGroups(const std::string& preset, const std::vector<std::string>& groups) {
    std::set<std::string> out;
    const std::vector<std::string> w = presetWords(preset);
    if (w.empty()) return out;
    for (const std::string& g : groups) {
        const std::string l = lowerAscii(g);
        bool keep = false;
        for (const std::string& k : w) keep |= l.find(k) != std::string::npos;
        if (!keep) out.insert(g);
    }
    return out;
}
// applying a preset keeps the favourites and the Favourites-only switch
inline void applyPreset(Rules& r, const std::string& preset, const std::vector<std::string>& groups) {
    r.hiddenModes.clear();
    r.hiddenGroups = presetHiddenGroups(preset, groups);
}

// the settings tree's search: a group matches by its name or by any of its modes
inline bool containsNoCase(const std::string& hay, const std::string& needle) { return needle.empty() || lowerAscii(hay).find(lowerAscii(needle)) != std::string::npos; }

// ---- the saved copy: a small file of its own (modes-shown.txt in the data folder), written the moment a tick changes and read at the start.
// It does not depend on settings.conf, which a startup guard can set aside or a locked folder can keep from being replaced.
// Four lines: "hiddenModes=a|b", "hiddenGroups=...", "favs=...", "favsOnly=0/1". A name never holds a | or a line break.
inline std::string toText(const Rules& r) {
    return "hiddenModes=" + joinList(r.hiddenModes) + "\nhiddenGroups=" + joinList(r.hiddenGroups) + "\nfavs=" + joinList(r.favs) +
           "\nfavsOnly=" + (r.favsOnly ? "1" : "0") + "\n";
}
inline bool fromText(const std::string& text, Rules& out) {
    Rules r;
    std::istringstream in(text);
    std::string line;
    int seen = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "hiddenModes") { r.hiddenModes = parseList(v); seen++; }
        else if (k == "hiddenGroups") { r.hiddenGroups = parseList(v); seen++; }
        else if (k == "favs") { r.favs = parseList(v); seen++; }
        else if (k == "favsOnly") { r.favsOnly = v == "1"; seen++; }
    }
    if (seen < 4) return false;   // not our file (or cut short): the caller keeps what it has
    out = r;
    return true;
}
inline std::filesystem::path u8path(const std::string& p) { return std::filesystem::path(reinterpret_cast<const char8_t*>(p.c_str())); }
// write to a temp file and replace; when the replace is refused (a virus scanner or a sync client holds the file) write in place
inline bool writeFile(const std::string& path, const Rules& r) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (u8path(path).has_parent_path()) fs::create_directories(u8path(path).parent_path(), ec);
    const std::string text = toText(r), tmp = path + ".tmp";
    {
        std::ofstream f(u8path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        f.flush();
        if (!f) return false;
    }
    fs::rename(u8path(tmp), u8path(path), ec);
    if (!ec) return true;
    fs::remove(u8path(tmp), ec);
    std::ofstream f(u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    f.flush();
    return (bool)f;
}
inline bool readFile(const std::string& path, Rules& out) {
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return fromText(ss.str(), out);
}

}  // namespace modevis
