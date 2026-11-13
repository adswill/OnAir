// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/automation/expr.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dect2::automation {

namespace {
constexpr size_t kMaxText = 2000;
constexpr int kMaxDepth = 40;
constexpr int kMaxNodes = 500;
constexpr int kMaxSteps = 5000;
constexpr size_t kMaxResult = 4096;

char lowc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
std::string lowerStr(const std::string& s) { std::string o = s; for (char& c : o) c = lowc(c); return o; }
bool containsCi(const std::string& h, const std::string& n) {
    if (n.empty()) return true;
    if (n.size() > h.size()) return false;
    const std::string a = lowerStr(h), b = lowerStr(n);
    return a.find(b) != std::string::npos;
}

struct Fault { std::string msg; };
}  // namespace

enum class K { Lit, Field, Neg, Not, Bin, And, Or, Call, List };
enum class Op { Eq, Ne, Lt, Le, Gt, Ge, Contains, Starts, Ends, Matches, In, Add, Sub, Mul, Div, Mod };

struct Expr::Node {
    K k = K::Lit;
    Value lit;
    std::string name;
    Op op = Op::Eq;
    std::vector<std::shared_ptr<const Node>> kids;
    int pos = 0;
};

namespace {
using NodeP = std::shared_ptr<const Expr::Node>;

struct FnInfo { const char* name; int lo, hi; };
const FnInfo kFns[] = {
    {"distance_km", 2, 4}, {"bearing_deg", 2, 4}, {"within", 5, 5}, {"inside", 3, 3}, {"hour", 0, 0}, {"minute", 0, 0}, {"weekday", 0, 0},
    {"lower", 1, 1}, {"upper", 1, 1}, {"len", 1, 1}, {"abs", 1, 1}, {"min", 2, 2}, {"max", 2, 2}, {"round", 1, 1}, {"str", 1, 1}, {"num", 1, 1}, {"has", 1, 1},
};

struct Parser {
    const std::string& t;
    size_t i = 0;
    int nodes = 0;
    int depth = 0;
    ExprError err;
    bool bad = false;

    explicit Parser(const std::string& s) : t(s) {}
    NodeP fail(const std::string& m, size_t at) { if (!bad) { bad = true; err.msg = m; err.pos = (int)at; } return nullptr; }
    void ws() { while (i < t.size() && std::isspace((unsigned char)t[i])) i++; }
    std::shared_ptr<Expr::Node> mk(K k, size_t at) {
        auto n = std::make_shared<Expr::Node>();
        n->k = k; n->pos = (int)at;
        if (++nodes > kMaxNodes) fail("expression too large", at);
        return n;
    }
    static bool idStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
    static bool idChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '.'; }
    bool word(const char* w) {   // a keyword at i, as a whole word
        const size_t n = strlen(w);
        if (t.compare(i, n, w) != 0) return false;
        if (i + n < t.size() && idChar(t[i + n])) return false;
        i += n;
        return true;
    }
    bool sym(const char* s) {
        const size_t n = strlen(s);
        if (t.compare(i, n, s) != 0) return false;
        i += n;
        return true;
    }

