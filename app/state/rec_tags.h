// Tags and notes for recordings: kept in one small text file in the recordings folder (".onair-tags.tsv"), keyed by the recording's file
// name, so a library moved with its folder keeps them. Pure (std only); the library window (app/panels/recording_library_ui.cpp) uses it.
// docs/RECORDING-TAGS.md.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace rectags {

struct Entry { std::vector<std::string> tags; std::string note; };
using Store = std::map<std::string, Entry>;   // file name -> tags and note

// tags typed as "airband, night; ISS" -> {"airband","night","iss"}: split on comma / semicolon, trimmed, lower-cased, no empties or duplicates
std::vector<std::string> parseTags(const std::string& text);
std::string joinTags(const std::vector<std::string>& tags);   // "airband, night"

const char* fileName();                                 // ".onair-tags.tsv"
bool load(const std::string& dir, Store& out);          // false when there is no file (out is then empty)
bool save(const std::string& dir, const Store& s);      // entries without tags and note are not written; false when the folder is not writable
void set(Store& s, const std::string& file, const std::string& tagsText, const std::string& note);

// Search: every word of the query must match (case-insensitive) the file name, a tag or the note; "#tag" matches a tag exactly.
// An empty query matches everything.
bool matches(const std::string& file, const Entry* e, const std::string& query);
// All tags in use with how many recordings carry each, most used first (the filter chips)
std::vector<std::pair<std::string, int>> tagCounts(const Store& s);

}  // namespace rectags
