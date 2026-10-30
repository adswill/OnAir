#include "dect2/tv/mheg_engine.h"
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <set>

namespace dect2 {
namespace {

int64_t berInt(const std::vector<uint8_t>& v) {
    if (v.empty()) return 0;
    uint64_t r = (v[0] & 0x80) ? ~0ull : 0;
    for (size_t i = 0; i < v.size() && i < 8; i++) r = (r << 8) | v[i];
    return (int64_t)r;
}
const MhegNode* kid(const MhegNode& n, uint32_t tag) {
    for (const auto& k : n.kids) if (k.cls == 2 && k.tag == tag) return &k;
    return nullptr;
}
std::string normId(std::string s) {
    if (s.rfind("DSM:", 0) == 0) s.erase(0, 4);
    size_t i = 0;
    while (i < s.size() && (s[i] == '~' || s[i] == '/')) i++;
    return s.substr(i);
}
uint32_t colourOf(const MhegNode* c, uint32_t dflt) {
    if (!c) return dflt;
    for (const auto& k : c->kids)
        if (k.cls == 0 && k.tag == 4 && k.value.size() >= 3) {
            uint32_t a = k.value.size() >= 4 ? 255u - k.value[3] : 255u;
            return ((uint32_t)k.value[0] << 24) | ((uint32_t)k.value[1] << 16) | ((uint32_t)k.value[2] << 8) | a;
        }
    return dflt;
}
bool pair2(const MhegNode* n, int& a, int& b) {
    if (!n || n->kids.size() < 2) return false;
    a = (int)berInt(n->kids[0].value);
    b = (int)berInt(n->kids[1].value);
    return true;
}

struct Val {
    enum K { None, Int, Bool, Str, Obj, Ref } k = None;
    int64_t i = 0;
    std::string s;       // Str, Ref; for Obj the group id
    int n = 0;           // Obj: object number
};
struct Object {
    int num = 0, tag = 0;
    const MhegNode* node = nullptr;
    bool active = false;
    int x = 0, y = 0, w = 0, h = 0;
    uint32_t fill = 0, line = 0, tcol = 0xFFFFFFFFu;
    std::string text, ref;
    Val v;
};
struct Group {
    uint64_t gid = 0;
    std::string id;
    bool isScene = false;
    MhegParse parse;
    std::map<int, Object> objs;
    std::vector<int> order, links;
    std::map<int, uint64_t> timers;
    int inputReg = 0;
};
struct Event { uint64_t gid = 0; int obj = 0, type = 0; int dk = 0; int64_t di = 0; std::string ds; };   // dk: 0 no data, 1 integer, 2 string, 3 boolean
struct Res { Group* g = nullptr; Object* o = nullptr; };
enum { evRunning = 4, evStopped = 5, evUserInput = 6, evTimer = 8 };

} // namespace

struct MhegEngine::Impl {
    Fetch fetch;
    MhegLimits lim;
    std::unique_ptr<Group> app, sc;
    std::deque<Event> q;
    std::vector<std::unique_ptr<Group>> dead;   // replaced groups live until the event that replaced them is finished
    std::vector<int> keys;
    uint64_t now = 0, nextGid = 1;
    bool run = false;
    int actionsLeft = 0, eventsLeft = 0, transLeft = 0;
    uint32_t rng = 12345;
    MhegStats st;
    std::vector<std::string> logv;
    std::set<int> warned;
    mutable std::map<std::string, std::vector<uint8_t>> files;
    mutable std::set<std::string> missingFiles;

    void note(const std::string& s) { if (logv.size() < 200) logv.push_back(s); }

    bool getFile(const std::string& path, std::vector<uint8_t>& out) const {
        auto it = files.find(path);
        if (it != files.end()) { out = it->second; return true; }
        if (!fetch) return false;
        std::vector<uint8_t> d;
        if (!fetch(path, d) || d.size() > lim.fileBytes) return false;
        if (files.size() < 256) files[path] = d;
        out = std::move(d);
        return true;
    }