    NodeP orExpr() {
        NodeP l = andExpr();
        for (;;) {
            ws();
            const size_t at = i;
            if (bad || !(word("or") || sym("||"))) return l;
            NodeP r = andExpr();
            if (bad) return nullptr;
            auto n = mk(K::Or, at); n->kids = {l, r}; l = n;
        }
    }
    NodeP andExpr() {
        NodeP l = notExpr();
        for (;;) {
            ws();
            const size_t at = i;
            if (bad || !(word("and") || sym("&&"))) return l;
            NodeP r = notExpr();
            if (bad) return nullptr;
            auto n = mk(K::And, at); n->kids = {l, r}; l = n;
        }
    }
    NodeP notExpr() {
        ws();
        const size_t at = i;
        if (word("not") || (i < t.size() && t[i] == '!' && !(i + 1 < t.size() && t[i + 1] == '=') && (++i, true))) {
            if (++depth > kMaxDepth) return fail("nested too deep", at);
            NodeP e = notExpr();
            depth--;
            if (bad) return nullptr;
            auto n = mk(K::Not, at); n->kids = {e}; return n;
        }
        return cmpExpr();
    }
    NodeP cmpExpr() {
        NodeP l = addExpr();
        if (bad) return nullptr;
        ws();
        const size_t at = i;
        Op op;
        if (sym("==")) op = Op::Eq;
        else if (sym("!=")) op = Op::Ne;
        else if (sym("<=")) op = Op::Le;
        else if (sym(">=")) op = Op::Ge;
        else if (sym("<")) op = Op::Lt;
        else if (sym(">")) op = Op::Gt;
        else if (word("contains")) op = Op::Contains;
        else if (word("startswith")) op = Op::Starts;
        else if (word("endswith")) op = Op::Ends;
        else if (word("matches")) op = Op::Matches;
        else if (word("in")) op = Op::In;
        else return l;
        NodeP r = addExpr();
        if (bad) return nullptr;
        if (op == Op::In && r->k != K::List) return fail("'in' needs a list such as [1, 2, 3]", at);
        auto n = mk(K::Bin, at); n->op = op; n->kids = {l, r};
        return n;
    }
    NodeP addExpr() {
        NodeP l = mulExpr();
        for (;;) {
            ws();
            if (bad || i >= t.size() || (t[i] != '+' && t[i] != '-')) return l;
            const size_t at = i;
            const Op op = t[i++] == '+' ? Op::Add : Op::Sub;
            NodeP r = mulExpr();
            if (bad) return nullptr;
            auto n = mk(K::Bin, at); n->op = op; n->kids = {l, r}; l = n;
        }
    }
    NodeP mulExpr() {
        NodeP l = unary();
        for (;;) {
            ws();
            if (bad || i >= t.size() || (t[i] != '*' && t[i] != '/' && t[i] != '%')) return l;
            const size_t at = i;
            const char c = t[i++];
            NodeP r = unary();
            if (bad) return nullptr;
            auto n = mk(K::Bin, at); n->op = c == '*' ? Op::Mul : c == '/' ? Op::Div : Op::Mod; n->kids = {l, r}; l = n;
        }
    }
    NodeP unary() {
        ws();
        const size_t at = i;
        if (i < t.size() && t[i] == '-') {
            i++;
            if (++depth > kMaxDepth) return fail("nested too deep", at);
            NodeP e = unary();
            depth--;
            if (bad) return nullptr;
            auto n = mk(K::Neg, at); n->kids = {e}; return n;
        }
        return primary();
    }
    bool unitFactor(const std::string& u, double& mul) {
        static const struct { const char* n; double m; } tab[] = {
            {"hz", 1}, {"khz", 1e3}, {"mhz", 1e6}, {"ghz", 1e9}, {"ms", 0.001}, {"s", 1}, {"min", 60}, {"h", 3600},
            {"km", 1}, {"nm", 1.852}, {"mi", 1.609344}, {"ft", 1}, {"kt", 1}};
        const std::string l = lowerStr(u);
        for (const auto& e : tab) if (l == e.n) { mul = e.m; return true; }
        return false;
    }
    NodeP primary() {
        ws();
        const size_t at = i;
        if (i >= t.size()) return fail("the expression ends too early", at);
        const char c = t[i];
        if (c == '(') {
            i++;
            if (++depth > kMaxDepth) return fail("nested too deep", at);
            NodeP e = orExpr();
            depth--;
            if (bad) return nullptr;
            ws();
            if (i >= t.size() || t[i] != ')') return fail("missing ')'", i);
            i++;
            return e;
        }
        if (c == '[') {
            i++;
            auto n = mk(K::List, at);
            ws();
            if (i < t.size() && t[i] == ']') { i++; return n; }
            for (;;) {
                if (++depth > kMaxDepth) return fail("nested too deep", i);
                NodeP e = orExpr();
                depth--;
                if (bad) return nullptr;
                n->kids.push_back(e);
                ws();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == ']') { i++; return n; }
                return fail("expected ',' or ']'", i);
            }
        }
        if (c == '"' || c == '\'') {
            i++;
            std::string s;
            while (i < t.size() && t[i] != c) {
                if (t[i] == '\\' && i + 1 < t.size()) { i++; s += t[i] == 'n' ? '\n' : t[i] == 't' ? '\t' : t[i]; i++; }
                else s += t[i++];
            }
            if (i >= t.size()) return fail("unterminated text", at);
            i++;
            auto n = mk(K::Lit, at); n->lit = Value::string(s); return n;
        }
        if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < t.size() && std::isdigit((unsigned char)t[i + 1]))) {
            size_t j = i;
            while (j < t.size() && (std::isdigit((unsigned char)t[j]) || t[j] == '.')) j++;
            if (j < t.size() && (t[j] == 'e' || t[j] == 'E')) {
                size_t k = j + 1;
                if (k < t.size() && (t[k] == '+' || t[k] == '-')) k++;
                if (k < t.size() && std::isdigit((unsigned char)t[k])) { while (k < t.size() && std::isdigit((unsigned char)t[k])) k++; j = k; }
            }
            const std::string num = t.substr(i, j - i);
            char* end = nullptr;
            double v = std::strtod(num.c_str(), &end);
            if (!end || *end || !std::isfinite(v)) return fail("bad number", at);
            i = j;
            size_t u = i;
            while (u < t.size() && std::isalpha((unsigned char)t[u])) u++;
            if (u > i) {
                double m;
                if (!unitFactor(t.substr(i, u - i), m)) return fail("unknown unit '" + t.substr(i, u - i) + "'", i);
                v *= m; i = u;
            }
            auto n = mk(K::Lit, at); n->lit = Value::number(v); return n;
        }
        if (idStart(c)) {
            size_t j = i;
            while (j < t.size() && idChar(t[j])) j++;
            const std::string id = t.substr(i, j - i);
            if (id == "true" || id == "false") { i = j; auto n = mk(K::Lit, at); n->lit = Value::boolean(id == "true"); return n; }
            if (id == "null") { i = j; return mk(K::Lit, at); }
            size_t k = j;
            while (k < t.size() && std::isspace((unsigned char)t[k])) k++;
            if (k < t.size() && t[k] == '(') {   // a call
                const FnInfo* fn = nullptr;
                for (const FnInfo& f : kFns) if (id == f.name) fn = &f;
                if (!fn) return fail("unknown function '" + id + "'", at);
                i = k + 1;
                auto n = mk(K::Call, at); n->name = id;
                ws();
                if (i < t.size() && t[i] == ')') i++;
                else for (;;) {
                    if (++depth > kMaxDepth) return fail("nested too deep", i);
                    NodeP e = orExpr();
                    depth--;
                    if (bad) return nullptr;
                    n->kids.push_back(e);
                    ws();
                    if (i < t.size() && t[i] == ',') { i++; continue; }
                    if (i < t.size() && t[i] == ')') { i++; break; }
                    return fail("expected ',' or ')'", i);
                }
                const int na = (int)n->kids.size();
                if (na < fn->lo || na > fn->hi) return fail("'" + id + "' takes " + (fn->lo == fn->hi ? std::to_string(fn->lo) : std::to_string(fn->lo) + " to " + std::to_string(fn->hi)) + " arguments, got " + std::to_string(na), at);
                if ((id == "distance_km" || id == "bearing_deg") && na != 2 && na != 4) return fail("'" + id + "' takes 2 or 4 arguments", at);
                return n;
            }
            static const char* const reserved[] = {"and", "or", "not", "in", "contains", "startswith", "endswith", "matches"};
            for (const char* r : reserved) if (id == r) return fail("'" + id + "' is an operator, a value is expected here", at);
            i = j;
            auto n = mk(K::Field, at); n->name = id; return n;
        }
        return fail(std::string("unexpected '") + c + "'", at);
    }
};

