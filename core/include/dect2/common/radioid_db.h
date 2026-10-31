// Radio-ID name database for DMR and NXDN: a CSV the user downloads (radioid.net style) or brings, parsed locally, so calls show
// "1234567  W1ABC Joe Smith" instead of a bare number. The data is never shipped with OnAir (docs/RADIOID.md says where to get it).
//
// Accepted files: comma, semicolon or tab separated, with or without a header line, quotes and CRLF allowed. Recognised header names
// (case and punctuation ignored): radio id / id / dmr id / nxdn id, callsign / call, first name / name / fname, last name / surname,
// city, state / region, country. Without a header the order is id, callsign, first name, last name, city, state, country.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

class RadioIdDb {
public:
    struct Entry { std::string callsign, name, place; };   // place: "City, State, Country" (the parts that exist)
    // Replaces the contents with the parsed text; returns the number of rows kept (bad rows are skipped and counted in skipped())
    size_t parse(const std::string& csv);
    bool loadFile(const std::string& path, std::string& err);
    size_t size() const { return ids_.size(); }
    size_t skipped() const { return skipped_; }
    bool find(uint32_t id, Entry& out) const;
    // "W1ABC Joe Smith" (callsign and name; either may be missing), "" when the id is not in the file
    std::string label(uint32_t id) const;
    void clear() { ids_.clear(); pool_.clear(); offs_.clear(); skipped_ = 0; }
private:
    std::vector<uint32_t> ids_;     // sorted
    std::vector<uint32_t> offs_;    // start of the record in pool_ ("call\x1fname\x1fplace\0")
    std::string pool_;
    size_t skipped_ = 0;
};

// The two databases of the app (kind 0 DMR, 1 NXDN), safe to use from the UI and a loader thread
enum class RadioIdKind { Dmr = 0, Nxdn = 1 };
std::string radioIdLabel(RadioIdKind k, uint32_t id);            // "" when no file is loaded or the id is unknown
void radioIdInstall(RadioIdKind k, std::shared_ptr<RadioIdDb> db);   // nullptr unloads
size_t radioIdCount(RadioIdKind k);
const char* radioIdUrl(RadioIdKind k);                          // where the public file is, for the download button and the docs

}  // namespace dect2
