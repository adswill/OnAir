// The Workspace layout's model (app/canvas/canvas_model.h): add / remove / link blocks, a block whose source was removed, tab groups,
// snapping, receiver choice, the decoder's mode steps, the text form round trip (also with odd text and unknown kinds), several
// workspaces in one file, unique names and the shipped templates.
#include "canvas/canvas_model.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace canvas;

int main() {
    int fails = 0;
    auto chk = [&](bool ok, const char* m) { if (!ok) { std::printf("FAIL: %s\n", m); fails++; } };

    // kinds: every kind has a unique key and a default size
    for (int i = 0; i < (int)Kind::Count; i++) {
        const KindInfo& k = info((Kind)i);
        chk(k.kind == (Kind)i, "kind table in order");
        chk(infoByKey(k.key) == &k, "key finds its kind");
        chk(k.w >= 100 && k.h >= 60, "default size");
        chk(k.group >= 0 && k.group < kNumGroups, "group in range");
    }
    chk(infoByKey("nope") == nullptr, "unknown key");

    // add, link, remove
    Workspace w;
    chk(w.blocks.empty() && w.sources().empty(), "blank");
    const int spec0 = w.add(Kind::Spectrum, 10, 10);
    chk(w.find(spec0)->source == 0 && w.sourceMissing(spec0), "a spectrum before any source has none");
    const int s1 = w.add(Kind::Source, 0, 0);
    chk(w.find(s1)->source == 0 && !w.sourceMissing(s1), "a source needs no source");
    const int spec = w.add(Kind::Spectrum, 100, 0);
    chk(w.find(spec)->source == s1, "a new spectrum takes the source");
    const int s2 = w.add(Kind::Source, 0, 400);
    const int dec = w.add(Kind::Decoder, 300, 0);
    chk(w.find(dec)->source == s2, "the last source added");
    chk(w.link(dec, s1) && w.find(dec)->source == s1, "relink");
    chk(!w.link(dec, spec), "a spectrum is not a source");
    chk(!w.link(dec, 999), "no such source");
    chk(!w.link(s1, s2), "a source takes no source");
    chk(!w.link(w.add(Kind::Clock, 0, 0), s1), "a clock takes no source");
    chk(w.link(spec0, s2) && w.usersOf(s2).size() == 1, "users of a source");
    chk(w.usersOf(s1).size() == 2, "two users");
    chk(w.sources().size() == 2 && w.sources()[0] == s1, "sources in order");
    const int ids = w.nextId;

    // a block whose source was removed
    std::vector<int> unl;
    chk(w.remove(s1, &unl), "remove a source");
    chk(unl.size() == 2, "its two users are reported");
    chk(w.find(spec)->source == 0 && w.find(dec)->source == 0, "they are unlinked");
    chk(w.sourceMissing(spec) && w.sourceMissing(dec), "and say so");
    chk(w.find(spec0)->source == s2, "a block on another source keeps it");
    chk(!w.remove(s1), "removed twice");
    chk(w.add(Kind::Notes, 0, 0) == ids, "ids are never reused");

    // decoder owner and mode steps
    chk(w.decoderOwner(s2) == 0, "no decoder on s2");
    w.link(dec, s2);
    chk(w.decoderOwner(s2) == dec, "decoder owns s2");
    w.find(dec)->paused = true;
    chk(w.decoderOwner(s2) == 0, "a paused decoder owns nothing");
    w.find(dec)->paused = false;
    {
        ModeStep m = modeStep(-1, dec, "11", "68");
        chk(m.kind == ModeStep::Select && m.mode == "11", "after a load: the decoder's mode is selected");
        chk(modeStep(-1, dec, "11", "11").kind == ModeStep::None, "already in that mode");
        chk(modeStep(dec, dec, "11", "68").kind == ModeStep::None, "same owner: the receiver's own change is followed, not undone");
        chk(modeStep(-1, 0, "", "68").kind == ModeStep::None, "after a load with no decoder: the mode is left alone");
        m = modeStep(dec, 0, "", "11");
        chk(m.kind == ModeStep::Idle && m.mode == kIdleMode, "decoder paused or removed: the idle mode");
        chk(modeStep(dec, 0, "", kIdleMode).kind == ModeStep::None, "already idle");
        chk(modeStep(0, 0, "", "11").kind == ModeStep::None, "nothing changed");
        chk(modeStep(5, 7, "260", "11").kind == ModeStep::Select, "a new owner selects its mode");
        chk(modeStep(5, 7, "", "11").kind == ModeStep::None, "an owner without a mode yet takes the receiver's");
    }

    // receivers
    {
        Workspace r;
        bool create = true;
        chk(pickReceiver(r, {1}, 4, create) == 1 && !create, "first source: RX 1");
        const int a = r.add(Kind::Source, 0, 0); r.find(a)->setI("rx", 1);
        chk(pickReceiver(r, {1}, 4, create) == 2 && create, "second: a new RX 2");
        chk(pickReceiver(r, {1, 3}, 4, create) == 3 && !create, "an open receiver no block drives comes first");
        for (int i = 2; i <= 4; i++) { const int b = r.add(Kind::Source, 0, 0); r.find(b)->setI("rx", i); }
        chk(pickReceiver(r, {1, 2, 3, 4}, 4, create) == 0, "all taken");
        chk(r.receiverOf(a) == 1 && r.receiverOf(999) == 0, "receiverOf");
        const int dup = r.add(Kind::Source, 0, 0); r.find(dup)->setI("rx", 2);
        r.fixLinks();
        chk(r.receiverOf(dup) == 0, "a second block on the same receiver is cleared");
    }

    // tab groups
    {
        Workspace t;
        const int a = t.add(Kind::Log, 0, 0), b = t.add(Kind::Notes, 500, 0), c = t.add(Kind::Clock, 0, 300), m = t.add(Kind::Map, 0, 0);
        chk(t.shown(a) && t.renders(a), "own window: shown");
        chk(!t.joinTab(a, a), "onto itself");
        chk(!t.joinTab(m, a) && !t.joinTab(a, m), "a tool window cannot be a tab");
        chk(t.joinTab(b, a), "join");
        chk(t.find(a)->tab == a && t.find(b)->tab == a, "the group is named after the target");
        chk(t.find(b)->r == t.find(a)->r, "it takes the group's rectangle");
        chk(t.shown(b) && !t.shown(a), "the dropped block is shown");
        chk(!t.renders(a), "a hidden tab is not drawn");
        chk(!t.joinTab(b, a), "already in that group");
        chk(t.joinTab(c, b), "join through a member");
        chk(t.tabMembers(a).size() == 3, "three tabs");
        chk(t.showTab(a) && t.shown(a) && !t.shown(c), "show a tab");
        t.find(a)->collapsed = true;
        chk(t.shown(a) && !t.renders(a), "collapsed: not drawn");
        t.find(a)->collapsed = false;
        chk(t.leaveTab(a) && t.find(a)->tab == 0 && t.shown(a), "leave");
        chk(t.activeTab[t.find(b)->tab] != a, "the group shows another member");
        chk(t.leaveTab(b) && t.find(c)->tab == 0 && t.activeTab.empty(), "the last two: the group dissolves");
        chk(!t.leaveTab(c), "not in a group");
        t.joinTab(b, a); t.joinTab(c, a);
        chk(t.remove(c) && t.tabMembers(a).size() == 2 && t.shown(b) == !t.shown(a), "remove a shown tab: another is shown");
        t.remove(b);
        chk(t.find(a)->tab == 0 && t.activeTab.empty(), "remove down to one: dissolved");
    }

    // snapping
    {
        Workspace s;
        s.grid = 16;
        const int a = s.add(Kind::Notes, 0, 0);
        s.find(a)->r = Rect{0, 0, 300, 200};
        const int b = s.add(Kind::Notes, 0, 0);
        Rect r = snapRect(s, b, Rect{306, 5, 300, 200}, false, 10);
        chk(r.x == 300 && r.y == 0, "left edge to the right edge, top to top");
        r = snapRect(s, b, Rect{333, 37, 300, 200}, false, 10);
        chk(r.x == 336 && r.y == 32, "too far: the grid");
        s.grid = 0;
        r = snapRect(s, b, Rect{333.5f, 37, 300, 200}, false, 10);
        chk(r.x == 333.5f && r.y == 37, "no grid, too far: unchanged");
        r = snapRect(s, b, Rect{0, 205, 296, 100}, true, 10);
        chk(r.x == 0 && r.y == 205 && r.w == 300, "resize: the right edge snaps to the right edge above");
        r = snapRect(s, b, Rect{-20, -9, 50, 20}, true, 10);
        chk(r.w >= 60 && r.h >= 40, "a least size");
        r = snapRect(s, b, Rect{-30, -30, 100, 100}, false, 5);
        chk(r.x >= 0 && r.y >= 0, "kept on the canvas");
        r = snapRect(s, b, Rect{306, 900, 300, 200}, false, 10);
        chk(r.x == 306, "no snap to a block far away on the other axis");
    }

    // the text form: round trip
    {
        Workspace w2 = makeTemplate(3);
        w2.name = "My station\twith\\odd \"text\"\n2nd line";
        w2.grid = 8; w2.snap = false; w2.links = false;
        const int n = w2.add(Kind::Notes, 12.4f, 7.6f);
        w2.find(n)->set("text", "line 1\nline 2 with spaces\\ and = signs");
        w2.find(n)->set("empty", "");
        w2.find(n)->setD("level", -3.25);
        const int l = w2.add(Kind::Log, 0, 0);
        w2.joinTab(l, n);
        w2.find(l)->collapsed = true;
        const int dec = w2.find(w2.sources()[0]) ? w2.usersOf(w2.sources()[0])[0] : 0;
        for (Block& b : w2.blocks) if (b.kind == Kind::Decoder) b.paused = true;
        (void)dec;
        const std::string text = serialise(w2);
        Workspace back;
        chk(parse(text, back), "parses");
        chk(back.name == w2.name, "name round trip");
        chk(back.grid == 8 && !back.snap && !back.links, "options");
        chk(back.blocks.size() == w2.blocks.size(), "block count");
        bool same = true;
        for (const Block& b : w2.blocks) {
            const Block* o = back.find(b.id);
            if (!o || o->kind != b.kind || o->source != b.source || o->tab != b.tab || o->collapsed != b.collapsed || o->paused != b.paused || o->cfg != b.cfg ||
                std::fabs(o->r.x - std::round(b.r.x)) > 0 || std::fabs(o->r.w - std::round(b.r.w)) > 0) same = false;
        }
        chk(same, "every block round trips");
        chk(back.find(n)->get("text") == "line 1\nline 2 with spaces\\ and = signs", "text with new lines and backslashes");
        chk(back.find(n)->cfg.count("empty") && back.find(n)->get("empty", "x").empty(), "an empty value");
        chk(back.find(n)->getD("level") == -3.25, "a number");
        chk(back.activeTab == w2.activeTab, "the shown tabs");
        chk(back.nextId == w2.nextId, "next id");
        chk(serialise(back) == text, "a second round trip is identical");
    }

    // tolerant parsing
    {
        Workspace o;
        chk(!parse("", o) && !parse("hello\nworld\n", o), "not a workspace");
        const std::string t = "onair-workspace 1\nname X\nblock 3 source 0 0 0 300 300 0 0 0\nblock 4 hologram 3 0 0 300 300 0 0 0\n"
                              "block 5 spectrum 9 10 10 300 200 0 0 0\nblock 6 decoder 3 0 0 300 300 0 0 0\nblock 6 log 0 0 0 10 10 0 0 0\n"
                              "cfg 77 a b\nwhat is this\nblock 7 log 0 0 0 10 10 0 0 7\nactive 7 7\nend\n";
        chk(parse(t, o), "parses");
        chk(o.blocks.size() == 4, "the unknown kind and the duplicate id are left out");
        chk(o.find(5) && o.find(5)->source == 0, "a link to a source that is not there is dropped");
        chk(o.find(6)->source == 3, "a good link stays");
        chk(o.find(7)->tab == 0 && o.activeTab.empty(), "a one-member tab group is dissolved");
        chk(o.find(7)->r.w >= 60 && o.find(7)->r.h >= 40, "a tiny size gets the default");
        chk(o.nextId > 7, "next id past every id");
        const std::string crlf = "onair-workspace 1\r\nname Win\r\nblock 1 clock 0 1 2 300 200 0 0 0\r\nend\r\n";
        chk(parse(crlf, o) && o.name == "Win" && o.blocks.size() == 1, "CR LF line ends");
        const std::string comma = "onair-workspace 1\ngrid 12,5\nblock 1 notes 0 1,5 2 300 200 0 0 0\ncfg 1 v 0,25\nend\n";
        chk(parse(comma, o) && std::fabs(o.grid - 12.5f) < 1e-4f && o.find(1)->getD("v") == 0.25, "decimal commas are read");
    }

    // several workspaces in one file, unique names
    {
        std::vector<Workspace> v;
        for (int i = 0; i < kNumTemplates; i++) v.push_back(makeTemplate(i));
        const std::vector<Workspace> back = parseMany(writeMany(v));
        chk(back.size() == (size_t)kNumTemplates, "four in one file");
        bool names = true;
        for (int i = 0; i < kNumTemplates && i < (int)back.size(); i++) names = names && back[i].name == kTemplateNames[i];
        chk(names, "their names");
        chk(parseMany("onair-workspace 1\nname A\nonair-workspace 1\nname B\nend\n").size() == 2, "a missing end");
        chk(uniqueName(v, "Listener") == "Listener (2)", "a taken name");
        chk(uniqueName(v, "New") == "New", "a free name");
        v.push_back(Workspace()); v.back().name = "Listener (2)";
        chk(uniqueName(v, "Listener") == "Listener (3)", "the next free number");
        chk(uniqueName(v, "") == "Workspace", "an empty name");
    }

    // templates
    {
        chk(makeTemplate(0).blocks.empty(), "Blank is blank");
        auto count = [](const Workspace& w, Kind k) { int n = 0; for (const Block& b : w.blocks) n += b.kind == k; return n; };
        const Workspace li = makeTemplate(1);
        chk(count(li, Kind::Source) == 1 && count(li, Kind::Spectrum) == 1 && count(li, Kind::Audio) == 1 && count(li, Kind::Demod) == 1, "Listener: source, spectrum, demodulator, audio");
        const Workspace ac = makeTemplate(2);
        chk(count(ac, Kind::Source) == 1 && count(ac, Kind::Decoder) == 1 && count(ac, Kind::Map) == 1, "Aircraft: source, decoder, map");
        bool adsb = false;
        for (const Block& b : ac.blocks) if (b.kind == Kind::Decoder) adsb = b.get("mode") == "11";
        chk(adsb, "the Aircraft decoder is ADS-B");
        const Workspace st = makeTemplate(3);
        chk(count(st, Kind::Source) == 1 && count(st, Kind::Tracker) == 1 && count(st, Kind::Rotator) == 1 && count(st, Kind::Decoder) == 1, "Satellite station: source, tracker, rotator, decoder");
        for (int i = 0; i < kNumTemplates; i++) {
            const Workspace t = makeTemplate(i);
            bool linked = true, overlap = false;
            for (const Block& b : t.blocks) {
                if (info(b.kind).needsSource && t.sourceMissing(b.id)) linked = false;
                for (const Block& o : t.blocks) if (o.id < b.id && b.r.x < o.r.x + o.r.w && o.r.x < b.r.x + b.r.w && b.r.y < o.r.y + o.r.h && o.r.y < b.r.y + b.r.h) overlap = true;
            }
            chk(linked, "every template block that needs a source has one");
            chk(!overlap, "no template blocks overlap");
            chk(t.name == kTemplateNames[i], "template name");
        }
    }

    // escaping
    chk(unescape(escape("")) == "" && escape("") == "\\e", "empty field");
    chk(unescape(escape("a b\\s")) == "a b\\s", "a literal backslash-s");
    chk(escape("a b").find(' ') == std::string::npos, "no space in a token");

    if (fails) { std::printf("%d failures\n", fails); return 1; }
    std::printf("canvas model: all passed\n");
    return 0;
}