struct Ctx {
    const Env& env;
    int steps = 0;
};

[[noreturn]] void die(const std::string& m) { throw Fault{m}; }

bool numFromText(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end && !*end && std::isfinite(v);
}

Value fieldOf(const Env& e, const std::string& k) {
    if (!e.ev) return Value();
    const Event& ev = *e.ev;
    if (k == "type") return Value::string(ev.type);
    if (k == "mode") return Value::string(ev.mode);
    if (k == "freq") return ev.freqHz != 0 ? Value::number(ev.freqHz) : Value();
    if (k == "freq_mhz") return ev.freqHz != 0 ? Value::number(ev.freqHz / 1e6) : Value();
    if (k == "time") return Value::number(ev.time);
    auto it = ev.f.find(k);
    return it == ev.f.end() ? Value() : it->second;
}

bool looseEq(const Value& a, const Value& b) {
    if (a.null() || b.null()) return a.null() && b.null();
    if (a.t == Value::Num && b.t == Value::Num) return a.n == b.n;
    if (a.t == Value::Str && b.t == Value::Str) return a.s == b.s;
    if (a.t == Value::Bool && b.t == Value::Bool) return a.b == b.b;
    double x, y;
    const bool ax = a.t == Value::Num ? (x = a.n, true) : a.t == Value::Str ? numFromText(a.s, x) : a.t == Value::Bool ? (x = a.b, true) : false;
    const bool by = b.t == Value::Num ? (y = b.n, true) : b.t == Value::Str ? numFromText(b.s, y) : b.t == Value::Bool ? (y = b.b, true) : false;
    return ax && by && x == y;
}