    // ---- groups
    std::unique_ptr<Group> loadGroup(const std::string& path, bool scene) {
        std::vector<uint8_t> d;
        if (!getFile(path, d)) { st.fetchFailures++; note("missing file " + path); return nullptr; }
        auto g = std::make_unique<Group>();
        g->parse = mhegParse(d.data(), d.size());
        if (!g->parse.error.empty() || g->parse.roots.size() != 1) { note("damaged file " + path + ": " + g->parse.error); return nullptr; }
        const MhegNode& root = g->parse.roots[0];
        if (root.cls != 2 || root.tag != (scene ? 1u : 0u)) { note("wrong object type in " + path); return nullptr; }
        g->gid = nextGid++;
        g->id = normId(path);
        g->isScene = scene;
        Object go; go.num = 0; go.tag = (int)root.tag; go.node = &root; go.active = true;
        g->objs[0] = go;
        if (const MhegNode* r = kid(root, 51)) g->inputReg = (int)berInt(r->value);
        const MhegNode* items = kid(root, 8);
        if (items) for (const auto& it : items->kids) {
            if (it.cls != 2 || it.tag < 9 || it.tag > 34 || it.kids.empty()) continue;
            if (g->objs.size() >= lim.objects) { note("too many objects"); break; }
            const MhegNode& rc = it.kids[0];
            int num = -1;
            if (rc.cls == 0 && rc.tag == 16 && rc.kids.size() >= 2) num = (int)berInt(rc.kids[1].value);
            else if (rc.cls == 0 && rc.tag == 2) num = (int)berInt(rc.value);
            if (num <= 0 || g->objs.count(num)) continue;
            Object o; o.num = num; o.tag = (int)it.tag; o.node = &it;
            const MhegNode* ia = kid(it, 56);
            bool presentable = it.tag == 22 || it.tag == 25 || it.tag == 29 || it.tag == 21 || it.tag == 23 || it.tag == 24 || (it.tag >= 26 && it.tag <= 34);
            o.active = presentable ? !(ia && !ia->value.empty() && ia->value[0] == 0) : true;
            if (it.tag == 20) o.active = true;
            pair2(kid(it, 76), o.w, o.h);
            pair2(kid(it, 77), o.x, o.y);
            o.fill = colourOf(kid(it, 85), 0);
            o.line = colourOf(kid(it, 84), 0);
            o.tcol = colourOf(kid(it, 41), 0xFFFFFFFFu);
            if (const MhegNode* c = kid(it, 58))
                for (const auto& k : c->kids) {
                    if (k.cls == 0 && k.tag == 4) o.text.assign(k.value.begin(), k.value.end());
                    else if (k.cls == 0 && k.tag == 16 && !k.kids.empty()) o.ref.assign(k.kids[0].value.begin(), k.kids[0].value.end());
                }
            if (o.text.size() > lim.stringBytes) o.text.resize(lim.stringBytes);
            if (it.tag >= 15 && it.tag <= 19)
                if (const MhegNode* ov = kid(it, 67); ov && !ov->kids.empty()) {
                    const MhegNode& k = ov->kids[0];
                    if (k.cls == 0 && k.tag == 2) { o.v.k = Val::Int; o.v.i = berInt(k.value); }
                    else if (k.cls == 0 && k.tag == 1) { o.v.k = Val::Bool; o.v.i = !k.value.empty() && k.value[0]; }
                    else if (k.cls == 0 && k.tag == 4) { o.v.k = Val::Str; o.v.s.assign(k.value.begin(), k.value.end()); }
                }
            if (it.tag == 20) g->links.push_back(num);
            g->order.push_back(num);
            g->objs[num] = std::move(o);
        }
        return g;
    }
    Group* byGid(uint64_t gid) { if (app && app->gid == gid) return app.get(); if (sc && sc->gid == gid) return sc.get(); return nullptr; }
    Group* byId(const std::string& id) { std::string n = normId(id); if (app && app->id == n) return app.get(); if (sc && sc->id == n) return sc.get(); return nullptr; }

