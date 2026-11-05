// The unknown signal analyser of the Sub-GHz sensors mode: from the pulse and gap widths of a package that no decoder took
// it guesses the modulation, slices the bits with rtl_433's own slicers and writes a flex decoder definition for the result.
#pragma once
#include "dect2/sensors/sensors_tel.h"

extern "C" {
#include "pulse_data.h"
}

namespace dect2 { namespace sensors {

void analyse(const pulse_data_t* p, bool fsk, SensorUnknown& out);

} } // namespace dect2::sensors
