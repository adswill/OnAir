// Privacy pieces of the session store (docs/MODE-EXPERIENCE.md, 2.7): the per-install salt, salted hashing of personal identifiers with a
// per-mode "Keep real ids" switch, the single "Redact message text" switch, the escaping helpers every writer uses (radio text is
// attacker controlled) and the owner-only file helpers.
//
// Personal or tracking-capable identifiers (BLE and Wi-Fi addresses, ANT+ ids, NFC UIDs, DECT and phone identities, meter ids, Remote ID,
// pager capcodes, satellite terminal ids, radio ids paired with a position ...) are stored as a stable short tag derived from a salted
// hash ("dev 7F3A") unless the user switched "Keep real ids" on for that mode. The salt is 128 random bits created at first use, kept in
// a file only the owner can read and never exported (not in a backup, a bundle or a log).
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace dect2::sess {

// How a mode's ids are treated
enum class IdClass : uint8_t {
    Open,           // ids are not personal (aircraft, ships, balloons ...): stored as they are
    Personal,       // always personal: hashed unless "Keep real ids" is on for the mode
    WithPosition,   // personal only when the session carries a position (radio ids with LRRP, APRS, D-PRS ...)
};

class Privacy {
public:
    Privacy();
    ~Privacy();

    // ---- the salt
    // Loads <dir>/session_salt.bin, creating it (16 random bytes, owner-only) when missing. false when it cannot be written (the salt then lives
    // in memory only: hashes are stable within the run but not across runs).
    bool loadSalt(const std::string& dir);
    void setSalt(const std::array<uint8_t, 16>& salt);   // tests
    std::array<uint8_t, 16> salt() const;                // tests only: never put it in a file other than session_salt.bin
    bool regenerateSalt();                               // "Delete everything": old tags can no longer be matched to new ones

    // ---- hashing
    // 64 bit keyed hash of (mode, id) as 16 hex digits: HMAC-SHA256(salt, mode || id), cut. Different per install, stable within one.
    std::string hash(uint16_t mode, const std::string& id) const;
    // "dev 7F3A": the label and the first 4 hex digits of the hash (what the list shows while real ids are off)
    std::string shortTag(uint16_t mode, const std::string& id, const char* label = "id") const;

    // ---- per mode
    void setClass(uint16_t mode, IdClass c);
    IdClass classOf(uint16_t mode) const;
    void setKeepRealIds(uint16_t mode, bool keep);
    bool keepRealIds(uint16_t mode) const;
    // true when an id of this mode must not be stored as it is (Personal, or WithPosition with a position, and "Keep real ids" off)
    bool masks(uint16_t mode, bool hasPosition) const;
    // `id` itself, or its short tag when masked. Empty ids stay empty.
    std::string protect(uint16_t mode, const std::string& id, bool hasPosition = false, const char* label = "id") const;
    // The modes of docs/MODE-EXPERIENCE.md 2.7 get their classes (BLE, Wi-Fi, ANT+, NFC, DECT, meters, Remote ID, pagers, satellite terminals,
    // digital voice with positions ...); an adapter may override with setClass(). Called once by the constructor of the global instance.
    void applyDefaultClasses();

    // ---- text redaction: one switch for pager text, APRS messages, LoRa chat, ACARS text and notifications
    void setRedactText(bool on);
    bool redactText() const;
    // the text itself, or a placeholder of the same length class ("[redacted, 42 characters]") while the switch is on
    std::string text(const std::string& s) const;

    // ---- settings as key=value lines (kept in the app's settings; never the salt)
    std::string save() const;
    void load(const std::string& text);

private:
    mutable std::mutex mu_;
    std::array<uint8_t, 16> salt_{};
    bool haveSalt_ = false;
    std::string saltPath_;
    std::map<uint16_t, IdClass> cls_;
    std::map<uint16_t, bool> keep_;
    bool redact_ = false;
    void ensureSaltLocked();
};

Privacy& privacy();   // the program-wide instance

// ---- escaping for every export and writer. Radio text is attacker controlled.
// A CSV cell: control characters removed, a leading = + - @ (also after spaces or a tab) gets a leading apostrophe so a spreadsheet does not run it as
// a formula, quoted when it holds a comma or a quote. Numbers are written by the exporter itself, never through this function.
std::string csvCell(const std::string& s);
// A JSON string body (no surrounding quotes): quotes, backslashes and control characters escaped; invalid UTF-8 bytes become U+FFFD.
std::string jsonEscape(const std::string& s);
// SRT / subtitle text: control characters removed except line breaks, an empty line (which ends a cue) turned into a space.
std::string srtText(const std::string& s);
// Control characters (below 0x20 except \t, and 0x7F) removed; invalid UTF-8 replaced.
std::string stripControl(const std::string& s);
// true when `s` is valid UTF-8
bool validUtf8(const std::string& s);
// A string cut to at most `maxBytes` bytes without splitting a UTF-8 character.
std::string cutUtf8(const std::string& s, size_t maxBytes);

// ---- files only the owner can use: 0700 folders and 0600 files on POSIX, a protected ACL with the current user on Windows
bool makeOwnerOnlyDir(const std::string& path);                 // creates the folder (its parents with the default mode) owner-only; a folder that exists is left alone; false when it cannot be made
bool restrictToOwner(const std::string& path, bool isDir);      // tightens an existing file or folder
bool writeOwnerOnlyFile(const std::string& path, const std::string& data);   // atomic: temp file in the same folder, then rename

}  // namespace dect2::sess