    void post(const Group& g, int obj, int type, int dk = 0, int64_t di = 0, const std::string& ds = std::string()) {
        if (q.size() >= lim.queue) { note("event queue full, event dropped"); return; }
        Event e; e.gid = g.gid; e.obj = obj; e.type = type; e.dk = dk; e.di = di; e.ds = ds;
        q.push_back(std::move(e));
    }
    void startGroup(Group& g) {
        post(g, 0, evRunning);
        for (int n : g.order) if (g.objs[n].active) post(g, n, evRunning);
        if (const MhegNode* a = kid(g.parse.roots[0], 5)) execList(*a, &g);
    }

    // ---- references and values
    Res resolve(const MhegNode& n, Group* ctx) {
        Res r;
        if (n.cls == 0 && n.tag == 2) { r.g = ctx; }
        else if (n.cls == 0 && n.tag == 16 && n.kids.size() >= 2) {
            r.g = byId(std::string(n.kids[0].value.begin(), n.kids[0].value.end()));
            if (r.g) { auto it = r.g->objs.find((int)berInt(n.kids[1].value)); r.o = it == r.g->objs.end() ? nullptr : &it->second; }
            return r;
        } else if (n.cls == 2 && n.tag == 236 && !n.kids.empty()) {
            Res v = resolve(n.kids[0], ctx);
            if (v.o && v.o->v.k == Val::Obj) {
                r.g = byId(v.o->v.s);
                if (r.g) { auto it = r.g->objs.find(v.o->v.n); r.o = it == r.g->objs.end() ? nullptr : &it->second; }
            }
            return r;
        }
        if (r.g) { auto it = r.g->objs.find((int)berInt(n.value)); r.o = it == r.g->objs.end() ? nullptr : &it->second; }
        return r;
    }
    bool getInt(const MhegNode& n, Group* ctx, int64_t& out) {
        if (n.cls == 0 && n.tag == 2) { out = berInt(n.value); return true; }
        if (n.cls == 2 && n.tag == 236) { Res r = resolve(n, ctx); if (r.o && r.o->v.k == Val::Int) { out = r.o->v.i; return true; } }
        return false;
    }
    bool getStr(const MhegNode& n, Group* ctx, std::string& out) {
        if (n.cls == 0 && n.tag == 4) { out.assign(n.value.begin(), n.value.end()); return true; }
        if (n.cls == 2 && n.tag == 236) { Res r = resolve(n, ctx); if (r.o && r.o->v.k == Val::Str) { out = r.o->v.s; return true; } }
        return false;
    }
    bool getBool(const MhegNode& n, Group* ctx, bool& out) {
        if (n.cls == 0 && n.tag == 1) { out = !n.value.empty() && n.value[0]; return true; }
        if (n.cls == 2 && n.tag == 236) { Res r = resolve(n, ctx); if (r.o && r.o->v.k == Val::Bool) { out = r.o->v.i != 0; return true; } }
        return false;
    }

