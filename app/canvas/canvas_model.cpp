// The Workspace layout's model (canvas_model.h): blocks, links, tab groups, snapping, the text form, templates.
#include "canvas_model.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <locale>
#include <set>
#include <sstream>

namespace canvas {

const char* const kGroupNames[kNumGroups] = {"Radio", "Listen", "Decode", "Station", "Other"};
const char* const kIdleMode = "55";

namespace {
const KindInfo kInfo[(int)Kind::Count] = {
    {Kind::Source, "source", "Radio source", 0, "One receiver: the radio, its frequency, sample rate and gains. Add one per radio for several at once.", false, false, 360, 330},
    {Kind::Spectrum, "spectrum", "Spectrum and waterfall", 0, "The spectrum above the waterfall of a source.", true, false, 640, 360},
    {Kind::Demod, "demod", "Demodulator", 1, "A channel of its own on a source: its own frequency offset, demodulator, bandwidth and squelch.", true, false, 340, 210},
    {Kind::Audio, "audio", "Audio output", 1, "Which source is heard, the volume, the output device and the channels mixed in.", true, false, 360, 260},
    {Kind::Decoder, "decoder", "Decoder", 2, "Any mode of the mode list on a source, with its results views.", true, false, 640, 440},
    {Kind::Map, "map", "Map", 2, "Aircraft, ships, sondes, satellites and stations on a map.", false, true, 560, 420},
    {Kind::Tracker, "tracker", "Satellite tracker", 3, "Satellite passes, the sky plot, Doppler and the automatic follow.", false, true, 620, 460},
    {Kind::Rotator, "rotator", "Antenna rotator", 3, "GS-232, Hamlib rotctld or ASCOM Alpaca rotator and mount control.", false, true, 380, 420},
    {Kind::Transmitter, "tx", "Transmitter", 3, "The guided transmit panel of a source (the usual warnings; nothing is blocked).", true, false, 560, 480},
    {Kind::Recorder, "recorder", "Recorder", 0, "IQ and audio recording of a source.", true, false, 420, 190},
    {Kind::Log, "log", "Log", 4, "The log of a source's receiver.", true, false, 520, 220},
    {Kind::History, "history", "History", 4, "The decoded history of a source.", true, false, 560, 320},
    {Kind::Frequencies, "frequencies", "Frequency list", 4, "The frequency database: known frequencies by mode and place.", false, true, 560, 400},
    {Kind::Bookmarks, "bookmarks", "Bookmarks", 4, "The memories (bookmarked frequencies) and their lists.", false, true, 520, 400},
    {Kind::Scanner, "scanner", "Scanner", 1, "The scanner: steps through a list of frequencies and stops on activity.", false, true, 560, 420},
    {Kind::Clock, "clock", "Clock", 4, "UTC and local time.", false, false, 240, 120},
    {Kind::Notes, "notes", "Notes", 4, "Free text, saved with the workspace.", false, false, 300, 200},
};

std::string fmtNum(double v) {
    char b[48];
    if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%lld", (long long)std::llround(v));
    else std::snprintf(b, sizeof b, "%.9g", v);
    std::string s = b;
    for (char& c : s) if (c == ',') c = '.';   // a locale with a decimal comma
    return s;
}

double parseNum(const std::string& s, double def) {
    if (s.empty()) return def;
    std::string t = s;
    for (char& c : t) if (c == ',') c = '.';
    std::istringstream is(t);
    is.imbue(std::locale::classic());
    double v = def;
    if (!(is >> v)) return def;
    return v;
}

std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> v;
    std::string cur;
    bool any = false;
    for (char c : line) {
        if (c == ' ' || c == '\t') { if (any) { v.push_back(cur); cur.clear(); any = false; } }
        else { cur += c; any = true; }
    }
    if (any) v.push_back(cur);
    return v;
}
}  // namespace

const KindInfo& info(Kind k) { return kInfo[std::max(0, std::min((int)Kind::Count - 1, (int)k))]; }
const KindInfo* infoByKey(const std::string& key) {
    for (const KindInfo& i : kInfo) if (key == i.key) return &i;
    return nullptr;
}

