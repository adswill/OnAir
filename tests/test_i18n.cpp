// The interface translations (app/i18n/*.txt):
//  - every catalog key is text the sources pass to tr(), trw() or N_() (no stale keys); fails otherwise
//  - every such text missing from a catalog is counted and listed as a warning (the English text is shown for it)
//  - a translation keeps the printf conversions of its key, in the same order (they are format strings); fails otherwise
//  - the lookup itself: language codes with regions, "Label##id", trw()'s fixed ID
#include "i18n.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

// the C escapes of a string literal's body
static std::string unescapeC(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        const char c = s[++i];
        if (c == 'n') o += '\n';
        else if (c == 't') o += '\t';
        else if (c == 'x') {
            int v = 0, n = 0;
            while (i + 1 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && n < 2) { v = v * 16 + std::stoi(std::string(1, s[++i]), nullptr, 16); n++; }
            o += (char)v;
        } else o += c;
    }
    return o;
}

// the texts inside every tr( ... ), trw( ... ), N_( ... ) of a source file; adjacent literals are joined as the compiler does
static void collect(const std::string& src, std::set<std::string>& used) {
    for (size_t p = 0; p < src.size(); p++) {
        size_t open = std::string::npos;
        for (const char* f : {"trw(", "tr(", "N_("}) {
            const size_t n = std::strlen(f);
            if (src.compare(p, n, f) == 0 && (p == 0 || !(std::isalnum((unsigned char)src[p - 1]) || src[p - 1] == '_' || src[p - 1] == ':' || src[p - 1] == '.'))) { open = p + n; break; }
        }
        if (open == std::string::npos) continue;
        int depth = 0;
        std::string cur;
        bool inLit = false, haveLit = false;
        size_t i = open;
        for (; i < src.size(); i++) {
            const char c = src[i];
            if (c == '"') {
                size_t j = i + 1;
                while (j < src.size() && src[j] != '"') j += src[j] == '\\' ? 2 : 1;
                cur += unescapeC(src.substr(i + 1, j - i - 1));
                haveLit = true; inLit = true;
                i = j;
                continue;
            }
            if (c == '\'') { size_t j = i + 1; while (j < src.size() && src[j] != '\'') j += src[j] == '\\' ? 2 : 1; i = j; continue; }
            if (std::isspace((unsigned char)c)) continue;
            if (inLit) { used.insert(cur); cur.clear(); inLit = false; }
            if (c == '(') depth++;
            else if (c == ')') { if (depth == 0) break; depth--; }
        }
        if (inLit) used.insert(cur);
        (void)haveLit;
        p = open - 1;
    }
}

static std::vector<std::string> specifiers(const std::string& s) {
    static const std::regex re(R"(%[-+ #0]*[0-9*]*(\.[0-9*]+)?(hh|h|ll|l|z|j|t|L)?[diouxXeEfFgGaAcsp%])");
    std::vector<std::string> v;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it) v.push_back(it->str());
    return v;
}

static std::string unescapeCatalog(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { const char c = s[++i]; o += c == 'n' ? '\n' : c == 't' ? '\t' : c; }
        else o += s[i];
    }
    return o;
}

static std::string escapeCatalog(const std::string& s) {
    std::string o;
    for (char c : s) o += c == '\n' ? std::string("\\n") : c == '\t' ? std::string("\\t") : c == '\\' ? std::string("\\\\") : std::string(1, c);
    return o;
}