    // ---- actions
    void activate(Group* g, Object* o, bool on) {
        if (!g || !o || o->num == 0 || o->active == on) return;
        o->active = on;
        post(*g, o->num, on ? evRunning : evStopped);
    }
    void reorder(Group* g, int num, bool front) {
        auto it = std::find(g->order.begin(), g->order.end(), num);
        if (it == g->order.end()) return;
        g->order.erase(it);
        if (front) g->order.push_back(num); else g->order.insert(g->order.begin(), num);
    }
    void colourAction(const MhegNode& a, Group* ctx, uint32_t Object::*field) {
        if (a.kids.size() < 2) return;
        Res t = resolve(a.kids[0], ctx);
        if (!t.o) return;
        const MhegNode& c = a.kids[1];
        if (c.cls == 2 && c.tag == 231 && !c.kids.empty()) {
            std::string s;
            if (getStr(c.kids[0], ctx, s) && s.size() >= 3) t.o->*field = ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) | ((uint32_t)(uint8_t)s[2] << 8) | (s.size() >= 4 ? 255u - (uint8_t)s[3] : 255u);
        }
    }
    void transition(const std::string& path) {
        if (++st.transitions, --transLeft < 0) { note("too many scene changes in one step, stopped"); run = false; return; }
        std::string p = normId(path);
        auto g = loadGroup(p, true);
        if (!g) return;
        if (sc) dead.push_back(std::move(sc));
        sc = std::move(g);
        startGroup(*sc);
    }
    void execList(const MhegNode& list, Group* ctx) {
        for (const auto& a : list.kids) {
            if (!run) return;
            if (--actionsLeft < 0) { note("too many actions in one step, application stopped"); run = false; return; }
            st.actions++;
            execOne(a, ctx);
        }
    }
    static std::string refName(const MhegNode& n) {
        if (n.cls == 0 && n.tag == 16 && !n.kids.empty()) return std::string(n.kids[0].value.begin(), n.kids[0].value.end());
        return std::string();
    }
    void execOne(const MhegNode& a, Group* ctx) {
        if (a.cls != 2) return;
        auto K = [&](size_t i) -> const MhegNode* { return i < a.kids.size() ? &a.kids[i] : nullptr; };
        switch (a.tag) {
        case 115: case 178: if (K(0)) { Res t = resolve(*K(0), ctx); activate(t.g, t.o, true); } break;     // Activate, Run
        case 125: case 216: if (K(0)) { Res t = resolve(*K(0), ctx); activate(t.g, t.o, false); } break;   // Deactivate, Stop
        case 212: {   // SetVariable
            if (!K(1) || !K(0)) break;
            Res t = resolve(*K(0), ctx);
            if (!t.o) break;
            const MhegNode& nv = *K(1);
            if (nv.kids.empty()) break;
            int64_t i; bool b; std::string s;
            if (nv.tag == 226 && getInt(nv.kids[0], ctx, i)) { t.o->v.k = Val::Int; t.o->v.i = i; }
            else if (nv.tag == 225 && getBool(nv.kids[0], ctx, b)) { t.o->v.k = Val::Bool; t.o->v.i = b; }
            else if (nv.tag == 227 && getStr(nv.kids[0], ctx, s)) { if (s.size() > lim.stringBytes) s.resize(lim.stringBytes); t.o->v.k = Val::Str; t.o->v.s = s; }
            else if (nv.tag == 228) { Res r = resolve(nv.kids[0], ctx); if (r.g && r.o) { t.o->v.k = Val::Obj; t.o->v.s = r.g->id; t.o->v.n = r.o->num; } }
            break;
        }
        case 116: case 218: case 171: case 129: case 168: {   // Add, Subtract, Multiply, Divide, Modulo
            if (!K(0) || !K(1)) break;
            Res t = resolve(*K(0), ctx);
            int64_t v;
            if (!t.o || t.o->v.k != Val::Int || !getInt(*K(1), ctx, v)) break;
            int64_t& x = t.o->v.i;
            if (a.tag == 116) x += v; else if (a.tag == 218) x -= v; else if (a.tag == 171) x *= v;
            else if (v == 0) note("division by zero ignored"); else if (a.tag == 129) x /= v; else x %= v;
            break;
        }
        case 118: {   // Append
            if (!K(0) || !K(1)) break;
            Res t = resolve(*K(0), ctx); std::string s;
            if (t.o && t.o->v.k == Val::Str && getStr(*K(1), ctx, s) && t.o->v.s.size() + s.size() <= lim.stringBytes) t.o->v.s += s;
            break;
        }
        case 210: {   // SetTimer on a group: id, optional {ms}
            if (!K(0) || !K(1)) break;
            Res t = resolve(*K(0), ctx); int64_t id, ms;
            if (!t.g || !getInt(*K(1), ctx, id)) break;
            if (K(2) && !K(2)->kids.empty() && getInt(K(2)->kids[0], ctx, ms)) { if (t.g->timers.size() < 64) t.g->timers[(int)id] = now + (uint64_t)std::max<int64_t>(0, ms); }
            else t.g->timers.erase((int)id);
            break;
        }
        case 193: {   // SetData
            if (!K(0) || !K(1)) break;
            Res t = resolve(*K(0), ctx); std::string s;
            if (!t.o) break;
            const MhegNode& nc = *K(1);
            if (getStr(nc, ctx, s)) { if (s.size() > lim.stringBytes) s.resize(lim.stringBytes); t.o->text = s; t.o->ref.clear(); }
            else if (nc.cls == 0 && nc.tag == 16) { t.o->ref = refName(nc); t.o->text.clear(); }
            break;
        }
        case 207: case 186: {   // SetPosition, SetBoxSize
            if (!K(0) || !K(1) || !K(2)) break;
            Res t = resolve(*K(0), ctx); int64_t x, y;
            if (t.o && getInt(*K(1), ctx, x) && getInt(*K(2), ctx, y)) { if (a.tag == 207) { t.o->x = (int)x; t.o->y = (int)y; } else { t.o->w = (int)std::max<int64_t>(0, x); t.o->h = (int)std::max<int64_t>(0, y); } }
            break;
        }
        case 195: colourAction(a, ctx, &Object::fill); break;
        case 201: colourAction(a, ctx, &Object::line); break;
        case 240: colourAction(a, ctx, &Object::tcol); break;
        case 119: case 185: if (K(0)) { Res t = resolve(*K(0), ctx); if (t.g && t.o) reorder(t.g, t.o->num, a.tag == 119); } break;
        case 184: {   // SendEvent
            if (!K(1) || !K(2)) break;
            Res src = resolve(*K(1), ctx);
            if (!src.g || !src.o) break;
            Event e; e.gid = src.g->gid; e.obj = src.o->num; e.type = (int)berInt(K(2)->value);
            if (K(3)) { int64_t i; bool b; std::string s; if (getInt(*K(3), ctx, i)) { e.dk = 1; e.di = i; } else if (getBool(*K(3), ctx, b)) { e.dk = 3; e.di = b; } else if (getStr(*K(3), ctx, s)) { e.dk = 2; e.ds = s; } }
            if (q.size() < lim.queue) q.push_back(std::move(e));
            break;
        }
        case 222: case 214: if (K(0)) transition(refName(*K(0))); break;   // TransitionTo, Spawn (a Scene change; Application spawning is not supported)
        case 166: if (K(0)) {   // Launch another Application
            std::string p = refName(*K(0));
            auto g = loadGroup(normId(p), false);
            if (g) { if (sc) dead.push_back(std::move(sc)); if (app) dead.push_back(std::move(app)); q.clear(); app = std::move(g); startGroup(*app); }
            break;
        }
        case 176: run = false; break;   // Quit
        case 120: {   // Call a resident program
            if (!K(0)) break;
            Res t = resolve(*K(0), ctx);
            if (!t.o || t.o->tag != 9) break;
            const MhegNode* nm = kid(*t.o->node, 64);
            std::string name = nm ? std::string(nm->value.begin(), nm->value.end()) : std::string();
            const MhegNode* params = nullptr;
            for (size_t i = 2; i < a.kids.size(); i++) if (a.kids[i].cls == 0 && a.kids[i].tag == 16) params = &a.kids[i];
            bool ok = false;
            if (name == "Rnd" && params && params->kids.size() >= 2) {
                int64_t mx;
                Res out = resolve(params->kids[1].kids.empty() ? params->kids[1] : params->kids[1].kids[0], ctx);
                if (!params->kids[0].kids.empty() && getInt(params->kids[0].kids[0], ctx, mx) && out.o && mx > 0) {
                    rng = rng * 1664525u + 1013904223u;
                    out.o->v.k = Val::Int; out.o->v.i = 1 + (int64_t)(rng >> 8) % mx; ok = true;
                }
            }
            if (!ok && !warned.count(1000 + (int)name.size())) { warned.insert(1000 + (int)name.size()); note("resident program not supported: " + name); st.unsupported++; }
            if (K(1)) { Res sres = resolve(*K(1), ctx); if (sres.o) { sres.o->v.k = Val::Bool; sres.o->v.i = ok; } }
            break;
        }
        case 142: {   // GetEngineSupport: only the features this engine really has are answered "yes"
            if (!K(0) || !K(1)) break;
            std::string f; Res out = resolve(*K(1), ctx);
            if (!getStr(*K(0), ctx, f) || !out.o) break;
            static const char* yes[] = {"UKEngineProfile", "DSMCC", "MHEGProfile"};
            bool s = false;
            for (const char* y : yes) if (f.find(y) != std::string::npos) s = true;
            out.o->v.k = Val::Bool; out.o->v.i = s;
            break;
        }
        default:
            st.unsupported++;
            if (!warned.count((int)a.tag)) { warned.insert((int)a.tag); char b[64]; snprintf(b, sizeof b, "action %u not supported", a.tag); note(b); }
        }
    }

