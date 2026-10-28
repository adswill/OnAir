// Synthetic MHEG-5 test material (stage 2 of docs/MHEG-REDBUTTON.md). NOT a broadcast: no real UK stream or BBC application is used or copied.
// Builds a small interactive application written from the ASN.1 module of ISO/IEC 13522-5 (BER, UK profile style): an Application "a" that starts
// Scene "s1"; the scene shows a Rectangle, a Text, a Bitmap (PNG file of the carousel), an Integer variable "presses" and Links that react to keys:
//   red (100)    -> presses := 1, the Text "Red pressed" (object 6) is activated, the prompt text (object 2) is deactivated
//   green (101)  -> SetTimer on the scene (id 1, 500 ms); when the timer fires (TimerFired id 1) the Text 7 "Timer fired" is activated
//   yellow (102) -> TransitionTo scene "s2"  (scene s2 has a Text and a link on blue (103) -> TransitionTo s1)
//   blue (103)   -> in s1: Quit
// Object numbers are fixed so tests can address them.
#pragma once
#include "dect2/tv/dsmcc.h"
#include "dect2/tv/mheg.h"
#include <cstdint>
#include <string>
#include <vector>

namespace mhegtest {
using dect2::MhegNode;

inline std::vector<uint8_t> bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }
inline MhegNode prim(int cls, uint32_t tag, std::vector<uint8_t> v) { MhegNode n; n.cls = cls; n.tag = tag; n.value = std::move(v); return n; }
inline MhegNode cons(int cls, uint32_t tag, std::vector<MhegNode> k) { MhegNode n; n.cls = cls; n.tag = tag; n.constructed = true; n.kids = std::move(k); return n; }
inline std::vector<uint8_t> intBytes(int64_t v) {
    std::vector<uint8_t> o;
    for (int i = 7; i >= 0; i--) o.push_back((uint8_t)(v >> (8 * i)));
    while (o.size() > 1 && ((o[0] == 0 && !(o[1] & 0x80)) || (o[0] == 0xFF && (o[1] & 0x80)))) o.erase(o.begin());
    return o;
}
inline MhegNode integer(int64_t v) { return prim(0, 2, intBytes(v)); }              // universal INTEGER
inline MhegNode enumv(int v) { return prim(0, 10, intBytes(v)); }                    // universal ENUMERATED
inline MhegNode ctxInt(uint32_t tag, int64_t v) { return prim(2, tag, intBytes(v)); }
inline MhegNode ctxBool(uint32_t tag, bool v) { return prim(2, tag, {(uint8_t)(v ? 0xFF : 0)}); }
inline MhegNode octets(const std::string& s) { return prim(0, 4, bytes(s)); }
inline MhegNode ext(const std::string& group, int num) { return cons(0, 16, {octets(group), integer(num)}); }   // ExternalReference
inline MhegNode ints2(uint32_t tag, int a, int b) { return cons(2, tag, {integer(a), integer(b)}); }
inline MhegNode colour(uint32_t tag, std::vector<uint8_t> rgba) { return cons(2, tag, {prim(0, 4, std::move(rgba))}); }
inline MhegNode inl(const std::string& s) { return cons(2, 58, {octets(s)}); }                                    // included-content
inline MhegNode refc(const std::string& s) { return cons(2, 58, {cons(0, 16, {octets(s)})}); }                    // referenced-content

// elementary actions (tags from the ASN.1 module); targets are internal object numbers (INTEGER) or an external reference
inline MhegNode act(uint32_t tag, const MhegNode& target) { return cons(2, tag, {target}); }
inline MhegNode setVarInt(int obj, int64_t v) { return cons(2, 212, {integer(obj), cons(2, 226, {integer(v)})}); }
inline MhegNode transitionTo(const std::string& group) { return cons(2, 222, {ext(group, 0), prim(0, 5, {})}); }
inline MhegNode setTimer(int sceneObj, int id, int ms) { return cons(2, 210, {integer(sceneObj), integer(id), cons(0, 16, {integer(ms)})}); }

