// Elements for the satellite identification modes (see sat_elements.h).
#include "dect2/satmisc/sat_elements.h"
#include "dect2/cubesat/cubesat_db.h"
#include "dect2/geo/sat_plan.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace dect2 {
namespace satmisc {

std::vector<Tle> loadAllElements(const std::string& dir) {
    std::map<int, Tle> m;
    mergeNewestTle(m, parseTleText(cubesat::tleSnapshotText()));
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(dir), ec)) {
        const std::string ext = e.path().extension().string();
        if (ext != ".txt" && ext != ".tle") continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::stringstream ss; ss << in.rdbuf();
        mergeNewestTle(m, parseTleText(ss.str()));
    }
    std::vector<Tle> v;
    v.reserve(m.size());
    for (auto& kv : m) v.push_back(std::move(kv.second));
    return v;
}

} // namespace satmisc
} // namespace dect2
