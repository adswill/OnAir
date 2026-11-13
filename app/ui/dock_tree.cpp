// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The docking layout tree. See dock_tree.h.
#include "dock_tree.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dock {

namespace {

float clampRatio(float r) { return std::isfinite(r) ? std::max(0.05f, std::min(0.95f, r)) : 0.5f; }

Node* leafOfMut(Node& n, const std::string& name) {
    if (n.kind == Node::Leaf) return std::find(n.tabs.begin(), n.tabs.end(), name) != n.tabs.end() ? &n : nullptr;
    for (Node& k : n.kids) if (Node* f = leafOfMut(k, name)) return f;
    return nullptr;
}

// a split with an empty child becomes its other child
void normalise(Node& n) {
    if (n.kind == Node::Leaf) { if (n.active >= (int)n.tabs.size() || n.active < 0) n.active = 0; return; }
    for (Node& k : n.kids) normalise(k);
    if (n.kids.size() != 2) { n = Node(); return; }
    if (n.kids[0].empty()) { Node keep = std::move(n.kids[1]); n = std::move(keep); }
    else if (n.kids[1].empty()) { Node keep = std::move(n.kids[0]); n = std::move(keep); }
}

void collect(const Node& n, std::vector<std::string>& out) {
    if (n.kind == Node::Leaf) { out.insert(out.end(), n.tabs.begin(), n.tabs.end()); return; }
    for (const Node& k : n.kids) collect(k, out);
}

bool anyAlive(const Node& n, const std::function<bool(const std::string&)>& alive) {
    if (n.kind == Node::Leaf) { for (const auto& t : n.tabs) if (!alive || alive(t)) return true; return false; }
    for (const Node& k : n.kids) if (anyAlive(k, alive)) return true;
    return false;
}

void layoutRec(const Node& n, Rect r, float gap, const std::function<bool(const std::string&)>& alive,
               std::vector<LeafBox>& leaves, std::vector<SplitBar>& bars, std::vector<int>& path) {
    if (n.kind == Node::Leaf) {
        LeafBox b;
        b.r = r;
        for (int i = 0; i < (int)n.tabs.size(); i++) {
            if (alive && !alive(n.tabs[i])) continue;
            if (i == n.active) b.active = (int)b.tabs.size();
            b.tabs.push_back(n.tabs[i]);
        }
        if (!b.tabs.empty()) leaves.push_back(std::move(b));
        return;
    }
    const bool a0 = anyAlive(n.kids[0], alive), a1 = anyAlive(n.kids[1], alive);
    if (!a0 && !a1) return;
    if (a0 != a1) { path.push_back(a0 ? 0 : 1); layoutRec(n.kids[a0 ? 0 : 1], r, gap, alive, leaves, bars, path); path.pop_back(); return; }
    const bool h = n.kind == Node::SplitH;
    const float len = h ? r.w : r.h;
    const float g = std::min(gap, std::max(0.f, len));
    const float first = std::floor((len - g) * clampRatio(n.ratio));
    Rect r0 = r, r1 = r;
    SplitBar bar;
    bar.vertical = h;
    bar.origin = h ? r.x : r.y;
    bar.len = len;
    bar.r = r;
    if (h) { r0.w = first; r1.x = r.x + first + g; r1.w = std::max(0.f, r.w - first - g); bar.r.x = r.x + first; bar.r.w = g; }
    else   { r0.h = first; r1.y = r.y + first + g; r1.h = std::max(0.f, r.h - first - g); bar.r.y = r.y + first; bar.r.h = g; }
    bar.path = path;
    bars.push_back(bar);
    path.push_back(0); layoutRec(n.kids[0], r0, gap, alive, leaves, bars, path); path.pop_back();
    path.push_back(1); layoutRec(n.kids[1], r1, gap, alive, leaves, bars, path); path.pop_back();
}

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '|' || c == '(' || c == ')' || c == ',' || c == '\\') o += '\\'; o += c; }
    return o;
}

