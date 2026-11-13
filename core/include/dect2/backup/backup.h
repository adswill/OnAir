// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Backup and restore of the user's data (Tools > Backup and restore). Pure functions, no UI and no globals.
//
// The archive (.onairbackup) is an ordinary zip with the entries stored (not compressed), so it opens with any zip tool:
//   manifest.txt        what is inside (format, app version, items, files)
//   settings.txt        the settings (secret keys left out)
//   codecs-listing.txt  the names of the user-installed codecs (never the codec files)
//   data/<path>         files from the data folder
//   secrets.enc         ONLY with "include secrets" + a passphrase: voice keys and API credentials, encrypted
// Secrets are encrypted as the voice key store does: PBKDF2-HMAC-SHA256 (200000 rounds) -> AES-256-CTR + HMAC-SHA256 (encrypt-then-MAC),
// built on dect2::meshcrypto. No secret is ever written to the archive in the clear, logged, or put into a diagnostic bundle.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {
namespace backup {

constexpr int kFormat = 1;
extern const char* const kExt;   // ".onairbackup"

// ---- what can be backed up ----
struct ItemDef {
    const char* id;          // "settings", "memories", ...
    const char* label;       // shown with the tick box
    bool secret;             // only with include secrets + a passphrase
    std::vector<std::string> paths;   // relative to the data folder: "dir/" = the whole folder, "*.ext" = files of the data folder itself
};
const std::vector<ItemDef>& catalog();
const ItemDef* findItem(const std::string& id);
// Which item (id) a data-folder file belongs to; empty when none (a file nobody asked to keep).
std::string itemOfPath(const std::string& rel);
// True for a settings key that holds a credential (API keys, passwords, tokens): kept out of the plain settings.
bool isSecretPrefKey(const std::string& key);

// ---- settings records ----
struct PrefItem { std::string key; char type; std::string value; };   // type: D double, I integer, B bool, S string, C scan channel
std::string serializePrefs(const std::vector<PrefItem>& items);
bool parsePrefs(const std::string& text, std::vector<PrefItem>& out);
// Split into the plain part and the credentials.
void splitSecretPrefs(const std::vector<PrefItem>& all, std::vector<PrefItem>& plain, std::vector<PrefItem>& secret);

// ---- what a restore may apply ----
// Allow: applied. Confirm: only after the user ticks that exact key in the UI (network exposure, server addresses, uploads, transmit,
// paths, consents, anything unknown). Never: dropped (a credential-looking key in the plain part, an odd key name).
enum class PrefPolicy { Allow, Confirm, Never };
PrefPolicy prefRestorePolicy(const std::string& key, char type, bool fromSealedBlock);
void classifyRestorePrefs(const std::vector<PrefItem>& in, bool fromSealedBlock, std::vector<PrefItem>& allow, std::vector<PrefItem>& confirm, std::vector<std::string>& refused);

// ---- the archive ----
struct Entry { std::string name; std::string data; };
std::string zipPack(const std::vector<Entry>& entries);

struct Limits {
    size_t maxEntries = 20000;
    uint64_t maxEntryBytes = 512ull << 20;
    uint64_t maxTotalBytes = 2048ull << 20;
    size_t maxNameLen = 240;
};
// Reads a stored zip. Returns "" on success, else why not. Refuses: compressed or encrypted entries (a zip bomb needs compression), zip64,
// duplicate names, unsafe names (see safeRelPath), sizes beyond the limits or the archive, bad CRC, overlapping or out-of-range offsets.
std::string zipUnpack(const std::string& bytes, std::vector<Entry>& out, const Limits& lim = Limits());
// A name that stays inside the target folder: not empty, not absolute (no leading / or \, no drive letter), no "..", ".", empty parts,
// backslash, colon, control characters or trailing dot/space in a part.
bool safeRelPath(const std::string& rel, size_t maxLen = 240);

// False for a path a backup may never write: the folders whose content is loaded as code (codecs/, plugins/ ...) and library or executable
// extensions. Applied to the manifest check, the plan and the writer.
bool restorablePath(const std::string& rel);

// ---- manifest ----
struct Manifest {
    int format = 0;
    std::string app, created;
    bool secrets = false;
    struct Item { std::string id; int files = 0; uint64_t bytes = 0; };
    std::vector<Item> items;
    struct File { std::string name; uint64_t bytes = 0; std::string item; };
    std::vector<File> files;
};
std::string writeManifest(const Manifest& m);
bool parseManifest(const std::string& text, Manifest& m);
// The archive entries must be exactly the manifest's files plus the fixed ones; returns a reason, "" when it matches.
std::string checkManifestAgainst(const Manifest& m, const std::vector<Entry>& entries);

enum class VersionState { Ok, Warn, Refuse };
// A newer archive format is refused; an archive from a newer app version only warns.
VersionState checkVersion(const Manifest& m, const std::string& thisApp, std::string& message);
int compareVersions(const std::string& a, const std::string& b);   // numeric parts, <0 a older, 0 equal, >0 a newer

// ---- restore planning ----
enum class Mode { Merge, Replace };   // Merge keeps what exists and adds what is missing; Replace overwrites with the archive's copy
struct PlanOp { std::string name; enum Kind { Write, SkipExisting } kind; bool existed; };
// ids = the ticked items. existing = the data-folder paths that exist now. Only manifest files of ticked, non-"settings" items.
std::vector<PlanOp> planRestore(const Manifest& m, const std::set<std::string>& ids, Mode mode, const std::set<std::string>& existing);
// Settings merge: Merge adds only keys that are missing (haveKeys), Replace takes all.
std::vector<PrefItem> selectPrefs(const std::vector<PrefItem>& in, Mode mode, const std::set<std::string>& haveKeys);

// ---- extraction onto disk (name = path inside the data folder, without the "data/" prefix) ----
struct ExtractReport { int written = 0, skipped = 0; std::vector<std::string> errors; };
// dryRun writes nothing and reports what would happen. Every target is checked to stay inside root (no symlinked parents); each file is
// written to a temporary name and renamed over the old one.
ExtractReport extractTo(const std::string& root, const std::vector<Entry>& entries, const std::vector<PlanOp>& plan, bool dryRun);
bool writeFileAtomic(const std::string& path, const std::string& data);

// ---- secrets ----
std::string packEntries(const std::vector<Entry>& e);
bool unpackEntries(const std::string& blob, std::vector<Entry>& out, const Limits& lim = Limits());
bool passphraseOk(const std::string& pass, std::string& why);   // at least 8 characters
// "OABK1\n" | salt16 | rounds u32 | iv16 | mac32 | ciphertext. Returns "" on success.
std::string sealSecrets(const std::string& plain, const std::string& passphrase, std::string& blob, uint32_t rounds = 200000);
// Wrong passphrase, tampering and truncation are all reported as an error (MAC checked before decrypting).
std::string openSecrets(const std::string& blob, const std::string& passphrase, std::string& plain);
void pbkdf2Sha256(const std::string& pw, const uint8_t* salt, size_t saltLen, uint32_t rounds, uint8_t* out, size_t outLen);

// ---- automatic backup (never with secrets) ----
std::string autoBackupName(int64_t epochSec);   // onair-auto-YYYYMMDD-HHMMSS.onairbackup (UTC)
bool isAutoBackupName(const std::string& name);
bool autoBackupDue(int64_t lastEpoch, int64_t now, int intervalDays);
// Names (of the automatic ones only) to delete so that `keep` newest remain.
std::vector<std::string> autoBackupsToDelete(std::vector<std::string> names, int keep);

} // namespace backup
} // namespace dect2