std::string Block::get(const std::string& k, const std::string& def) const {
    const auto it = cfg.find(k);
    return it == cfg.end() ? def : it->second;
}
int Block::getI(const std::string& k, int def) const { return (int)std::lround(parseNum(get(k), def)); }
double Block::getD(const std::string& k, double def) const { return parseNum(get(k), def); }
void Block::setD(const std::string& k, double v) { cfg[k] = fmtNum(v); }

Block* Workspace::find(int id) {
    for (Block& b : blocks) if (b.id == id) return &b;
    return nullptr;
}
const Block* Workspace::find(int id) const {
    for (const Block& b : blocks) if (b.id == id) return &b;
    return nullptr;
}

int Workspace::add(Kind k, float x, float y) {
    Block b;
    b.id = nextId++;
    b.kind = k;
    b.r = Rect{x, y, info(k).w, info(k).h};
    if (info(k).needsSource) {
        const std::vector<int> s = sources();
        if (!s.empty()) b.source = s.back();
    }
    blocks.push_back(b);
    return b.id;
}

bool Workspace::remove(int id, std::vector<int>* unlinked) {
    const auto it = std::find_if(blocks.begin(), blocks.end(), [id](const Block& b) { return b.id == id; });
    if (it == blocks.end()) return false;
    const int group = it->tab;
    blocks.erase(it);
    for (Block& b : blocks) if (b.source == id) { b.source = 0; if (unlinked) unlinked->push_back(b.id); }
    if (group) {
        const std::vector<int> m = tabMembers(group);
        if (m.size() == 1) { if (Block* o = find(m[0])) o->tab = 0; activeTab.erase(group); }
        else if (!m.empty() && activeTab[group] == id) activeTab[group] = m[0];
        else if (m.empty()) activeTab.erase(group);
    }
    return true;
}

bool Workspace::link(int id, int sourceId) {
    Block* b = find(id);
    if (!b || !info(b->kind).needsSource) return false;
    if (sourceId) {
        const Block* s = find(sourceId);
        if (!s || s->kind != Kind::Source) return false;
    }
    b->source = sourceId;
    return true;
}

std::vector<int> Workspace::sources() const {
    std::vector<int> v;
    for (const Block& b : blocks) if (b.kind == Kind::Source) v.push_back(b.id);
    return v;
}

std::vector<int> Workspace::usersOf(int sourceId) const {
    std::vector<int> v;
    if (!sourceId) return v;
    for (const Block& b : blocks) if (b.source == sourceId) v.push_back(b.id);
    return v;
}

bool Workspace::sourceMissing(int id) const {
    const Block* b = find(id);
    if (!b || !info(b->kind).needsSource) return false;
    const Block* s = find(b->source);
    return !s || s->kind != Kind::Source;
}

void Workspace::fixLinks() {
    int maxId = 0;
    std::set<int> rxSeen;
    for (Block& b : blocks) {
        maxId = std::max(maxId, b.id);
        if (!info(b.kind).needsSource) b.source = 0;
        else if (b.source) { const Block* s = find(b.source); if (!s || s->kind != Kind::Source) b.source = 0; }
        if (b.kind == Kind::Source) {   // one block per receiver: a second block on the same receiver gets a new one when it is drawn
            const int rx = b.getI("rx");
            if (rx > 0 && !rxSeen.insert(rx).second) b.cfg.erase("rx");
        }
        if (b.r.w < 60) b.r.w = info(b.kind).w;
        if (b.r.h < 40) b.r.h = info(b.kind).h;
    }
    nextId = std::max(nextId, maxId + 1);
    std::map<int, std::vector<int>> groups;
    for (const Block& b : blocks) if (b.tab) groups[b.tab].push_back(b.id);
    for (auto& g : groups) {
        if (g.second.size() == 1) { find(g.second[0])->tab = 0; activeTab.erase(g.first); continue; }
        const auto it = activeTab.find(g.first);
        if (it == activeTab.end() || std::find(g.second.begin(), g.second.end(), it->second) == g.second.end()) activeTab[g.first] = g.second[0];
    }
    for (auto it = activeTab.begin(); it != activeTab.end();) it = groups.count(it->first) && groups[it->first].size() > 1 ? std::next(it) : activeTab.erase(it);
}

