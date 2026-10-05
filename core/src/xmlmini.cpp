#include "dect2/xmlmini.h"
#include <cctype>
#include <cstdlib>

namespace dect2 {

namespace {

std::string localOf(const std::string& n) { auto p = n.find(':'); return p == std::string::npos ? n : n.substr(p + 1); }

std::string decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 10) { o += s[i]; continue; }
        std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "amp") o += '&';
        else if (ent == "lt") o += '<';
        else if (ent == "gt") o += '>';
        else if (ent == "quot") o += '"';
        else if (ent == "apos") o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            long v = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? strtol(ent.c_str() + 2, nullptr, 16) : strtol(ent.c_str() + 1, nullptr, 10);
            if (v < 0x80) o += (char)v;
            else if (v < 0x800) { o += (char)(0xC0 | (v >> 6)); o += (char)(0x80 | (v & 0x3F)); }
            else { o += (char)(0xE0 | (v >> 12)); o += (char)(0x80 | ((v >> 6) & 0x3F)); o += (char)(0x80 | (v & 0x3F)); }
        } else { o += s[i]; continue; }
        i = e;
    }
    return o;
}

struct Parser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    explicit Parser(const std::string& t) : s(t) {}
    void ws() { while (i < s.size() && isspace((unsigned char)s[i])) i++; }
    bool starts(const char* p) const { return s.compare(i, strlen(p), p) == 0; }
    static size_t strlen(const char* p) { size_t n = 0; while (p[n]) n++; return n; }
    // skips comments, processing instructions, doctype
    void skipMisc() {
        for (;;) {
            ws();
            if (starts("<!--")) { size_t e = s.find("-->", i + 4); i = e == std::string::npos ? s.size() : e + 3; }
            else if (starts("<?")) { size_t e = s.find("?>", i + 2); i = e == std::string::npos ? s.size() : e + 2; }
            else if (starts("<!DOCTYPE")) { size_t e = s.find('>', i); i = e == std::string::npos ? s.size() : e + 1; }
            else return;
        }
    }
    std::string name() {
        size_t b = i;
        while (i < s.size() && !isspace((unsigned char)s[i]) && s[i] != '>' && s[i] != '/' && s[i] != '=') i++;
        return s.substr(b, i - b);
    }
    std::shared_ptr<XmlNode> element() {
        if (++depth > 100 || i >= s.size() || s[i] != '<') return nullptr;
        i++;
        auto n = std::make_shared<XmlNode>();
        n->name = name();
        if (n->name.empty()) return nullptr;
        for (;;) {
            ws();
            if (i >= s.size()) return nullptr;
            if (s[i] == '/') { if (s.compare(i, 2, "/>") != 0) return nullptr; i += 2; depth--; return n; }
            if (s[i] == '>') { i++; break; }
            std::string k = name();
            ws();
            if (i >= s.size() || s[i] != '=' || k.empty()) return nullptr;
            i++; ws();
            if (i >= s.size() || (s[i] != '"' && s[i] != '\'')) return nullptr;
            char q = s[i++];
            size_t e = s.find(q, i);
            if (e == std::string::npos) return nullptr;
            n->attrs[k] = decode(s.substr(i, e - i));
            i = e + 1;
        }
        for (;;) {
            if (i >= s.size()) return nullptr;
            if (s[i] != '<') {
                size_t e = s.find('<', i);
                if (e == std::string::npos) return nullptr;
                n->text += decode(s.substr(i, e - i));
                i = e;
            } else if (starts("<!--")) { size_t e = s.find("-->", i + 4); if (e == std::string::npos) return nullptr; i = e + 3; }
            else if (starts("<![CDATA[")) { size_t e = s.find("]]>", i + 9); if (e == std::string::npos) return nullptr; n->text += s.substr(i + 9, e - i - 9); i = e + 3; }
            else if (starts("<?")) { size_t e = s.find("?>", i + 2); if (e == std::string::npos) return nullptr; i = e + 2; }
            else if (starts("</")) {
                i += 2;
                std::string cn = name();
                ws();
                if (i >= s.size() || s[i] != '>' || cn != n->name) return nullptr;
                i++; depth--;
                return n;
            } else {
                auto c = element();
                if (!c) return nullptr;
                n->children.push_back(c);
            }
        }
    }
};

} // namespace

std::string XmlNode::attr(const std::string& key, const std::string& def) const {
    auto it = attrs.find(key);
    if (it != attrs.end()) return it->second;
    for (auto& a : attrs) if (localOf(a.first) == key) return a.second;
    return def;
}

bool XmlNode::has(const std::string& key) const {
    if (attrs.count(key)) return true;
    for (auto& a : attrs) if (localOf(a.first) == key) return true;
    return false;
}

std::vector<std::shared_ptr<XmlNode>> XmlNode::all(const std::string& n) const {
    std::vector<std::shared_ptr<XmlNode>> v;
    for (auto& c : children) if (c->local() == n) v.push_back(c);
    return v;
}

std::shared_ptr<XmlNode> XmlNode::first(const std::string& n) const {
    for (auto& c : children) if (c->local() == n) return c;
    return nullptr;
}

std::shared_ptr<XmlNode> parseXml(const std::string& text) {
    Parser p(text);
    // a byte order mark
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) p.i = 3;
    p.skipMisc();
    return p.element();
}

} // namespace dect2
