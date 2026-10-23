#include "adapters_internal.h"

namespace dect2 {

std::unique_ptr<TvAdapter> makeTvAdapter(int stdMode) {
    switch (stdMode) {
    case 1: return makeDvbtAdapter(false);
    case 2: return makeDvbtAdapter(true);
    case 8: return makeDvbsAdapter();
    case 42: return makeDvbcAdapter();
    case 3: case 5: case 6: case 9: return makeRasterAdapter(stdMode);
    default: return nullptr;
    }
}

bool spanTvRaster(int stdMode) { auto a = makeTvAdapter(stdMode); return a && a->raster(); }
const char* spanTvName(int stdMode) { auto a = makeTvAdapter(stdMode); return a ? a->name() : ""; }

} // namespace dect2
