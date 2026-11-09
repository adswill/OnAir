// FM DX log: which stations are normally heard, and an alert with PI proof when a foreign one appears (a tropospheric or sporadic-E opening).
// Pure data and rules; the heard table comes from the census, the band scanner and the station being listened to.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

// Country of a PI code (the first nibble) with the ECC (group 1A): ISO code in capitals ("LT"), "" when not known. Table: IEC 62106 Annex D, ITU region 1.
std::string rdsCountry(int pi, int ecc);
// Without the ECC the nibble is ambiguous: every country that shares it, in table order.
std::vector<std::string> rdsCountriesLikely(int pi);

struct FmDxHeard {
    double mhz = 0;
    int pi = 0, ecc = -1;
    std::string ps;
    int64_t first = 0, last = 0; // Unix seconds
    uint32_t count = 0;
    float bestDb = -200;
};

struct FmDxAlert {
    bool fire = false;
    double mhz = 0;
    int pi = 0;
    std::string ps, country;     // the country is exact with an ECC
    std::string likely;          // without the ECC: "LT/ES/..."; empty when exact
    bool certain = false;        // the ECC proved the country
    float levelDb = 0;
    int64_t at = 0;
};

class FmDxLog {
public:
    void setHomeCountry(const std::string& iso) { home_ = iso; }   // "" : learned from what is heard most
    std::string homeCountry() const;
    void setMinAboveDb(float db) { minAbove_ = db; }
    void setLearnSeconds(int64_t s) { learnSec_ = s; }    // no alerts before this much listening (the first look at the band)
    // A station heard now. aboveDb: over the noise floor. Returns an alert the first time a foreign PI shows up on a frequency, once learned.
    FmDxAlert observe(int64_t now, double mhz, int pi, int ecc, const std::string& ps, float aboveDb);
    const std::vector<FmDxAlert>& alerts() const { return alerts_; }
    std::vector<FmDxHeard> heard() const;
    bool learned(int64_t now) const { return t0_ != 0 && now - t0_ >= learnSec_; }
    std::string toCsv() const;                            // the DX log: time, MHz, PI, PS, country
    std::string serialize() const;                        // the heard table, one station per line
    void load(const std::string& text);
private:
    std::map<uint64_t, FmDxHeard> heard_;
    std::vector<FmDxAlert> alerts_;
    std::string home_;
    float minAbove_ = 12;
    int64_t learnSec_ = 600, t0_ = 0;
};

} // namespace dect2