    // ---- events
    void dispatch(const Event& e) {
        Group* src = byGid(e.gid);
        if (!src) return;
        for (Group* g : {app.get(), sc.get()}) {
            if (!g) continue;
            uint64_t gid = g->gid;
            std::vector<int> links = g->links;
            for (int ln : links) {
                if (!run || !byGid(gid)) return;
                auto it = g->objs.find(ln);
                if (it == g->objs.end() || !it->second.active) continue;
                const MhegNode* cond = kid(*it->second.node, 62);
                const MhegNode* eff = kid(*it->second.node, 63);
                if (!cond || !eff || cond->kids.size() < 2) continue;
                if ((int)berInt(cond->kids[1].value) != e.type) continue;
                Res s = resolve(cond->kids[0], g);
                if (s.g != src || !s.o || s.o->num != e.obj) continue;
                if (cond->kids.size() >= 3) {
                    const MhegNode& d = cond->kids[2];
                    bool match = (d.tag == 2 && e.dk == 1 && berInt(d.value) == e.di) || (d.tag == 1 && e.dk == 3 && (!d.value.empty() && d.value[0]) == (e.di != 0)) ||
                                 (d.tag == 4 && e.dk == 2 && std::string(d.value.begin(), d.value.end()) == e.ds);
                    if (!match) continue;
                }
                st.linksFired++;
                execList(*eff, g);
            }
        }
    }
    void step(uint64_t t) {
        now = t;
        actionsLeft = lim.actionsPerStep; eventsLeft = lim.eventsPerStep; transLeft = lim.transitionsPerStep;
        if (!run) return;
        for (Group* g : {app.get(), sc.get()}) {
            if (!g) continue;
            std::vector<int> due;
            for (auto& kv : g->timers) if (kv.second <= now) due.push_back(kv.first);
            std::sort(due.begin(), due.end());
            for (int id : due) { g->timers.erase(id); post(*g, 0, evTimer, 1, id); }
        }
        for (int k : keys) if (sc && sc->inputReg != 0) post(*sc, 0, evUserInput, 1, k);
        keys.clear();
        while (run && !q.empty()) {
            if (--eventsLeft < 0) { note("runaway: too many events in one step, application stopped"); run = false; break; }
            Event e = std::move(q.front());
            q.pop_front();
            st.events++;
            dispatch(e);
            dead.clear();
        }
        if (!run) q.clear();
    }
};

