// MHEG-5 BER structure parser: a synthetic Scene with nested constructed items, high tag numbers, long lengths, an indefinite-length item, and damaged input.
#include "dect2/common/png.h"
#include "dect2/tv/dsmcc.h"
#include "dect2/tv/mheg.h"
#include <algorithm>
using namespace dect2;
#include "../common/test_util.h"

static MhegNode prim(int cls, uint32_t tag, std::vector<uint8_t> v) { MhegNode n; n.cls = cls; n.tag = tag; n.value = std::move(v); return n; }
static MhegNode cons(int cls, uint32_t tag, std::vector<MhegNode> k) { MhegNode n; n.cls = cls; n.tag = tag; n.constructed = true; n.kids = std::move(k); return n; }


static uint32_t crc32b(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
static void be32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 3; i >= 0; i--) o.push_back((uint8_t)(v >> (8 * i))); }
static void chunk(std::vector<uint8_t>& o, const char* t, const std::vector<uint8_t>& d) {
    be32(o, (uint32_t)d.size());
    std::vector<uint8_t> td(t, t + 4); td.insert(td.end(), d.begin(), d.end());
    o.insert(o.end(), td.begin(), td.end());
    be32(o, crc32b(td.data(), td.size()));
}
// a w x h RGB PNG of one colour, uncompressed deflate blocks
static std::vector<uint8_t> makePng(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; y++) { raw.push_back(0); for (int x = 0; x < w; x++) { raw.push_back(r); raw.push_back(g); raw.push_back(b); } }
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    do {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n >= raw.size() ? 1 : 0);
        z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8)); z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, bb = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; bb = (bb + a) % 65521; }
    be32(z, (bb << 16) | a);
    std::vector<uint8_t> o = {0x89, 'P', 'N', 'G', 13, 10, 26, 10}, ihdr;
    be32(ihdr, (uint32_t)w); be32(ihdr, (uint32_t)h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(o, "IHDR", ihdr); chunk(o, "IDAT", z); chunk(o, "IEND", {});
    return o;
}
static std::vector<uint8_t> bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }
static MhegNode ref(int num) { return cons(0, 16, {prim(0, 4, bytes("~")), prim(0, 2, {(uint8_t)num})}); }   // ExternalReference { group-identifier, object-number }
static MhegNode ints2(uint32_t tag, int a, int b) { return cons(2, tag, {prim(0, 2, {(uint8_t)(a >> 8), (uint8_t)a}), prim(0, 2, {(uint8_t)(b >> 8), (uint8_t)b})}); }
static MhegNode colour(uint32_t tag, std::vector<uint8_t> rgba) { return cons(2, tag, {prim(0, 4, std::move(rgba))}); }

