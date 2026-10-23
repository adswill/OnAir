// Static preview of an MHEG-5 Scene: the Rectangle, Text and Bitmap items as draw operations. Tag numbers: ISO13522-MHEG-5 ASN.1 module (ITU-T T.172), as published in
// the RedButton engine (redbutton-browser/ISO13522-MHEG-5.asn); see mheg.h. Nothing is executed. Parts of the data that the standard leaves to a profile (the colour
// octets, the textual font attributes, the character set) are read the way the UK profile (ETSI ES 202 184) uses them and are tolerant: when they do not fit, defaults are used.
#include "dect2/tv/mheg.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace {
const MhegNode* kid(const MhegNode& n, uint32_t tag) {
    for (const auto& k : n.kids) if (k.cls == 2 && k.tag == tag) return &k;
    return nullptr;
}
int64_t berInt(const std::vector<uint8_t>& v) {
    if (v.empty() || v.size() > 8) return 0;
    int64_t r = (v[0] & 0x80) ? -1 : 0;
    for (uint8_t c : v) r = (int64_t)(((uint64_t)r << 8) | c);
    return r;
}
// an INTEGER given either directly (primitive context tag) or wrapped in an explicit tag (CHOICE)
bool intOf(const MhegNode* n, int64_t& out) {
    if (!n) return false;
    if (!n->constructed) { out = berInt(n->value); return true; }
    for (const auto& k : n->kids) if (!k.constructed && k.cls == 0 && (k.tag == 2 || k.tag == 10)) { out = berInt(k.value); return true; }
    return false;
}
// two integers in a SEQUENCE (box size, position, coordinate system)
bool pairOf(const MhegNode* n, int64_t& a, int64_t& b) {
    if (!n || !n->constructed) return false;
    std::vector<int64_t> v;
    for (const auto& k : n->kids) if (!k.constructed && k.cls == 0 && k.tag == 2) v.push_back(berInt(k.value));
    if (v.size() < 2) return false;
    a = v[0]; b = v[1];
    return true;
}
bool boolOf(const MhegNode* n, bool def) {
    if (!n) return def;
    if (!n->constructed) return !n->value.empty() && n->value[0] != 0;
    for (const auto& k : n->kids) if (!k.constructed && k.cls == 0 && k.tag == 1) return !k.value.empty() && k.value[0] != 0;
    return def;
}
const std::vector<uint8_t>* octetsOf(const MhegNode* n) {   // an OCTET STRING given directly or wrapped
    if (!n) return nullptr;
    if (!n->constructed) return &n->value;
    for (const auto& k : n->kids) if (!k.constructed && k.cls == 0 && k.tag == 4) return &k.value;
    return nullptr;
}
bool colourOf(const MhegNode* n, uint32_t& rgba) {   // absolute-colour OCTET STRING; a colour-index INTEGER needs a palette that is not resolved
    if (!n) return false;
    const std::vector<uint8_t>* v = octetsOf(n);
    if (!v || v->size() < 3 || v->size() > 4) return false;
    const uint8_t a = v->size() >= 4 ? (uint8_t)(255 - (*v)[3]) : 255;
    rgba = ((uint32_t)(*v)[0] << 24) | ((uint32_t)(*v)[1] << 16) | ((uint32_t)(*v)[2] << 8) | a;
    return true;
}
bool validUtf8(const std::vector<uint8_t>& s) {
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t c = s[i];
        const size_t n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 99;
        if (n == 99 || i + n >= s.size() + (n == 0 ? 1 : 0)) return false;
        for (size_t k = 1; k <= n; k++) if ((s[i + k] & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}
std::string toText(const std::vector<uint8_t>& b) {
    std::string o;
    const bool u8 = validUtf8(b);
    for (size_t i = 0; i < b.size(); i++) {
        const uint8_t c = b[i];
        if (c == 0x1B) { i += 2; continue; }   // an escape code and its two bytes (colour and style codes of the UK text profile)
        if (c < 0x20 && c != '\n' && c != '\t') { if (c == 0x0D) o += '\n'; continue; }
        if (c >= 0x80 && !u8) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }   // Latin-1
        else o += (char)c;
    }
    return o;
}
int fontSizeOf(const MhegNode* n) {   // "plain.28.32.0": style, size, line height, letter spacing
    const std::vector<uint8_t>* v = octetsOf(n);
    if (!v) return 0;
    const std::string s(v->begin(), v->end());
    const size_t d = s.find('.');
    if (d == std::string::npos) return 0;
    const int sz = atoi(s.c_str() + d + 1);
    return sz >= 6 && sz <= 200 ? sz : 0;
}
// original-content: included OCTET STRING or ReferencedContent SEQUENCE; returns false when absent
bool contentOf(const MhegNode* n, std::vector<uint8_t>& inl, std::string& ref) {
    if (!n) return false;
    if (!n->constructed) { inl = n->value; return true; }
    for (const auto& k : n->kids) {
        if (k.cls == 0 && k.tag == 4 && !k.constructed) { inl = k.value; return true; }
        if (k.cls == 0 && k.tag == 16 && k.constructed)
            for (const auto& f : k.kids) if (f.cls == 0 && f.tag == 4 && !f.constructed) { ref.assign(f.value.begin(), f.value.end()); return true; }
    }
    return false;
}
int objectNumber(const MhegNode& item) {   // rootClass: ExternalReference SEQUENCE { group-identifier, object-number } or an internal-reference INTEGER
    for (const auto& k : item.kids) {
        if (k.cls == 0 && k.tag == 16 && k.kids.size() >= 2 && !k.kids[1].constructed) return (int)berInt(k.kids[1].value);
        if (k.cls == 0 && k.tag == 2) return (int)berInt(k.value);
    }
    return 0;
}
bool isImage(const std::vector<uint8_t>& d) {
    return (d.size() > 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') || (d.size() > 4 && d[0] == 0xFF && d[1] == 0xD8);
}
} // namespace

MhegRender mhegRenderScene(const MhegNode& scene, const MhegResolve& resolve) {
    MhegRender r;
    if (scene.cls != 2 || scene.tag != 1) { r.notes.push_back("not a Scene"); return r; }
    int64_t a = 0, b = 0;
    if (pairOf(kid(scene, 52), a, b) && a > 0 && b > 0 && a <= 4096 && b <= 4096) { r.width = (int)a; r.height = (int)b; }
    const MhegNode* items = kid(scene, 8);
    if (!items) { r.notes.push_back("the scene has no items"); return r; }
    for (const auto& it : items->kids) {
        if (it.cls != 2) continue;
        if (it.tag == 20) { r.links++; continue; }
        if (it.tag >= 9 && it.tag <= 11) { r.programs++; continue; }
        if (it.tag != 22 && it.tag != 25 && it.tag != 29) continue;   // Bitmap, Rectangle, Text
        if (!boolOf(kid(it, 56), true)) continue;                      // initially-active FALSE: not shown without an action
        MhegDrawOp op;
        op.object = objectNumber(it);
        int64_t w = 0, h = 0, x = 0, y = 0;
        if (!pairOf(kid(it, 76), w, h)) { r.notes.push_back("object " + std::to_string(op.object) + " has no box size"); continue; }
        pairOf(kid(it, 77), x, y);
        op.x = (int)std::max<int64_t>(-8192, std::min<int64_t>(x, 8192)); op.y = (int)std::max<int64_t>(-8192, std::min<int64_t>(y, 8192));
        op.w = (int)std::max<int64_t>(0, std::min<int64_t>(w, 8192)); op.h = (int)std::max<int64_t>(0, std::min<int64_t>(h, 8192));
        std::vector<uint8_t> inl;
        std::string ref;
        const bool haveContent = contentOf(kid(it, 58), inl, ref);
        if (it.tag == 25) {
            op.kind = MhegDrawOp::Rectangle;
            uint32_t c;
            if (colourOf(kid(it, 85), c)) op.fill = c;
            if (colourOf(kid(it, 84), c)) op.line = c;
            int64_t lw = 1;
            if (intOf(kid(it, 82), lw)) op.lineWidth = (int)std::max<int64_t>(0, std::min<int64_t>(lw, 64));
        } else if (it.tag == 29) {
            op.kind = MhegDrawOp::Text;
            uint32_t c;
            if (colourOf(kid(it, 41), c)) op.textColour = c;
            if (colourOf(kid(it, 39), c)) op.background = c;
            op.fontSize = fontSizeOf(kid(it, 43));
            int64_t j;
            if (intOf(kid(it, 87), j) && j >= 1 && j <= 4) op.hJust = (int)j;
            if (intOf(kid(it, 88), j) && j >= 1 && j <= 4) op.vJust = (int)j;
            op.wrap = boolOf(kid(it, 91), false);
            if (!ref.empty()) {
                op.ref = ref;
                std::vector<uint8_t> d;
                if (resolve && resolve(ref, d)) op.text = toText(d); else op.missing = true;
            } else if (haveContent) op.text = toText(inl);
        } else {
            op.kind = MhegDrawOp::Bitmap;
            if (!ref.empty()) {
                op.ref = ref;
                std::vector<uint8_t> d;
                if (resolve && resolve(ref, d)) { if (isImage(d)) op.image = std::move(d); else r.notes.push_back("bitmap " + ref + " is not PNG or JPEG (MPEG stills are not decoded)"); }
                else op.missing = true;
            } else if (isImage(inl)) op.image = std::move(inl);
            else if (haveContent) r.notes.push_back("an included bitmap is not PNG or JPEG");
        }
        r.ops.push_back(std::move(op));
    }
    return r;
}

const DsmccFile* mhegFindFile(const std::vector<DsmccFile>& files, const std::string& refIn) {
    std::string ref = refIn;
    for (const char* pre : {"DSM:", "dsm:", "CI:", "ci:", "rec:", "file:"}) if (ref.rfind(pre, 0) == 0) { ref = ref.substr(strlen(pre)); break; }
    while (!ref.empty() && (ref[0] == '/' || ref[0] == '~')) ref.erase(0, 1);
    if (ref.empty()) return nullptr;
    for (const auto& f : files) if (f.path == ref) return &f;
    for (const auto& f : files)   // the reference as the end of a path, on a directory boundary
        if (f.path.size() > ref.size() && f.path.compare(f.path.size() - ref.size(), ref.size(), ref) == 0 && f.path[f.path.size() - ref.size() - 1] == '/') return &f;
    const size_t s = ref.find_last_of('/');
    const std::string base = s == std::string::npos ? ref : ref.substr(s + 1);
    for (const auto& f : files) {   // the same file name anywhere
        const size_t fs = f.path.find_last_of('/');
        if ((fs == std::string::npos ? f.path : f.path.substr(fs + 1)) == base) return &f;
    }
    return nullptr;
}

} // namespace dect2
