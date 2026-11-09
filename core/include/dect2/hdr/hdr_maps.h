// HD Radio traffic and weather maps. iHeart TTN stations send the traffic map as nine 200 x 200 tiles (LOT files TMT_*_x_y_YYYYMMDD_HHMM.png) and a radar overlay (DWRO_id_*_YYYYMMDD_HHMM.png) with
// its corner coordinates in DWRI_ text. This puts the tiles together and reads the overlay's position. File name conventions as in nrsc5-gui. No base map is shipped: the overlay is drawn on a plain background.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace hdr {

struct MapImage { int w = 0, h = 0; std::vector<uint8_t> rgba; std::string stamp; };

// Parses names; false when the name is not of that kind. x, y are 0..2.
bool parseTrafficName(const std::string& name, int& x, int& y, std::string& stamp);
bool parseRadarName(const std::string& name, std::string& id, std::string& stamp);
// The text of a DWRI_ file: the area id and the corner coordinates (north, west, south, east)
bool parseRadarInfo(const std::string& text, std::string& id, double corners[4]);

class TrafficMap {
public:
    // A decoded tile; returns true once the nine tiles of one timestamp are in, then image() is that map.
    bool addTile(int x, int y, const std::string& stamp, const uint8_t* rgba, int w, int h);
    bool ready() const { return ready_; }
    const MapImage& image() const { return out_; }
    int tilesOfCurrent() const;
private:
    struct Tile { bool have = false; std::vector<uint8_t> px; };
    std::map<std::string, std::vector<Tile>> sets_;
    MapImage out_;
    bool ready_ = false;
};

struct RadarInfo { bool valid = false; std::string id; double corners[4] = {}; };
// The radar overlay on a neutral background, sized as the overlay. Pixels with alpha 0 show the background.
void radarComposite(const uint8_t* rgba, int w, int h, MapImage& out);

}  // namespace hdr
}  // namespace dect2