int main() {
    std::string txt = "Hello MHEG";
    MhegNode scene = cons(2, 1, {
        prim(2, 0, {1, 0x2C}),                                       // [0] 300
        cons(2, 3, {prim(0, 4, std::vector<uint8_t>(txt.begin(), txt.end())), prim(2, 40, {0xFF})}),   // a tag number above 30
        cons(2, 7, {prim(2, 5, std::vector<uint8_t>(300, 0x55))}),   // long-form length
    });
    auto enc = mhegEncode(scene);
    CHECK(mhegLooksLikeApplication(enc.data(), enc.size()), "scene recognised");
    auto r = mhegParse(enc.data(), enc.size());
    CHECK(r.error.empty() && r.roots.size() == 1 && r.consumed == enc.size(), "parse ok: %s", r.error.c_str());
    if (!r.roots.empty()) {
        const auto& s = r.roots[0];
        CHECK(s.name == "Scene" && s.kids.size() == 3, "scene with 3 children");
        CHECK(s.kids.size() == 3 && s.kids[1].kids.size() == 2 && s.kids[1].kids[1].tag == 40, "nested high tag");
        CHECK(s.kids.size() == 3 && s.kids[2].kids[0].value.size() == 300, "long length");
        CHECK(mhegEncode(s) == enc, "re-encode identical");
        std::string d = mhegDump(s);
        CHECK(d.find("Hello MHEG") != std::string::npos && d.find("[0] 300") != std::string::npos, "dump text:\n%s", d.c_str());
    }
    // an application of indefinite length
    std::vector<uint8_t> ind = {0xA0, 0x80, 0x82, 0x01, 0x05, 0x00, 0x00};
    auto ri = mhegParse(ind.data(), ind.size());
    CHECK(ri.error.empty() && ri.roots.size() == 1 && ri.roots[0].name == "Application" && ri.roots[0].kids.size() == 1, "indefinite: %s", ri.error.c_str());
    // damaged: truncated, deep nesting, junk
    auto cut = enc; cut.resize(cut.size() - 5);
    CHECK(!mhegParse(cut.data(), cut.size()).error.empty(), "truncated reported");
    std::vector<uint8_t> deep;
    for (int i = 0; i < 100; i++) { deep.push_back(0x30); deep.push_back(0x80); }
    CHECK(!mhegParse(deep.data(), deep.size()).error.empty(), "deep nesting refused");
    uint8_t junk[] = {0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
    CHECK(!mhegParse(junk, sizeof junk).error.empty() && !mhegLooksLikeApplication(junk, sizeof junk), "junk refused");
    // through a carousel: the file comes out of an object carousel (compressed, several blocks) and is parsed
    std::vector<DsmccTestFile> files = {{"a", enc}, {"junk", std::vector<uint8_t>(junk, junk + sizeof junk)}};
    auto secs = buildObjectCarousel(files, 1, 200, true);
    DsmccCarousel c;
    for (int round = 0; round < 2; round++) for (auto& s : secs) c.section(s.data(), (int)s.size());
    int found = 0;
    for (auto& f : c.files()) if (mhegLooksLikeApplication(f.data.data(), f.data.size())) { found++; CHECK(f.path == "a", "path %s", f.path.c_str()); }
    CHECK(found == 1, "one application file in the carousel, found %d", found);

    // an application and a scene with a Rectangle, a Text (inline), a Text (a file of the carousel) and a Bitmap (a PNG of the carousel): carousel -> files -> parse -> draw ops
    {
        MhegNode app = cons(2, 0, {ref(0), cons(2, 8, {cons(2, 20, {ref(9)})})});   // [8] items: one Link
        MhegNode rect = cons(2, 25, {ref(1), ints2(76, 100, 50), ints2(77, 10, 20), colour(85, {255, 0, 0, 0}), colour(84, {0, 0, 255}), prim(2, 82, {2})});
        MhegNode t1 = cons(2, 29, {ref(2), ints2(76, 300, 40), ints2(77, 5, 70), cons(2, 58, {prim(0, 4, bytes("Weather 25C"))}), colour(41, {0, 255, 0}), prim(2, 87, {3}), prim(2, 43, bytes("plain.28.32.0"))});
        MhegNode t2 = cons(2, 29, {ref(3), ints2(76, 300, 40), ints2(77, 5, 120), cons(2, 58, {cons(0, 16, {prim(0, 4, bytes("//texts/stock.txt"))})})});
        MhegNode bmp = cons(2, 22, {ref(4), ints2(76, 8, 8), ints2(77, 400, 100), cons(2, 58, {cons(0, 16, {prim(0, 4, bytes("~/img/p.png"))})}), prim(2, 80, {0})});
        MhegNode hidden = cons(2, 25, {ref(5), ints2(76, 10, 10), prim(2, 56, {0})});
        MhegNode prog = cons(2, 9, {ref(6), prim(2, 64, bytes("Fn"))});
        MhegNode sc = cons(2, 1, {ref(0), cons(2, 8, {rect, t1, t2, bmp, hidden, prog}), prim(2, 51, {15}), ints2(52, 720, 576)});
        auto appE = mhegEncode(app), scE = mhegEncode(sc);
        auto png = makePng(8, 8, 10, 200, 30);
        std::string stock = "ACME 12.30";
        auto secs2 = buildObjectCarousel({{"startup", appE}, {"scene1", scE}, {"img/p.png", png}, {"texts/stock.txt", bytes(stock)}}, 7, 150, true);
        DsmccCarousel c2;
        for (int round = 0; round < 2; round++) for (auto& s : secs2) c2.section(s.data(), (int)s.size());
        auto fl = c2.files();
        CHECK(fl.size() == 4, "4 files, got %zu", fl.size());
        const DsmccFile* sf = nullptr;
        const DsmccFile* af = nullptr;
        for (auto& f : fl) { if (f.path == "scene1") sf = &f; if (f.path == "startup") af = &f; }
        CHECK(sf && af && mhegLooksLikeApplication(af->data.data(), af->data.size()), "scene and application files");
        if (sf && af) {
            auto pa = mhegParse(af->data.data(), af->data.size());
            CHECK(pa.error.empty() && pa.roots.size() == 1 && pa.roots[0].name == "Application" && pa.roots[0].kids.size() == 2, "app parse");
            if (!pa.roots.empty() && pa.roots[0].kids.size() == 2) CHECK(pa.roots[0].kids[1].name == "items" && pa.roots[0].kids[1].kids[0].name == "Link", "app names");
            auto ps = mhegParse(sf->data.data(), sf->data.size());
            CHECK(ps.error.empty() && ps.roots.size() == 1 && ps.roots[0].name == "Scene", "scene parse");
            std::string d = mhegDump(ps.roots[0]);
            for (const char* w : {"items", "scene-coordinate-system", "Rectangle", "Text", "Bitmap", "original-box-size", "original-position", "original-content", "text-colour", "ResidentProgram", "original-ref-fill-colour"})
                CHECK(d.find(w) != std::string::npos, "dump names %s:\n%s", w, d.c_str());
            MhegResolve res = [&](const std::string& r, std::vector<uint8_t>& out) { const DsmccFile* f = mhegFindFile(fl, r); if (!f) return false; out = f->data; return true; };
            MhegRender rr = mhegRenderScene(ps.roots[0], res);
            CHECK(rr.width == 720 && rr.height == 576 && rr.links == 0 && rr.programs == 1, "canvas %dx%d programs %d", rr.width, rr.height, rr.programs);
            CHECK(rr.ops.size() == 4, "4 draw ops (the hidden one is skipped), got %zu", rr.ops.size());
            if (rr.ops.size() == 4) {
                const auto& o0 = rr.ops[0]; const auto& o1 = rr.ops[1]; const auto& o2 = rr.ops[2]; const auto& o3 = rr.ops[3];
                CHECK(o0.kind == MhegDrawOp::Rectangle && o0.x == 10 && o0.y == 20 && o0.w == 100 && o0.h == 50 && o0.fill == 0xFF0000FFu && o0.line == 0x0000FFFFu && o0.lineWidth == 2, "rectangle %d %d %d %d fill %08X", o0.x, o0.y, o0.w, o0.h, o0.fill);
                CHECK(o1.kind == MhegDrawOp::Text && o1.text == "Weather 25C" && o1.textColour == 0x00FF00FFu && o1.hJust == 3 && o1.fontSize == 28 && o1.x == 5 && o1.y == 70, "text '%s'", o1.text.c_str());
                CHECK(o2.kind == MhegDrawOp::Text && o2.text == stock && !o2.missing, "text from a file '%s'", o2.text.c_str());
                CHECK(o3.kind == MhegDrawOp::Bitmap && o3.x == 400 && o3.y == 100 && !o3.image.empty() && !o3.missing, "bitmap");
                int w = 0, h = 0; std::vector<uint8_t> px;
                CHECK(decodePng(o3.image.data(), o3.image.size(), w, h, px) && w == 8 && h == 8 && px[0] == 10 && px[1] == 200 && px[2] == 30, "png decodes to the colour");
            }
            // a reference to a file that is not there is marked missing
            MhegRender r2 = mhegRenderScene(ps.roots[0], [](const std::string&, std::vector<uint8_t>&) { return false; });
            CHECK(r2.ops.size() == 4 && r2.ops[2].missing && r2.ops[3].missing && r2.ops[3].image.empty(), "missing content marked");
            // damaged scene data does not crash the renderer
            std::vector<uint8_t> bad = sf->data;
            for (size_t i = 8; i < bad.size(); i += 7) bad[i] ^= 0x5A;
            auto pb = mhegParse(bad.data(), bad.size());
            for (auto& rt : pb.roots) mhegRenderScene(rt, res);
        }
    }
    return testSummary("mheg");
}