bool Workspace::joinTab(int id, int target) {
    if (id == target) return false;
    Block* b = find(id);
    Block* t = find(target);
    if (!b || !t || info(b->kind).hosted || info(t->kind).hosted) return false;   // a tool window draws its own frame: it cannot be a tab
    if (t->tab && b->tab == t->tab) return false;
    if (b->tab) leaveTab(id);
    t = find(target);
    if (!t->tab) { t->tab = t->id; activeTab[t->tab] = t->id; }
    // the group's rectangle: the shown member's
    Rect gr = t->r;
    if (const Block* sh = find(activeTab[t->tab])) gr = sh->r;
    b->tab = t->tab;
    b->r = gr;
    b->collapsed = false;
    activeTab[t->tab] = id;
    return true;
}

bool Workspace::leaveTab(int id) {
    Block* b = find(id);
    if (!b || !b->tab) return false;
    const int g = b->tab;
    b->tab = 0;
    b->r.x += 24; b->r.y += 24;
    const std::vector<int> m = tabMembers(g);
    if (m.size() <= 1) { for (int o : m) find(o)->tab = 0; activeTab.erase(g); }
    else if (activeTab[g] == id) activeTab[g] = m[0];
    return true;
}

std::vector<int> Workspace::tabMembers(int group) const {
    std::vector<int> v;
    if (!group) return v;
    for (const Block& b : blocks) if (b.tab == group) v.push_back(b.id);
    return v;
}

bool Workspace::showTab(int id) {
    Block* b = find(id);
    if (!b || !b->tab) return false;
    activeTab[b->tab] = id;
    return true;
}

bool Workspace::shown(int id) const {
    const Block* b = find(id);
    if (!b) return false;
    if (!b->tab) return true;
    const auto it = activeTab.find(b->tab);
    return it != activeTab.end() && it->second == id;
}

bool Workspace::renders(int id) const {
    const Block* b = find(id);
    return b && shown(id) && !b->collapsed;
}

int Workspace::decoderOwner(int sourceId) const {
    if (!sourceId) return 0;
    for (const Block& b : blocks) if (b.kind == Kind::Decoder && b.source == sourceId && !b.paused) return b.id;
    return 0;
}

int Workspace::receiverOf(int sourceId) const {
    const Block* s = find(sourceId);
    return s && s->kind == Kind::Source ? std::max(0, s->getI("rx")) : 0;
}

Rect snapRect(const Workspace& w, int id, Rect r, bool resizing, float threshold) {
    const Block* me = w.find(id);
    const int group = me ? me->tab : 0;
    float bestX = threshold + 1, bestY = threshold + 1, dx = 0, dy = 0;
    auto tryX = [&](float mine, float theirs) { const float d = theirs - mine; if (std::fabs(d) < std::fabs(bestX) && std::fabs(d) <= threshold) { bestX = d; dx = d; } };
    auto tryY = [&](float mine, float theirs) { const float d = theirs - mine; if (std::fabs(d) < std::fabs(bestY) && std::fabs(d) <= threshold) { bestY = d; dy = d; } };
    for (const Block& o : w.blocks) {
        if (o.id == id || (group && o.tab == group) || !w.shown(o.id)) continue;
        const Rect& q = o.r;
        const bool overlapY = r.y < q.y + q.h + threshold && q.y < r.y + r.h + threshold;
        const bool overlapX = r.x < q.x + q.w + threshold && q.x < r.x + r.w + threshold;
        if (overlapY) {
            if (!resizing) { tryX(r.x, q.x + q.w); tryX(r.x, q.x); }
            tryX(r.x + r.w, q.x); tryX(r.x + r.w, q.x + q.w);
        }
        if (overlapX) {
            if (!resizing) { tryY(r.y, q.y + q.h); tryY(r.y, q.y); }
            tryY(r.y + r.h, q.y); tryY(r.y + r.h, q.y + q.h);
        }
    }
    const bool snapX = bestX <= threshold, snapY = bestY <= threshold;
    auto grid = [&](float v) { return w.grid > 0 ? std::round(v / w.grid) * w.grid : v; };
    if (resizing) {
        if (snapX) r.w += dx; else if (w.grid > 0) r.w = grid(r.x + r.w) - r.x;
        if (snapY) r.h += dy; else if (w.grid > 0) r.h = grid(r.y + r.h) - r.y;
        r.w = std::max(r.w, 60.f); r.h = std::max(r.h, 40.f);
    } else {
        if (snapX) r.x += dx; else r.x = grid(r.x);
        if (snapY) r.y += dy; else r.y = grid(r.y);
        r.x = std::max(0.f, r.x); r.y = std::max(0.f, r.y);
    }
    return r;
}

