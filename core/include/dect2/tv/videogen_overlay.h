// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Digital video generator: the overlay laid on the picture before it is encoded (a logo, parameter blocks, free text, rolling
// text), the magnetic snapping the editor uses, and the renderer. The same renderer draws the editor's preview and the encoded
// picture, so what is placed is what is sent.
#pragma once
#include "videogen_params.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dect2::vgen {

enum class BlockKind { Logo, Param, Text, Rolling };

struct Block {
    BlockKind kind = BlockKind::Text;
    Param param = Param::VideoCodec;   // Param: which parameter
    std::string text;                  // Text / Rolling: the text; Param: the label in front of the value; Logo: the picture file (PNG; other formats when FFmpeg decodes them)
    float x = 0.05f, y = 0.05f;        // top-left corner as a fraction of the picture width / height (the layout survives a change of resolution)
    float w = 0.2f, h = 0.15f;         // Logo: its box; Rolling: the width of the band (h follows the font); text blocks size to their text
    float fontPx = 24;                 // text height in pixels of the encoded picture
    uint32_t color = 0xFFFFFFFFu;      // text colour, 0xAABBGGRR (ImGui's order)
    bool background = true;            // a half-transparent dark box behind the text
    int direction = 0;                 // Rolling: 0 left to right, 1 right to left
    float charsPerSec = 1;             // Rolling: speed in characters per second
};

struct Layout {
    std::vector<Block> blocks;
};

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

// "key=value" lines, one block after a "[block]" line
std::string saveLayout(const Layout& l);
Layout loadLayout(const std::string& text);

// Magnetic snapping in picture pixels: r moved so an edge or the centre lands on an edge of another block (side by side or aligned), of
// the picture or on its centre line, when it is within `dist`. `others` are the other blocks' rectangles.
Rect snapRect(Rect r, const std::vector<Rect>& others, float W, float H, float dist);

// Draws a layout into an RGBA picture. One per thread (it keeps glyph and logo caches).
class OverlayRenderer {
public:
    OverlayRenderer();
    ~OverlayRenderer();
    OverlayRenderer(const OverlayRenderer&) = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;

    // A TrueType font (the bytes are kept by the shared pointer). False when it is not one; text is then not drawn.
    bool setFont(std::shared_ptr<const std::vector<uint8_t>> ttf);
    bool hasFont() const;
    // The text a block shows (Param: "label: value")
    static std::string blockText(const Block& b, const Settings& s);
    // The block's rectangle in pixels of a W x H picture
    Rect blockRect(const Block& b, const Settings& s, int W, int H);
    // Clear rgba (W x H x 4, straight alpha) and draw every block; t is the time in seconds (rolling text)
    void render(const Layout& l, const Settings& s, double t, int W, int H, std::vector<uint8_t>& rgba);
    float textWidth(const std::string& utf8, float px);

    // Lay an RGBA picture over a YUV 4:2:0 frame (BT.601, limited range)
    static void blendYuv420(const uint8_t* rgba, int W, int H, uint8_t* y, int ys, uint8_t* u, int us, uint8_t* v, int vs);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::vgen
