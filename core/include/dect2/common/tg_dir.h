// Talkgroup directory for DMR, TETRA and P25: names for talkgroup numbers, edited by the user and imported / exported as CSV, shown beside the
// numbers in the voice and trunking panels. One kind has its own numbering, so a name belongs to (kind, id). The data is never shipped
// (docs/TALKGROUPS.md says where to get lists).
//
// Own CSV (export format): Kind,Id,Name,Group,Note   with Kind = DMR / TETRA / P25 (also read: a Kind column "any" is the importing panel's kind).
// RadioReference-style CSV (a header with Decimal / DEC, Alpha Tag, Description, Tag, Category, Mode): Name = Alpha Tag (else Description),
// Group = Tag (else Category), Note = Description. The kind is the panel's. Without a header: id, name [, group [, note]]. Comma, semicolon or tab;
// quotes and CRLF allowed.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

enum class TgKind { Dmr = 0, Tetra = 1, P25 = 2 };
const char* tgKindName(TgKind k);   // "DMR" "TETRA" "P25"

struct TgDirEntry { TgKind kind = TgKind::Dmr; uint32_t id = 0; std::string name, group, note; };

class TgDir {
public:
    // Reads CSV text into the directory (rows for the same kind+id replace what is there). Returns the rows added or replaced; rows without a
    // usable id or name are counted in *skipped.
    size_t import(const std::string& csv, TgKind panelKind, size_t* skipped = nullptr);
    std::string exportCsv(bool allKinds, TgKind only = TgKind::Dmr) const;
    bool find(TgKind k, uint32_t id, TgDirEntry& out) const;
    std::string label(TgKind k, uint32_t id) const;   // "" when unknown
    void set(const TgDirEntry& e);                       // adds or replaces; an empty name removes
    bool erase(TgKind k, uint32_t id);
    size_t size() const { return m_.size(); }
    size_t count(TgKind k) const;
    std::vector<TgDirEntry> entries(TgKind k, const std::string& filter = "") const;   // sorted by id; the filter matches id, name, group or note (case-insensitive words)
    void clear() { m_.clear(); }
private:
    std::map<std::pair<int, uint32_t>, TgDirEntry> m_;
};

// The directory of the app, safe to use from the UI and loaders. The file is <dataDir>/talkgroups.csv (kept by the app).
std::string tgLabel(TgKind k, uint32_t id);        // "" when unknown
TgDir tgSnapshot();                                // a copy to read or export
void tgEdit(const std::function<void(TgDir&)>& f);  // change it under the lock
uint64_t tgRevision();                             // grows with every change (the UI redraws caches)

}  // namespace dect2