int pickReceiver(const Workspace& w, const std::vector<int>& openRx, int maxRx, bool& create) {
    create = false;
    std::set<int> used;
    for (int s : w.sources()) if (const int rx = w.receiverOf(s)) used.insert(rx);
    std::vector<int> open = openRx;
    std::sort(open.begin(), open.end());
    for (int rx : open) if (!used.count(rx)) return rx;
    for (int rx = 1; rx <= maxRx; rx++) if (!used.count(rx) && std::find(open.begin(), open.end(), rx) == open.end()) { create = true; return rx; }
    return 0;
}

ModeStep modeStep(int lastOwner, int owner, const std::string& ownerMode, const std::string& rxMode) {
    ModeStep s;
    if (owner) {
        if (owner != lastOwner && !ownerMode.empty() && ownerMode != rxMode) { s.kind = ModeStep::Select; s.mode = ownerMode; }
    } else if (lastOwner > 0 && rxMode != kIdleMode) {
        s.kind = ModeStep::Idle; s.mode = kIdleMode;
    }
    return s;
}

std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == ' ') o += "\\s";
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c == '\r') o += "\\r";
        else o += c;
    }
    if (o.empty()) o = "\\e";   // an empty field is still one token
    return o;
}

std::string unescape(const std::string& s) {
    if (s == "\\e") return "";
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        const char n = s[++i];
        o += n == 's' ? ' ' : n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n;
    }
    return o;
}

std::string serialise(const Workspace& w) {
    std::string o = "onair-workspace 1\n";
    o += "name " + escape(w.name) + "\n";
    o += "grid " + fmtNum(w.grid) + "\n";
    o += std::string("snap ") + (w.snap ? "1" : "0") + "\n";
    o += std::string("links ") + (w.links ? "1" : "0") + "\n";
    o += "next " + std::to_string(w.nextId) + "\n";
    for (const Block& b : w.blocks) {
        o += "block " + std::to_string(b.id) + " " + info(b.kind).key + " " + std::to_string(b.source) + " " + fmtNum(std::round(b.r.x)) + " " +
             fmtNum(std::round(b.r.y)) + " " + fmtNum(std::round(b.r.w)) + " " + fmtNum(std::round(b.r.h)) + " " + (b.collapsed ? "1" : "0") + " " +
             (b.paused ? "1" : "0") + " " + std::to_string(b.tab) + "\n";
        for (const auto& kv : b.cfg) o += "cfg " + std::to_string(b.id) + " " + escape(kv.first) + " " + escape(kv.second) + "\n";
    }
    for (const auto& t : w.activeTab) o += "active " + std::to_string(t.first) + " " + std::to_string(t.second) + "\n";
    o += "end\n";
    return o;
}

namespace {
// parses lines [from, to) of one workspace; the header line already checked
void parseBody(const std::vector<std::string>& lines, size_t from, size_t to, Workspace& w) {
    for (size_t i = from; i < to; i++) {
        const std::vector<std::string> t = tokens(lines[i]);
        if (t.empty()) continue;
        const std::string& k = t[0];
        if (k == "name" && t.size() >= 2) w.name = unescape(t[1]);
        else if (k == "grid" && t.size() >= 2) w.grid = std::max(0.f, (float)parseNum(t[1], 16));
        else if (k == "snap" && t.size() >= 2) w.snap = t[1] != "0";
        else if (k == "links" && t.size() >= 2) w.links = t[1] != "0";
        else if (k == "next" && t.size() >= 2) w.nextId = std::max(1, std::atoi(t[1].c_str()));
        else if (k == "block" && t.size() >= 11) {
            const KindInfo* ki = infoByKey(t[2]);
            if (!ki) continue;   // a block of a newer version: left out
            Block b;
            b.id = std::atoi(t[1].c_str());
            if (b.id <= 0 || w.find(b.id)) continue;
            b.kind = ki->kind;
            b.source = std::atoi(t[3].c_str());
            b.r = Rect{(float)parseNum(t[4], 0), (float)parseNum(t[5], 0), (float)parseNum(t[6], ki->w), (float)parseNum(t[7], ki->h)};
            b.collapsed = t[8] == "1";
            b.paused = t[9] == "1";
            b.tab = std::max(0, std::atoi(t[10].c_str()));
            w.blocks.push_back(b);
        } else if (k == "cfg" && t.size() >= 4) {
            if (Block* b = w.find(std::atoi(t[1].c_str()))) b->cfg[unescape(t[2])] = unescape(t[3]);
        } else if (k == "active" && t.size() >= 3) {
            w.activeTab[std::atoi(t[1].c_str())] = std::atoi(t[2].c_str());
        }
    }
    w.fixLinks();
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { if (!cur.empty() && cur.back() == '\r') cur.pop_back(); v.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) { if (cur.back() == '\r') cur.pop_back(); v.push_back(cur); }
    return v;
}

bool isHeader(const std::string& l) {
    const std::vector<std::string> t = tokens(l);
    return t.size() >= 2 && t[0] == "onair-workspace" && std::atoi(t[1].c_str()) >= 1;
}
}  // namespace