int order(const Value& a, const Value& b, bool& ok) {   // -1, 0, 1; ok false when they cannot be compared
    ok = true;
    if (a.null() || b.null()) { ok = false; return 0; }
    if (a.t == Value::Str && b.t == Value::Str) { const int c = a.s.compare(b.s); return c < 0 ? -1 : c > 0 ? 1 : 0; }
    double x = 0, y = 0;
    bool ax = true, by = true;
    if (a.t == Value::Num) x = a.n; else if (a.t == Value::Str) ax = numFromText(a.s, x); else ax = false;
    if (b.t == Value::Num) y = b.n; else if (b.t == Value::Str) by = numFromText(b.s, y); else by = false;
    if (!ax || !by) { ok = false; return 0; }
    return x < y ? -1 : x > y ? 1 : 0;
}

double needNum(const Value& v, const char* fn) {
    if (v.t == Value::Num) return v.n;
    double d;
    if (v.t == Value::Str && numFromText(v.s, d)) return d;
    die(std::string(fn) + " needs a number");
}

Value eval(const Expr::Node& n, Ctx& c);

Value callFn(const Expr::Node& n, Ctx& c) {
    const std::string& f = n.name;
    std::vector<Value> a;
    a.reserve(n.kids.size());
    for (const auto& k : n.kids) a.push_back(eval(*k, c));
    auto anyNull = [&] { for (const Value& v : a) if (v.null()) return true; return false; };
    if (f == "hour" || f == "minute" || f == "weekday") {
        const long long t = (long long)std::floor(c.env.now) + c.env.tzOffsetSec;
        const long long day = (long long)std::floor((double)t / 86400.0);
        const long long sod = t - day * 86400;
        if (f == "hour") return Value::number((double)(sod / 3600));
        if (f == "minute") return Value::number((double)((sod / 60) % 60));
        return Value::number((double)(((day % 7) + 11) % 7));   // 1970-01-01 was a Thursday; Sunday = 0
    }
    if (f == "has") {
        const Env& e = c.env;
        if (a[0].t != Value::Str) return Value::boolean(false);
        return Value::boolean(!fieldOf(e, a[0].s).null());
    }
    if (f == "str") return Value::string(a[0].text());
    if (f == "num") {
        double d;
        if (a[0].t == Value::Num) return a[0];
        if (a[0].t == Value::Str && numFromText(a[0].s, d)) return Value::number(d);
        return Value();
    }
    if (f == "lower" || f == "upper") {
        if (a[0].null()) return Value();
        std::string s = a[0].text();
        for (char& ch : s) ch = f == "lower" ? lowc(ch) : (ch >= 'a' && ch <= 'z' ? (char)(ch - 32) : ch);
        return Value::string(s);
    }
    if (f == "len") return a[0].null() ? Value() : Value::number((double)a[0].text().size());
    if (anyNull()) return Value();
    if (f == "abs") return Value::number(std::fabs(needNum(a[0], "abs")));
    if (f == "round") return Value::number(std::round(needNum(a[0], "round")));
    if (f == "min") return Value::number(std::min(needNum(a[0], "min"), needNum(a[1], "min")));
    if (f == "max") return Value::number(std::max(needNum(a[0], "max"), needNum(a[1], "max")));
    if (f == "distance_km" || f == "bearing_deg") {
        double la1, lo1, la2, lo2;
        if (a.size() == 2) {
            if (!c.env.stationSet) return Value();   // the receiver position is not set: the condition cannot hold
            la1 = c.env.stationLat; lo1 = c.env.stationLon;
            la2 = needNum(a[0], f.c_str()); lo2 = needNum(a[1], f.c_str());
        } else {
            la1 = needNum(a[0], f.c_str()); lo1 = needNum(a[1], f.c_str()); la2 = needNum(a[2], f.c_str()); lo2 = needNum(a[3], f.c_str());
        }
        return Value::number(f == "distance_km" ? geo::distanceKm(la1, lo1, la2, lo2) : geo::bearingDeg(la1, lo1, la2, lo2));
    }
    if (f == "within") {
        return Value::boolean(geo::distanceKm(needNum(a[0], "within"), needNum(a[1], "within"), needNum(a[2], "within"), needNum(a[3], "within")) <= needNum(a[4], "within"));
    }
    if (f == "inside") {
        std::vector<std::pair<double, double>> pts;
        if (a[2].t != Value::Str || !geo::parsePolygon(a[2].s, pts)) die("inside needs a polygon text such as \"54.1,24.3; 54.1,25.0; 54.7,24.6\"");
        return Value::boolean(geo::pointInPolygon(needNum(a[0], "inside"), needNum(a[1], "inside"), pts));
    }
    die("unknown function " + f);
}

