// Callsign lookup without the network: callsign prefix -> DXCC entity, continent, CQ and ITU zone, position. The built-in table is written
// for OnAir from the public ITU prefix allocations (a subset of about 200 of the 340 entities: every country with a regular amateur
// population); a full country file in the common cty.dat format (BigCTY, as contest loggers use) can be loaded from a file the user has.
// Nothing is downloaded.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace dect2::ham {

struct DxccEntity {
    int adif = 0;                 // the ADIF DXCC entity number; 0 when not known (a user file without the numbers)
    std::string name, cont, primary;
    int cqz = 0, ituz = 0;
    double lat = 0, lon = 0;      // degrees, east positive
    double tz = 0;                // hours from UTC as in the file (cty.dat: west positive, so as read, not flipped)
    bool wae = false;             // a WAE-only entity ('*' in cty.dat): shown, but it is not a DXCC entity (key() is empty)
};
struct DxccHit {
    const DxccEntity* e = nullptr;   // nullptr: not found (or /MM, /AM: no entity)
    int cqz = 0, ituz = 0;           // the zone of this prefix (some prefixes override the entity's)
    std::string prefix;              // the matching prefix, "=CALL" for an exact-call entry
    std::string cont;                // the continent of this prefix (an override {EU} wins over the entity's)
    double lat = 0, lon = 0;         // the position of this prefix (an override <lat/lon> wins over the entity's); degrees, east positive
    std::string key() const { return !e || e->wae ? "" : e->adif > 0 ? std::to_string(e->adif) : e->name; }   // the DXCC award key (ham_log.h awardKey)
};

class DxccDb {
public:
    static const DxccDb& builtin();
    // cty.dat: "Name: CQ: ITU: Cont: Lat: Lon(west +): UTC: Prefix:" then prefixes separated by commas, ending with ';'.
    // Prefix forms: KH6, =W1AW (exact call), (5) CQ zone, [8] ITU zone, <lat/lon> position, {EU} continent, ~tz~ overrides. Also read here:
    // "?" as any letter (R?9 = RA9 .. RZ9, at most two per prefix). cty.dat, wl_cty.dat (Win-Test) and cty_wt_mod.dat share this layout;
    // CRLF, a BOM and blank lines are fine. A VERyyyymmdd entry (the file's version stamp) is kept in version(). Text over 32 MB is refused.
    bool loadCty(const std::string& text, std::string& err);
    DxccHit lookup(const std::string& call) const;
    const std::vector<DxccEntity>& entities() const { return ents_; }
    const DxccEntity* byAdif(int adif) const;
    size_t prefixCount() const { return pre_.size() + exact_.size(); }
    const std::string& version() const { return version_; }   // "20240301" from the VER entry, "" when the file has none
private:
    struct Rule { int ent = -1, cqz = 0, ituz = 0; bool hasPos = false; double lat = 0, lon = 0; std::string cont; };
    void fill(DxccHit& h, const Rule& r) const;
    std::string version_;
    std::vector<DxccEntity> ents_;
    std::map<std::string, Rule> pre_, exact_;
    size_t maxLen_ = 0;
};

// the part of a callsign that decides the entity: "DL/LY1ABC" -> "DL", "LY1ABC/P" -> "LY1ABC", "LY1ABC/DL" -> "DL", "K1ABC/4" -> "K4";
// "" for /MM and /AM (maritime and aeronautical mobile count for no entity)
std::string prefixPart(const std::string& call);

}  // namespace dect2::ham
