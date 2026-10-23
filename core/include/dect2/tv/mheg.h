// MHEG-5 (ISO/IEC 13522-5) application files as carried in a DSM-CC object carousel (UK/Ireland/NZ profile ETSI ES 202 184): the files are ASN.1 BER.
// This parses the BER structure into a tree (class, tag, constructed/primitive, value) that a viewer can list. The names of the group, item-class and common
// attribute tags come from the ASN.1 module ISO13522-MHEG-5 (ITU-T T.172 02/1998) as published in the RedButton MHEG-5 engine (redbutton-browser/ISO13522-MHEG-5.asn,
// GPL-2.0-or-later); everything not in that text is shown by number. Read-only and bounded (depth and node count); nothing is executed: there are no scripts and no
// links or actions. A scene can be turned into draw operations for a static preview (mhegRenderScene).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "dect2/tv/dsmcc.h"

namespace dect2 {

struct MhegNode {
    int cls = 0;                  // 0 universal, 1 application, 2 context-specific, 3 private
    uint32_t tag = 0;
    bool constructed = false;
    bool indefinite = false;
    std::string name;             // "Application", "Scene", "INTEGER", "[3]" ...
    std::vector<uint8_t> value;   // primitive content
    std::vector<MhegNode> kids;   // constructed content
};

struct MhegParse {
    std::vector<MhegNode> roots;
    size_t consumed = 0;
    size_t nodes = 0;
    std::string error;            // empty when the whole input parsed
};

MhegParse mhegParse(const uint8_t* p, size_t n);
bool mhegLooksLikeApplication(const uint8_t* p, size_t n);   // starts with a constructed context tag [0] Application or [1] Scene whose length fits the data
std::string mhegDump(const MhegNode& n, int maxDepth = 12);  // indented object tree, one line per node
std::vector<uint8_t> mhegEncode(const MhegNode& n);          // definite-length BER, for test signals

// ---- static preview of a Scene: the visible items (Rectangle, Text, Bitmap) as draw operations in scene coordinates (default 720x576), in item order (back to front).
// Links, actions, programs and the input of the user are not run or followed ("interactive links not followed"). Items with initially-active FALSE are not drawn.
// Colours are 0xRRGGBBAA. Absolute colours are read as R,G,B and an optional fourth byte that is the transparency (255 = invisible, UK profile); colour indexes need a palette
// that is not resolved, so those items get a neutral default.
struct MhegDrawOp {
    enum Kind { Rectangle, Text, Bitmap } kind = Rectangle;
    int object = 0;                 // object number in the group
    int x = 0, y = 0, w = 0, h = 0;
    uint32_t fill = 0, line = 0;    // Rectangle (alpha 0 = none)
    int lineWidth = 1;
    std::string text;               // Text, UTF-8 (inline content, or the referenced file)
    uint32_t textColour = 0xFFFFFFFFu, background = 0;
    int hJust = 1, vJust = 1;       // 1 start, 2 end, 3 centre, 4 justified
    bool wrap = false;
    int fontSize = 0;               // pixels from a textual font-attributes value like "plain.28.32.0", 0 when not given
    std::string ref;                // referenced content (Text, Bitmap)
    std::vector<uint8_t> image;     // Bitmap: the bytes of the picture (PNG or JPEG, decoded by the caller); empty when not found or not a picture
    bool missing = false;           // the referenced content is not in the carousel (yet)
};
struct MhegRender {
    int width = 720, height = 576;
    std::vector<MhegDrawOp> ops;
    int links = 0, programs = 0;    // counted, never followed or run
    std::vector<std::string> notes;
};
using MhegResolve = std::function<bool(const std::string& ref, std::vector<uint8_t>& out)>;
MhegRender mhegRenderScene(const MhegNode& scene, const MhegResolve& resolve);
// the file of a carousel that a content reference names ("~/a/b.png", "//a/b.png", "DSM:a/b.png", "b.png"): exact path first, then the path as a suffix
const DsmccFile* mhegFindFile(const std::vector<DsmccFile>& files, const std::string& ref);

} // namespace dect2
