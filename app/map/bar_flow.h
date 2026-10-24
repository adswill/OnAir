// Wrapping for the button line laid over a map: pure maths, so the layout can be tested without ImGui.
#pragma once

namespace barflow {

// true when an item `w` wide, put at `x` (`left` is the start of the line), would run past `right` and so goes on the next line.
// The first item of a line never wraps, however wide.
inline bool wraps(float x, float w, float left, float right) { return x > left + 0.5f && x + w > right; }

// Where the next item goes: x on the current line (after `gap`), or `left` on the next one.
// `x` is the right edge of the last item; the result is {x, newLine}.
struct Spot { float x; bool newLine; };
inline Spot next(float lastRight, float w, float gap, float left, float right) {
    const float x = lastRight + gap;
    if (wraps(x, w, left, right)) return {left, true};
    return {x, false};
}

}  // namespace barflow