// a Link: event source, EventType, optional integer event data, then the actions
inline MhegNode link(int num, const std::string& group, const MhegNode& source, int eventType, int64_t data, bool hasData, std::vector<MhegNode> effect) {
    std::vector<MhegNode> cond = {source, enumv(eventType)};
    if (hasData) cond.push_back(integer(data));
    return cons(2, 20, {ext(group, num), cons(2, 62, cond), cons(2, 63, std::move(effect))});
}
enum { kIsRunning = 4, kUserInput = 6, kTimerFired = 8 };

struct TestApp {
    std::vector<dect2::DsmccTestFile> files;   // "a", "s1", "s2", "img/p.png"
};

inline TestApp makeTestApp(const std::vector<uint8_t>& png) {
    TestApp t;
    // Application "a": starts Scene s1
    MhegNode app = cons(2, 0, {ext("a", 0), cons(2, 5, {transitionTo("s1")})});
    // Scene s1
    const std::string g = "s1";
    MhegNode bg   = cons(2, 25, {ext(g, 1), ints2(76, 300, 100), ints2(77, 60, 60), colour(85, {0, 0, 160, 0}), colour(84, {255, 255, 255, 0}), ctxInt(82, 2)});
    MhegNode txt  = cons(2, 29, {ext(g, 2), ints2(76, 280, 40), ints2(77, 70, 70), inl("Press red"), colour(41, {255, 255, 255, 0}), ctxInt(87, 3)});
    MhegNode bmp  = cons(2, 22, {ext(g, 3), ints2(76, 32, 32), ints2(77, 400, 200), refc("img/p.png"), ctxInt(80, 0)});
    MhegNode var  = cons(2, 16, {ext(g, 4), cons(2, 67, {integer(0)})});
    MhegNode red  = cons(2, 29, {ext(g, 6), ints2(76, 280, 40), ints2(77, 70, 110), inl("Red pressed"), colour(41, {255, 255, 0, 0}), ctxBool(56, false)});
    MhegNode tim  = cons(2, 29, {ext(g, 7), ints2(76, 280, 40), ints2(77, 70, 150), inl("Timer fired"), colour(41, {0, 255, 0, 0}), ctxBool(56, false)});
    MhegNode lRed = link(5, g, integer(0), kUserInput, 100, true, {setVarInt(4, 1), act(115, integer(6)), act(125, integer(2))});
    MhegNode lGrn = link(8, g, integer(0), kUserInput, 101, true, {setTimer(0, 1, 500)});
    MhegNode lTim = link(9, g, integer(0), kTimerFired, 1, true, {act(115, integer(7))});
    MhegNode lYel = link(10, g, integer(0), kUserInput, 102, true, {transitionTo("s2")});
    MhegNode lBlu = link(11, g, integer(0), kUserInput, 103, true, {act(176, ext("a", 0))});   // Quit the application
    t.files.push_back({"a", dect2::mhegEncode(app)});
    t.files.push_back({"s1", dect2::mhegEncode(cons(2, 1, {ext(g, 0), cons(2, 8, {bg, txt, bmp, var, lRed, red, tim, lGrn, lTim, lYel, lBlu}), ctxInt(51, 4), ints2(52, 720, 576)}))});
    // Scene s2: a text and a link back on blue
    const std::string g2 = "s2";
    MhegNode t2 = cons(2, 29, {ext(g2, 1), ints2(76, 280, 40), ints2(77, 70, 70), inl("Second scene"), colour(41, {255, 255, 255, 0})});
    MhegNode back = link(2, g2, integer(0), kUserInput, 103, true, {transitionTo("s1")});
    t.files.push_back({"s2", dect2::mhegEncode(cons(2, 1, {ext(g2, 0), cons(2, 8, {t2, back}), ctxInt(51, 4), ints2(52, 720, 576)}))});
    t.files.push_back({"img/p.png", png});
    return t;
}
} // namespace mhegtest
