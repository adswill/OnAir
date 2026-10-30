// Retention of the library files the sessions point at (docs/MODE-EXPERIENCE.md, 2.7 storage hygiene): audio older than N days or beyond a
// size cap, IQ recordings the same, and the trash of user deletions.
//
// Safety rules: only files that a link of the session database points at are ever considered (never "every WAV in the folder"); only inside
// the configured library roots (a link pointing anywhere else is ignored, so a damaged database cannot make the program delete a file elsewhere);
// files of starred sessions are kept; the sidecar next to a deleted file (.json, .sigmf-meta) goes with it.
#pragma once
#include "dect2/session/session_db.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::sess {

struct LibraryRule {
    std::vector<std::string> roots;    // folders the files must be inside
    int maxAgeDays = 0;                // 0 = no age limit
    int64_t maxBytes = 0;              // 0 = no size cap
};

struct LibraryResult {
    int filesDeleted = 0;
    int staleLinks = 0;                // links to files that no longer exist (removed)
    int64_t bytesFreed = 0;
    std::vector<std::string> deleted;
};

// Applies `rule` to the files of the links of `linkType` (Audio or Iq). nowMs: UTC ms.
LibraryResult cleanLibrary(SessionDb& db, LinkType linkType, const LibraryRule& rule, int64_t nowMs);

// true when `path` is a file inside one of `roots` (symlinks and ".." resolved)
bool insideRoots(const std::string& path, const std::vector<std::string>& roots);

// User deletions: the file (and its sidecar) is moved into <trashDir>/<yyyy-mm-dd>/ and stays trashDays days. false when the move failed.
bool moveToTrash(const std::string& path, const std::string& trashDir, int64_t nowMs);
// Permanently removes what has been in the trash longer than `days` (mtime of the day folder's files). Returns the files removed.
int purgeTrash(const std::string& trashDir, int days, int64_t nowMs);

}  // namespace dect2::sess
