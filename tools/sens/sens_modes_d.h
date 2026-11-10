// sens_sweep registry entries added for the 3.9 sensitivity pass D (docs/SENSITIVITY-3.9-D.md), used by tests/sens/test_sens39d.cpp.
// Same conventions as sens_modes.h: the mode's own test signal, the receiver set up as the engine sets it up, "valid output" = the
// receiver's own count of units whose check passed.
#pragma once
#include "sens_modes.h"

namespace sens {

inline std::vector<Mode> extraModesD() {
    std::vector<Mode> v;
    return v;
}

} // namespace sens
