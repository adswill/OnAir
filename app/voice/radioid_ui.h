// The radio-ID name list of DMR and NXDN (dect2/common/radioid_db.h): where the user downloads or imports it, shown in the mode's
// Receiver tab, and the loading of the saved file at start. docs/RADIOID.md.
#pragma once
#include "dect2/common/radioid_db.h"
#include <string>

namespace radioidui {

void tick(dect2::RadioIdKind k);              // every frame while the mode is shown: loads the saved file once, collects a finished download
void section(dect2::RadioIdKind k);           // the "Names from a radio ID list" block (collapsed)
// "1234567" -> "1234567  W1ABC Joe Smith" (the id alone when it is not in the list)
std::string withName(dect2::RadioIdKind k, const std::string& idText, uint32_t id);

}
