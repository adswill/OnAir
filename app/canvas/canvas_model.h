// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Workspace layout (View > Layout > Workspace, docs/WORKSPACE-4.0.md): a blank canvas the user fills with blocks. This is the
// pure model, no ImGui and no App: the blocks, their links to a radio source, tab groups, snapping, the text form for the file,
// named workspaces and the shipped templates. canvas_ui.cpp draws it with the existing panels. Unit tested (tests/app/test_canvas.cpp).
//
// Coordinates are in unscaled points (multiplied by the interface scale when drawn), relative to the top left of the canvas.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace canvas {

enum class Kind {
    Source, Spectrum, Demod, Audio, Decoder, Map, Tracker, Rotator, Transmitter, Recorder, Log, History, Frequencies, Bookmarks,
    Scanner, Clock, Notes, Count
};

struct KindInfo {
    Kind kind;
    const char* key;     // in the file: never change it
    const char* name;    // in the Add block menu and the title bar (translated where drawn)
    int group;           // index into kGroupNames
    const char* tip;
    bool needsSource;    // linked to a Source block (its picker in the block)
    bool hosted;         // an existing tool window placed on the canvas (it draws its own title bar and close button)
    float w, h;          // default size
};
constexpr int kNumGroups = 5;
extern const char* const kGroupNames[kNumGroups];
const KindInfo& info(Kind k);
const KindInfo* infoByKey(const std::string& key);

// The mode a source runs when no decoder block uses it (the General receiver: spectrum, waterfall and channel blocks keep working)
extern const char* const kIdleMode;

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool operator!=(const Rect& o) const { return !(*this == o); }
};

struct Block {
    int id = 0;
    Kind kind = Kind::Notes;
    int source = 0;        // id of the Source block it uses, 0: none
    Rect r;
    bool collapsed = false;
    bool paused = false;   // a decoder block: its source runs the idle mode while it is paused
    int tab = 0;           // tab group, 0: a window of its own
    std::map<std::string, std::string> cfg;   // the block's own settings (a source's receiver and radio settings, a decoder's mode, notes ...)
    std::string get(const std::string& k, const std::string& def = "") const;
    int getI(const std::string& k, int def = 0) const;
    double getD(const std::string& k, double def = 0) const;
    void set(const std::string& k, const std::string& v) { cfg[k] = v; }
    void setI(const std::string& k, int v) { cfg[k] = std::to_string(v); }
    void setD(const std::string& k, double v);
};

struct Workspace {
    std::string name = "Workspace";
    std::vector<Block> blocks;
    int nextId = 1;
    float grid = 16;       // 0: no grid
    bool snap = true;      // snap to the grid and to the other blocks' edges
    bool links = true;     // draw the links from each source to its blocks
    std::map<int, int> activeTab;   // tab group -> the block shown

    Block* find(int id);
    const Block* find(int id) const;
    // a new block at (x, y) with its default size; a block that needs a source is linked to the last source block added. Returns its id.
    int add(Kind k, float x, float y);
    // removes the block; blocks that used it as their source keep running unlinked (source 0) until another is picked.
    // `unlinked` (optional) gets their ids. False when there is no such block.
    bool remove(int id, std::vector<int>* unlinked = nullptr);
    bool link(int id, int sourceId);              // sourceId: a Source block, or 0 to unlink. False when either is wrong
    std::vector<int> sources() const;             // the Source blocks, in order
    std::vector<int> usersOf(int sourceId) const; // the blocks linked to it
    bool sourceMissing(int id) const;             // the block needs a source and has none
    void fixLinks();                              // after loading: links to missing or non-source blocks dropped, duplicate receivers cleared, one-member tab groups dissolved, nextId past every id

    // tab groups: a block dropped on another joins its group (made when there is none) and is shown; it takes the group's rectangle
    bool joinTab(int id, int target);
    bool leaveTab(int id);                        // back to a window of its own, a little below and right of the group
    std::vector<int> tabMembers(int group) const;
    bool showTab(int id);
    bool shown(int id) const;                     // a window of its own, or the shown tab of its group
    bool renders(int id) const;                   // shown and not collapsed: only then is its body drawn

    int decoderOwner(int sourceId) const;         // the first decoder block on that source that is not paused, 0: none
    int receiverOf(int sourceId) const;           // the receiver number a Source block drives (cfg "rx"), 0: none yet
};

// The rectangle a moved (or resized) block settles at: its edges snap to the edges of the other shown blocks within `threshold`, otherwise
// to the grid. Resizing moves only the right and bottom edges.
Rect snapRect(const Workspace& w, int id, Rect r, bool resizing, float threshold);

// The receiver a new Source block takes: the lowest open receiver no Source block drives; else the lowest free number (create = true);
// 0 when every receiver up to maxRx is taken.
int pickReceiver(const Workspace& w, const std::vector<int>& openRx, int maxRx, bool& create);

// What the source's mode does this frame. lastOwner: the decoder block that owned the source last frame (-1: not known yet, after a load)
struct ModeStep { enum Kind { None, Select, Idle } kind = None; std::string mode; };
ModeStep modeStep(int lastOwner, int owner, const std::string& ownerMode, const std::string& rxMode);

// The text form (one workspace: "onair-workspace 1" ... "end"; several in one file). Unknown lines and block kinds are skipped.
std::string serialise(const Workspace& w);
bool parse(const std::string& text, Workspace& out);
std::vector<Workspace> parseMany(const std::string& text);
std::string writeMany(const std::vector<Workspace>& v);
std::string uniqueName(const std::vector<Workspace>& v, const std::string& base);   // base, "base (2)", "base (3)" ...

// Templates shipped with OnAir: 0 Blank, 1 Listener, 2 Aircraft, 3 Satellite station
constexpr int kNumTemplates = 4;
extern const char* const kTemplateNames[kNumTemplates];
Workspace makeTemplate(int i);

std::string escape(const std::string& s);     // ' ' '\n' '\t' '\r' '\\' -> \s \n \t \r \\ (one token per field)
std::string unescape(const std::string& s);

}  // namespace canvas