MhegEngine::MhegEngine(Fetch fetch, MhegLimits lim) : p_(new Impl) { p_->fetch = std::move(fetch); p_->lim = lim; }
MhegEngine::~MhegEngine() = default;

bool MhegEngine::start(const std::string& entry, uint64_t nowMs) {
    Impl& s = *p_;
    s.app.reset(); s.sc.reset(); s.q.clear(); s.keys.clear(); s.run = false; s.now = nowMs;
    auto g = s.loadGroup(normId(entry), false);
    if (!g) return false;
    s.app = std::move(g);
    s.run = true;
    s.actionsLeft = s.lim.actionsPerStep; s.eventsLeft = s.lim.eventsPerStep; s.transLeft = s.lim.transitionsPerStep;
    s.startGroup(*s.app);
    step(nowMs);
    return true;
}
void MhegEngine::key(int code) { if (p_->run && p_->keys.size() < 64) p_->keys.push_back(code); }
void MhegEngine::step(uint64_t nowMs) { p_->step(nowMs); }
bool MhegEngine::running() const { return p_->run; }
std::string MhegEngine::scene() const { return p_->sc ? p_->sc->id : std::string(); }
const MhegStats& MhegEngine::stats() const { return p_->st; }
const std::vector<std::string>& MhegEngine::log() const { return p_->logv; }