int main(int argc, char** argv) {
    const bool dump = argc > 1 && !std::strcmp(argv[1], "--dump");   // the texts to translate, one per line in the catalog's escaping
    // the sources
    std::set<std::string> used;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(DECT2_SOURCE_DIR) / "app")) {   // the modes live in app/<mode>/
        const std::string ext = e.path().extension().string(), name = e.path().filename().string();
        if ((ext != ".cpp" && ext != ".h" && ext != ".mm" && ext != ".inc") || name == "i18n.cpp" || name == "i18n.h") continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        collect(ss.str(), used);
    }
    std::set<std::string> keys = used;   // "Label##id": the catalog has the label
    for (const std::string& u : used) { const size_t h = u.find("##"); if (h != std::string::npos && h > 0) keys.insert(u.substr(0, h)); }
    if (dump) {
        for (const std::string& u : used) { const size_t h = u.find("##"); const std::string k = h != std::string::npos && h > 0 ? u.substr(0, h) : u; if (!k.empty()) std::printf("%s\n", escapeCatalog(k).c_str()); }
        return 0;
    }
    std::printf("%zu texts marked for translation in app/\n", used.size());
    CHECK(used.count("Start receiver") && used.count("BROADCAST RADIO") && used.count("Peak hold"), "the scan of the sources found too little");

    int n = 0;
    const i18n::Language* langs = i18n::languages(n);
    CHECK(n >= 7 && !std::strcmp(langs[0].code, "en"), "the language list");
    for (int li = 1; li < n; li++) {
        const char* text = i18n::catalogText(langs[li].code);
        CHECK(text != nullptr, "no catalog compiled in for %s", langs[li].code);
        if (!text) continue;
        std::istringstream in(text);
        std::string line;
        std::map<std::string, std::string> cat;
        int lineNo = 0, stale = 0, badFmt = 0;
        while (std::getline(in, line)) {
            lineNo++;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const size_t tab = line.find('\t');
            CHECK(tab != std::string::npos, "%s.txt:%d has no tab", langs[li].code, lineNo);
            if (tab == std::string::npos) continue;
            const std::string k = unescapeCatalog(line.substr(0, tab)), v = unescapeCatalog(line.substr(tab + 1));
            CHECK(!cat.count(k), "%s.txt:%d repeats \"%s\"", langs[li].code, lineNo, k.c_str());
            cat[k] = v;
            if (!keys.count(k)) { stale++; CHECK(false, "%s.txt:%d: \"%s\" is not in any tr()/trw()/N_() of the sources (stale key)", langs[li].code, lineNo, k.c_str()); }
            if (!v.empty() && specifiers(k) != specifiers(v)) { badFmt++; CHECK(false, "%s.txt:%d: the printf conversions differ: \"%s\" -> \"%s\"", langs[li].code, lineNo, k.c_str(), v.c_str()); }
            CHECK(v.find("##") == std::string::npos || k.find("##") != std::string::npos, "%s.txt:%d: the translation has a ## the key has not", langs[li].code, lineNo);
        }
        int missing = 0;
        std::string sample;
        for (const std::string& u : used) {
            const size_t h = u.find("##");
            const std::string k = h != std::string::npos && h > 0 ? u.substr(0, h) : u;
            if (k.empty() || cat.count(k) || cat.count(u)) continue;
            if (missing++ < 8) sample += "\n    \"" + k + "\"";
        }
        std::printf("%s: %zu entries, %d stale, %d with other conversions; warning: %d texts without a translation%s\n",
                    langs[li].code, cat.size(), stale, badFmt, missing, sample.c_str());
    }

    // the lookup
    CHECK(!std::strcmp(i18n::set("de-AT"), "de"), "de-AT -> de");
    CHECK(!std::strcmp(i18n::set("zh_CN.UTF-8"), "zh"), "zh_CN.UTF-8 -> zh");
    CHECK(!std::strcmp(i18n::set("zh-Hans-CN"), "zh"), "zh-Hans-CN -> zh");
    CHECK(!std::strcmp(i18n::set("pt-BR"), "pt") && !std::strcmp(i18n::set("ru_RU.UTF-8"), "ru") && !std::strcmp(i18n::set("it-IT"), "it"), "pt-BR -> pt, ru_RU.UTF-8 -> ru, it-IT -> it");
    CHECK(!std::strcmp(i18n::set("xx-YY"), "en"), "an unknown language -> en");
    i18n::set("en");
    CHECK(!std::strcmp(tr("Peak hold"), "Peak hold"), "English is the key itself");
    CHECK(!std::strcmp(trw("Passes"), "Passes###Passes"), "trw in English: %s", trw("Passes"));
    i18n::set("lt");
    CHECK(std::strcmp(tr("Peak hold"), "Peak hold") != 0, "Lithuanian has Peak hold");
    const std::string w = trw("Passes"), l = tr("Listen##ft8");
    CHECK(w.size() > 9 && w.compare(w.size() - 9, 9, "###Passes") == 0 && w.rfind("Passes", 0) != 0, "trw in Lithuanian: %s", w.c_str());
    CHECK(l.size() > 5 && l.compare(l.size() - 5, 5, "##ft8") == 0 && l.rfind("Listen", 0) != 0, "Label##id in Lithuanian: %s", l.c_str());
    CHECK(tr("Peak hold") == tr("Peak hold"), "the same pointer every call");
    CHECK(!std::strcmp(tr("no such text, never translated"), "no such text, never translated"), "an unknown text stays English");
    i18n::set("en");

    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
