// Auto-record in the interface (docs/MODE-EXPERIENCE.md, 3.2): the master switch (OFF by default), the "Record rules" editor and the one-click
// "Record this" of a row. The rules themselves live in core/include/dect2/session/record_rules.h.
#pragma once
#include "dect2/session/session.h"
#include <cstdint>

namespace kit {

// A line for the Items tab: [ ] Record (master switch) and a Rules... button. Opens the editor.
void recordBar(uint16_t mode);
// "Record this talkgroup" for a row: adds a rule (and turns the master switch on) in one click. Returns true when it did.
bool recordThisButton(const dect2::sess::Session& s);
// The editor window; call once per frame from the place that called recordBar (it only draws while open).
void recordRulesDialog(uint16_t mode);

}  // namespace kit
