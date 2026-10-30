// The sessions (calls, threads, tracks) of the session store as JSON for the remote API and scripts (docs/MODE-EXPERIENCE.md, 2.2 point 3).
// Paged, never the whole list: limit is at most 200 per page. The payload is sanitised like the summary events: ids as the store keeps them (personal
// ids are short tags already), the key and title, times and counts; never aliases, tags, notes, message text, file paths or positions.
#pragma once
#include <cstdint>
#include <string>

namespace dect2 {

// query: "mode=9&kind=1&offset=0&limit=50&q=text&active=1&since=ms". Unknown or missing keys are ignored. Returns an object
// {"gen":N,"total":N,"offset":N,"limit":N,"sessions":[...]}; an error (bad number) gives {"error":"..."} .
std::string sessionsJson(const std::string& query);

// the counters for the telemetry block: {"gen":N,"dropped":N,"logging":true}
std::string sessionStatusJson();

}  // namespace dect2