std::vector<Workspace> parseMany(const std::string& text) {
    std::vector<Workspace> out;
    const std::vector<std::string> lines = splitLines(text);
    for (size_t i = 0; i < lines.size(); i++) {
        if (!isHeader(lines[i])) continue;
        size_t e = i + 1;
        while (e < lines.size() && tokens(lines[e]) != std::vector<std::string>{"end"} && !isHeader(lines[e])) e++;
        Workspace w;
        w.blocks.clear();
        parseBody(lines, i + 1, e, w);
        out.push_back(w);
        i = e < lines.size() && isHeader(lines[e]) ? e - 1 : e;
    }
    return out;
}

bool parse(const std::string& text, Workspace& out) {
    std::vector<Workspace> v = parseMany(text);
    if (v.empty()) { out = Workspace(); return false; }
    out = v[0];
    return true;
}

std::string writeMany(const std::vector<Workspace>& v) {
    std::string o;
    for (const Workspace& w : v) o += serialise(w);
    return o;
}

std::string uniqueName(const std::vector<Workspace>& v, const std::string& base0) {
    const std::string base = base0.empty() ? "Workspace" : base0;
    auto used = [&](const std::string& n) { for (const Workspace& w : v) if (w.name == n) return true; return false; };
    if (!used(base)) return base;
    for (int n = 2; n < 10000; n++) { const std::string c = base + " (" + std::to_string(n) + ")"; if (!used(c)) return c; }
    return base + " (copy)";
}

const char* const kTemplateNames[kNumTemplates] = {"Blank", "Listener", "Aircraft", "Satellite station"};

Workspace makeTemplate(int i) {
    Workspace w;
    w.name = kTemplateNames[std::max(0, std::min(kNumTemplates - 1, i))];
    auto put = [&w](Kind k, float x, float y, float ww, float hh) { const int id = w.add(k, x, y); Block* b = w.find(id); b->r.w = ww; b->r.h = hh; return id; };
    const float g = 16;
    switch (i) {
    case 1: {   // one source + spectrum + a demodulator + audio
        const int s = put(Kind::Source, g, g, 352, 336);
        put(Kind::Spectrum, 384, g, 704, 400);
        put(Kind::Demod, g, 368, 352, 224);
        put(Kind::Audio, 384, 432, 352, 240);
        (void)s;
        break;
    }
    case 2: {   // source + ADS-B + map
        put(Kind::Source, g, g, 352, 336);
        const int d = put(Kind::Decoder, 384, g, 640, 448);
        w.find(d)->set("mode", "11");
        put(Kind::Map, 384, 480, 640, 400);
        put(Kind::Spectrum, g, 368, 352, 272);
        break;
    }
    case 3: {   // source + tracker + rotator + decoder
        put(Kind::Source, g, g, 352, 336);
        const int d = put(Kind::Decoder, 384, g, 640, 432);
        w.find(d)->set("mode", "260");   // NOAA APT; pick any other mode in the block
        put(Kind::Tracker, 384, 464, 640, 432);
        put(Kind::Rotator, 1040, g, 368, 432);
        put(Kind::Spectrum, g, 368, 352, 272);
        break;
    }
    default: break;
    }
    return w;
}

}  // namespace canvas