Value eval(const Expr::Node& n, Ctx& c) {
    if (++c.steps > kMaxSteps) die("the expression needs too many steps");
    switch (n.k) {
    case K::Lit: return n.lit;
    case K::Field: return fieldOf(c.env, n.name);
    case K::List: die("a list can only be used after 'in'");
    case K::Neg: { const Value v = eval(*n.kids[0], c); if (v.null()) return v; return Value::number(-needNum(v, "'-'")); }
    case K::Not: return Value::boolean(!eval(*n.kids[0], c).truthy());
    case K::And: return Value::boolean(eval(*n.kids[0], c).truthy() && eval(*n.kids[1], c).truthy());
    case K::Or: return Value::boolean(eval(*n.kids[0], c).truthy() || eval(*n.kids[1], c).truthy());
    case K::Call: return callFn(n, c);
    case K::Bin: break;
    }
    if (n.op == Op::In) {
        const Value l = eval(*n.kids[0], c);
        for (const auto& e : n.kids[1]->kids) if (looseEq(l, eval(*e, c))) return Value::boolean(true);
        return Value::boolean(false);
    }
    const Value l = eval(*n.kids[0], c), r = eval(*n.kids[1], c);
    switch (n.op) {
    case Op::Eq: return Value::boolean(looseEq(l, r));
    case Op::Ne: return Value::boolean(!looseEq(l, r));
    case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
        bool ok;
        const int o = order(l, r, ok);
        if (!ok) return Value::boolean(false);
        return Value::boolean(n.op == Op::Lt ? o < 0 : n.op == Op::Le ? o <= 0 : n.op == Op::Gt ? o > 0 : o >= 0);
    }
    case Op::Contains: return Value::boolean(!l.null() && !r.null() && containsCi(l.text(), r.text()));
    case Op::Starts: {
        if (l.null() || r.null()) return Value::boolean(false);
        const std::string a = lowerStr(l.text()), b = lowerStr(r.text());
        return Value::boolean(a.size() >= b.size() && a.compare(0, b.size(), b) == 0);
    }
    case Op::Ends: {
        if (l.null() || r.null()) return Value::boolean(false);
        const std::string a = lowerStr(l.text()), b = lowerStr(r.text());
        return Value::boolean(a.size() >= b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0);
    }
    case Op::Matches: return Value::boolean(!l.null() && !r.null() && globMatch(r.text(), l.text()));
    case Op::Add:
        if (l.null() || r.null()) return Value();
        if (l.t == Value::Str || r.t == Value::Str) {
            std::string s = l.text();
            s += r.text();
            if (s.size() > kMaxResult) die("the text is too long");
            return Value::string(std::move(s));
        }
        return Value::number(needNum(l, "'+'") + needNum(r, "'+'"));
    case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: {
        if (l.null() || r.null()) return Value();
        const double x = needNum(l, "arithmetic"), y = needNum(r, "arithmetic");
        double v;
        if (n.op == Op::Sub) v = x - y;
        else if (n.op == Op::Mul) v = x * y;
        else {
            if (y == 0) die("division by zero");
            v = n.op == Op::Div ? x / y : std::fmod(x, y);
        }
        if (!std::isfinite(v)) die("the result is not a number");
        return Value::number(v);
    }
    default: break;
    }
    die("bad expression");
}
}  // namespace