struct Parser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    bool eat(char c) { if (i < s.size() && s[i] == c) { i++; return true; } return false; }
    bool num(long& v) {
        size_t j = i;
        while (j < s.size() && (std::isdigit((unsigned char)s[j]) || (j == i && s[j] == '-'))) j++;
        if (j == i || j - i > 9) return false;
        v = std::strtol(s.substr(i, j - i).c_str(), nullptr, 10);
        i = j;
        return true;
    }
    bool name(std::string& out) {
        out.clear();
        while (i < s.size()) {
            char c = s[i];
            if (c == '\\') { if (i + 1 >= s.size()) return false; out += s[i + 1]; i += 2; continue; }
            if (c == '|' || c == ')' || c == '(' || c == ',') break;
            out += c; i++;
        }
        return !out.empty();
    }
    bool node(Node& n) {
        if (++depth > 64) return false;
        bool ok = false;
        if (eat('T')) {
            n = Node();
            long act = 0;
            if (!eat('(') || !num(act)) return false;
            while (eat('|')) { std::string nm; if (!name(nm)) return false; n.tabs.push_back(nm); }
            if (!eat(')') || n.tabs.empty()) return false;
            n.active = (act >= 0 && act < (long)n.tabs.size()) ? (int)act : 0;
            ok = true;
        } else if (i < s.size() && (s[i] == 'H' || s[i] == 'V')) {
            n = Node();
            n.kind = s[i] == 'H' ? Node::SplitH : Node::SplitV;
            i++;
            long r = 0;
            if (!eat('(') || !num(r) || !eat(',')) return false;
            n.ratio = clampRatio(r / 10000.f);
            n.kids.resize(2);
            if (!node(n.kids[0]) || !eat(',') || !node(n.kids[1]) || !eat(')')) return false;
            ok = true;
        }
        depth--;
        return ok;
    }
};

}  // namespace

bool contains(const Node& root, const std::string& name) { return leafOf(root, name) != nullptr; }

std::vector<std::string> panels(const Node& root) { std::vector<std::string> v; collect(root, v); return v; }

const Node* leafOf(const Node& root, const std::string& name) { return leafOfMut(const_cast<Node&>(root), name); }

bool remove(Node& root, const std::string& name) {
    Node* l = leafOfMut(root, name);
    if (!l) return false;
    const int idx = (int)(std::find(l->tabs.begin(), l->tabs.end(), name) - l->tabs.begin());
    l->tabs.erase(l->tabs.begin() + idx);
    if (l->active > idx || l->active >= (int)l->tabs.size()) l->active = std::max(0, l->active - 1);
    normalise(root);
    return true;
}

bool dockAt(Node& root, const std::string& target, Zone z, const std::string& name, float share) {
    if (name.empty() || z == Zone::None || name == target) return false;
    if (contains(root, name)) {   // moving: a centre drop onto its own leaf is no move at all
        const Node* own = leafOf(root, name);
        if (z == Zone::Centre && own && !target.empty() && std::find(own->tabs.begin(), own->tabs.end(), target) != own->tabs.end()) return false;
        if (own && own->tabs.size() == 1 && !target.empty() && own == leafOf(root, target)) return false;
        remove(root, name);
    }
    if (root.empty()) { root = Node(); root.tabs.push_back(name); return true; }
    Node* at = target.empty() ? nullptr : leafOfMut(root, target);
    if (!at) at = &root;
    if (z == Zone::Centre) {
        if (at->kind != Node::Leaf) {   // a tab of the whole tree makes no sense: dock into its first leaf
            Node* l = at;
            while (l->kind != Node::Leaf) l = &l->kids[0];
            at = l;
        }
        at->tabs.push_back(name);
        at->active = (int)at->tabs.size() - 1;
        return true;
    }
    Node fresh;
    fresh.tabs.push_back(name);
    Node old = std::move(*at);
    Node split;
    split.kind = (z == Zone::Left || z == Zone::Right) ? Node::SplitH : Node::SplitV;
    const bool first = z == Zone::Left || z == Zone::Top;
    const float s = clampRatio(share);
    split.ratio = first ? s : 1.f - s;
    split.kids.resize(2);
    split.kids[first ? 0 : 1] = std::move(fresh);
    split.kids[first ? 1 : 0] = std::move(old);
    *at = std::move(split);
    return true;
}

bool setActive(Node& root, const std::string& name) {
    Node* l = leafOfMut(root, name);
    if (!l) return false;
    const int idx = (int)(std::find(l->tabs.begin(), l->tabs.end(), name) - l->tabs.begin());
    if (l->active == idx) return false;
    l->active = idx;
    return true;
}

std::string cycleTab(const Node& root, const std::string& name, int dir, const std::function<bool(const std::string&)>& alive) {
    const Node* l = leafOf(root, name);
    if (!l) return name;
    const int n = (int)l->tabs.size();
    int idx = (int)(std::find(l->tabs.begin(), l->tabs.end(), name) - l->tabs.begin());
    for (int step = 0; step < n; step++) {
        idx = ((idx + (dir < 0 ? -1 : 1)) % n + n) % n;
        if (!alive || alive(l->tabs[idx])) return l->tabs[idx];
    }
    return name;
}

void layout(const Node& root, Rect r, float gap, const std::function<bool(const std::string&)>& alive,
            std::vector<LeafBox>& leaves, std::vector<SplitBar>& bars) {
    leaves.clear();
    bars.clear();
    std::vector<int> path;
    layoutRec(root, r, gap, alive, leaves, bars, path);
}

