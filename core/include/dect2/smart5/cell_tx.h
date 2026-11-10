// Transmitter from cell ID (3.9, docs/SMART5-tv-3.9.md item 14). DVB-T carries cell_id in the TPS and DVB-T2 in L1-post; the NIT
// may describe each cell with a cell_list_descriptor (tag 0x6C, EN 300 468 clause 6.2.7: position and extent of the cell and its
// subcells). This joins the received cell_id with that list and with the user's own book (cell ID -> site name), and gives the
// transmitter's name, or the cell's centre with distance and bearing from the user.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

struct NitCell {
    uint16_t cellId = 0;
    int cellIdExtension = -1;   // -1 for the cell itself, 0 - 255 for a subcell
    double latDeg = 0, lonDeg = 0;
    double extentLatDeg = 0, extentLonDeg = 0;
};

// Payload of a cell_list_descriptor (after tag and length). Returns cells and subcells.
std::vector<NitCell> parseCellList(const uint8_t* p, size_t len);

struct CellAnswer { bool known = false; std::string name; double latDeg = 0, lonDeg = 0, distKm = -1, bearingDeg = -1; bool fromNit = false; };

class CellTxBook {
public:
    void addNit(const std::vector<NitCell>& cells) { for (auto& c : cells) nit_[key(c.cellId, c.cellIdExtension)] = c; }
    void name(uint16_t cellId, const std::string& site) { user_[cellId] = site; }
    void setUser(double latDeg, double lonDeg) { ulat_ = latDeg; ulon_ = lonDeg; haveUser_ = true; }
    CellAnswer lookup(uint16_t cellId, int ext = -1) const;
    std::string text(uint16_t cellId, int ext = -1) const;
    std::string save() const;           // the user's book: "cell_id<TAB>name" lines
    void load(const std::string& s);
private:
    static uint32_t key(uint16_t id, int ext) { return (uint32_t)id << 9 | (uint32_t)(ext + 1); }
    std::map<uint32_t, NitCell> nit_;
    std::map<uint16_t, std::string> user_;
    double ulat_ = 0, ulon_ = 0; bool haveUser_ = false;
};

} // namespace smart5
} // namespace dect2