std::shared_ptr<const Expr> Expr::parse(const std::string& text, ExprError& err) {
    auto e = std::make_shared<Expr>();
    e->src_ = text;
    err = ExprError();
    if (text.size() > kMaxText) { err.msg = "expression longer than 2000 characters"; err.pos = (int)kMaxText; return nullptr; }
    size_t b = 0;
    while (b < text.size() && std::isspace((unsigned char)text[b])) b++;
    if (b == text.size()) return e;   // empty: always true
    Parser p(text);
    NodeP root = p.orExpr();
    if (!p.bad) { p.ws(); if (p.i < text.size()) p.fail(std::string("unexpected '") + text[p.i] + "'", p.i); }
    if (p.bad) { err = p.err; return nullptr; }
    e->root_ = root;
    return e;
}

Value Expr::eval(const Env& env, std::string* err) const {
    if (!root_) return Value::boolean(true);
    Ctx c{env};
    try {
        return automation::eval(*root_, c);
    } catch (const Fault& f) {
        if (err) *err = f.msg;
        return Value();
    }
}

bool Expr::test(const Env& env, std::string* err) const {
    if (!root_) return true;
    std::string e;
    const Value v = eval(env, &e);
    if (!e.empty()) { if (err) *err = e; return false; }
    return v.truthy();
}

namespace geo {
double distanceKm(double lat1, double lon1, double lat2, double lon2) {
    constexpr double R = 6371.0088, D = M_PI / 180;
    const double dla = (lat2 - lat1) * D, dlo = (lon2 - lon1) * D;
    const double a = std::sin(dla / 2) * std::sin(dla / 2) + std::cos(lat1 * D) * std::cos(lat2 * D) * std::sin(dlo / 2) * std::sin(dlo / 2);
    return 2 * R * std::asin(std::min(1.0, std::sqrt(a)));
}
double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    constexpr double D = M_PI / 180;
    const double dlo = (lon2 - lon1) * D;
    const double y = std::sin(dlo) * std::cos(lat2 * D);
    const double x = std::cos(lat1 * D) * std::sin(lat2 * D) - std::sin(lat1 * D) * std::cos(lat2 * D) * std::cos(dlo);
    double b = std::atan2(y, x) / D;
    if (b < 0) b += 360;
    return b;
}
bool parsePolygon(const std::string& text, std::vector<std::pair<double, double>>& pts) {
    pts.clear();
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find(';', i);
        if (e == std::string::npos) e = text.size();
        const std::string part = text.substr(i, e - i);
        i = e + 1;
        if (part.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        const size_t comma = part.find(',');
        if (comma == std::string::npos) return false;
        auto trim = [](std::string x) {
            const size_t b = x.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) return std::string();
            x.erase(0, b);
            while (!x.empty() && std::isspace((unsigned char)x.back())) x.pop_back();
            return x;
        };
        double la, lo;
        if (!numFromText(trim(part.substr(0, comma)), la) || !numFromText(trim(part.substr(comma + 1)), lo)) return false;
        if (std::fabs(la) > 90 || std::fabs(lo) > 180) return false;
        pts.push_back({la, lo});
        if (pts.size() > 64) return false;
    }
    return pts.size() >= 3;
}
bool pointInPolygon(double lat, double lon, const std::vector<std::pair<double, double>>& p) {
    bool in = false;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
        const double yi = p[i].first, xi = p[i].second, yj = p[j].first, xj = p[j].second;
        if (((yi > lat) != (yj > lat)) && (lon < (xj - xi) * (lat - yi) / (yj - yi) + xi)) in = !in;
    }
    return in;
}
}  // namespace geo

bool globMatch(const std::string& p, const std::string& s) {
    size_t pi = 0, si = 0, star = std::string::npos, mark = 0;
    while (si < s.size()) {
        if (pi < p.size() && p[pi] == '*') { star = pi++; mark = si; }
        else if (pi < p.size() && (p[pi] == '?' || lowc(p[pi]) == lowc(s[si]))) { pi++; si++; }
        else if (star != std::string::npos) { pi = star + 1; si = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == '*') pi++;
    return pi == p.size();
}

}  // namespace dect2::automation