float ratioAt(float pos, float origin, float len, float minPx) {
    if (!(len > 0)) return 0.5f;
    float lo = minPx / len, hi = 1.f - minPx / len;
    if (lo > hi) lo = hi = 0.5f;
    return clampRatio(std::max(lo, std::min(hi, (pos - origin) / len)));
}

bool setRatio(Node& root, const std::vector<int>& path, float ratio) {
    Node* n = &root;
    for (int k : path) { if (n->kind == Node::Leaf || k < 0 || k > 1 || (int)n->kids.size() != 2) return false; n = &n->kids[k]; }
    if (n->kind == Node::Leaf) return false;
    n->ratio = clampRatio(ratio);
    return true;
}

Rect zoneRect(Rect r, Zone z, float size, bool outer) {
    const float cx = r.x + r.w * 0.5f - size * 0.5f, cy = r.y + r.h * 0.5f - size * 0.5f;
    const float m = size * 0.25f, step = size * 1.2f;
    switch (z) {
    case Zone::Left:   return outer ? Rect{r.x + m, cy, size, size} : Rect{cx - step, cy, size, size};
    case Zone::Right:  return outer ? Rect{r.x + r.w - m - size, cy, size, size} : Rect{cx + step, cy, size, size};
    case Zone::Top:    return outer ? Rect{cx, r.y + m, size, size} : Rect{cx, cy - step, size, size};
    case Zone::Bottom: return outer ? Rect{cx, r.y + r.h - m - size, size, size} : Rect{cx, cy + step, size, size};
    case Zone::Centre: return outer ? Rect{} : Rect{cx, cy, size, size};
    default: return Rect{};
    }
}

Zone hitTest(Rect r, float px, float py, float size, bool outer) {
    for (Zone z : {Zone::Centre, Zone::Left, Zone::Right, Zone::Top, Zone::Bottom}) {
        if (outer && z == Zone::Centre) continue;
        if (zoneRect(r, z, size, outer).contains(px, py)) return z;
    }
    return Zone::None;
}

Rect previewRect(Rect r, Zone z, float share) {
    const float s = clampRatio(share);
    switch (z) {
    case Zone::Left:   return Rect{r.x, r.y, std::floor(r.w * s), r.h};
    case Zone::Right:  return Rect{r.x + r.w - std::floor(r.w * s), r.y, std::floor(r.w * s), r.h};
    case Zone::Top:    return Rect{r.x, r.y, r.w, std::floor(r.h * s)};
    case Zone::Bottom: return Rect{r.x, r.y + r.h - std::floor(r.h * s), r.w, std::floor(r.h * s)};
    case Zone::Centre: return r;
    default: return Rect{};
    }
}

std::string serialise(const Node& n) {
    if (n.empty()) return "";
    if (n.kind == Node::Leaf) {
        std::string s = "T(" + std::to_string(n.active);
        for (const auto& t : n.tabs) s += "|" + esc(t);
        return s + ")";
    }
    if (n.kids.size() != 2) return "";
    return std::string(n.kind == Node::SplitH ? "H(" : "V(") + std::to_string((long)std::lround(clampRatio(n.ratio) * 10000)) + "," +
           serialise(n.kids[0]) + "," + serialise(n.kids[1]) + ")";
}

bool parse(const std::string& s, Node& out) {
    out = Node();
    if (s.empty()) return true;
    Parser p{s};
    Node n;
    if (!p.node(n) || p.i != s.size()) return false;
    normalise(n);
    out = std::move(n);
    return true;
}

std::vector<Named> parseNamed(const std::string& text) {
    std::vector<Named> v;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        Named n{line.substr(0, t1), line.substr(t1 + 1, t2 - t1 - 1), line.substr(t2 + 1)};
        Node check;
        if (n.name.empty() || !parse(n.tree, check)) continue;
        putNamed(v, n.mode, n.name, n.tree);
    }
    return v;
}

std::string writeNamed(const std::vector<Named>& v) {
    std::string s;
    for (const Named& n : v) s += n.mode + "\t" + n.name + "\t" + n.tree + "\n";
    return s;
}

void putNamed(std::vector<Named>& v, const std::string& mode, const std::string& name, const std::string& tree) {
    std::string nm;
    for (char c : name) nm += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    for (Named& n : v) if (n.mode == mode && n.name == nm) { n.tree = tree; return; }
    v.push_back(Named{mode, nm, tree});
}

bool dropNamed(std::vector<Named>& v, const std::string& mode, const std::string& name) {
    const auto it = std::remove_if(v.begin(), v.end(), [&](const Named& n) { return n.mode == mode && n.name == name; });
    const bool any = it != v.end();
    v.erase(it, v.end());
    return any;
}

}  // namespace dock
