// The adapters of whole-span TV, one file each (core/src/spantv/adapter_*.cpp); makeTvAdapter() in adapters.cpp picks one by standard.
#pragma once
#include "dect2/engine/spantv/adapter.h"
#include <memory>

namespace dect2 {

std::unique_ptr<TvAdapter> makeDvbtAdapter(bool dvbt);   // DVB-T2 (false) or DVB-T (true)
std::unique_ptr<TvAdapter> makeDvbsAdapter();            // DVB-S / S2 / S2X
std::unique_ptr<TvAdapter> makeDvbcAdapter();            // DVB-C (free frequency)
std::unique_ptr<TvAdapter> makeRasterAdapter(int stdMode);   // ATSC 1.0 (3), ATSC 3.0 (5), ISDB-T (6), DTMB (9)

} // namespace dect2