std::string MhegEngine::variableText(const std::string& group, int object) const {
    Impl& s = *p_;
    Group* g = s.byId(group);
    if (!g) return std::string();
    auto it = g->objs.find(object);
    if (it == g->objs.end()) return std::string();
    const Val& v = it->second.v;
    if (v.k == Val::Int) return std::to_string(v.i);
    if (v.k == Val::Bool) return v.i ? "true" : "false";
    if (v.k == Val::Str) return v.s;
    return std::string();
}

MhegRender MhegEngine::render() const {
    Impl& s = *p_;
    MhegRender r;
    if (s.sc) {
        int a, b;
        if (pair2(kid(*s.sc->objs.at(0).node, 52), a, b) && a > 0 && b > 0 && a <= 4096 && b <= 4096) { r.width = a; r.height = b; }
    }
    for (Group* g : {s.app.get(), s.sc.get()}) {
        if (!g) continue;
        for (int n : g->order) {
            const Object& o = g->objs.at(n);
            if (o.tag == 20) r.links++;
            if (o.tag >= 9 && o.tag <= 11) r.programs++;
            if (!o.active || (o.tag != 22 && o.tag != 25 && o.tag != 29)) continue;
            MhegDrawOp op;
            op.object = n; op.x = o.x; op.y = o.y; op.w = o.w; op.h = o.h;
            if (o.tag == 25) {
                op.kind = MhegDrawOp::Rectangle; op.fill = o.fill; op.line = o.line;
                if (const MhegNode* lw = kid(*o.node, 82)) op.lineWidth = (int)berInt(lw->value);
            } else if (o.tag == 29) {
                op.kind = MhegDrawOp::Text; op.textColour = o.tcol;
                op.text = o.text; op.ref = o.ref;
                if (op.text.empty() && !o.ref.empty()) {
                    std::vector<uint8_t> d;
                    if (s.getFile(normId(o.ref), d)) op.text.assign(d.begin(), d.end()); else op.missing = true;
                }
                if (const MhegNode* j = kid(*o.node, 87)) op.hJust = (int)berInt(j->value);
                if (const MhegNode* j = kid(*o.node, 88)) op.vJust = (int)berInt(j->value);
                if (const MhegNode* w = kid(*o.node, 91)) op.wrap = !w->value.empty() && w->value[0];
                if (const MhegNode* f = kid(*o.node, 43)) { std::string fs(f->value.begin(), f->value.end()); size_t d = fs.find('.'); if (d != std::string::npos) op.fontSize = atoi(fs.c_str() + d + 1); }
            } else {
                op.kind = MhegDrawOp::Bitmap; op.ref = o.ref;
                if (!o.ref.empty() && !s.getFile(normId(o.ref), op.image)) op.missing = true;
            }
            r.ops.push_back(std::move(op));
        }
    }
    return r;
}

} // namespace dect2
