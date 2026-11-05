// The talkgroup directory (dect2/common/tg_dir.h) in the voice and trunking panels: the saved file is read at the start, "Talkgroup names" is the
// editor (add, edit, delete, search, import and export CSV, RadioReference style too), and withName() puts a name beside a talkgroup number.
// docs/TALKGROUPS.md.
#pragma once
#include "dect2/common/tg_dir.h"
#include <string>

namespace tgdirui {

void tick();                                   // loads <dataDir>/talkgroups.csv once; saves it after an edit (call every frame, cheap)
void section(dect2::TgKind k);                 // the "Talkgroup names" block (collapsed): list, edit, import, export
// "TG 91" -> "TG 91  World-wide" (the text alone when the directory has no name for it)
std::string withName(dect2::TgKind k, const std::string& text, uint32_t id);
std::string nameOf(dect2::TgKind k, uint32_t id);   // just the name or ""

}  // namespace tgdirui
