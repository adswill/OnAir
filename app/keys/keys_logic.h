// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Keys window's logic without any drawing, so it can be tested alone: hex entry checks, the edit form, the CSV import preview,
// masking and the per-call status classes. Key material is only ever returned masked here; the window asks keyHex() itself when the
// user presses Reveal. Nothing in this file logs or prints a key.
#pragma once
#include "dect2/voicekeys/voice_keys.h"
#include <string>
#include <vector>

namespace keyswin {

using dect2::voicekeys::Algorithm;
using dect2::voicekeys::Protocol;
using dect2::voicekeys::VoiceKey;

// The choices in the form's two lists (index 0 of the algorithm list is the first real cipher; Protocol list starts at "any").
const std::vector<Protocol>& protocolChoices();
const std::vector<Algorithm>& algorithmChoices();

// The text for a key being typed: "" when it is complete and valid for the algorithm, otherwise a clear error. An empty field says
// what to enter. Spaces and ':' between bytes are fine.
std::string hexProblem(Algorithm a, const std::string& hex);
// A live hint under the field: "8 of 8 bytes", "3 of 16 bytes (6 of 32 hex digits)", "any length". Never contains the key.
std::string hexCounter(Algorithm a, const std::string& hex);
// The one-line expectation for an algorithm, e.g. "AES-256-OFB: 32 bytes = 64 hex digits".
std::string lengthRule(Algorithm a);

// Mask a key of n bytes for display: asterisks plus the length, never any digit of the key.
std::string maskKey(size_t bytes);

// "key id 3, algorithm 0x84, TG 101, unit 5, NAC 0x293, CC 1"; "-" when none of them is set. Used for the table's association column.
std::string associationText(const VoiceKey& k);

// The edit form: every field as the user typed it.
struct KeyForm {
    std::string name, hex, keyId, algId, talkgroup, unit, nac, cc;
    int protocol = 0;    // index into protocolChoices()
    int algorithm = 0;   // index into algorithmChoices()
    bool enabled = true;
};
KeyForm formFromKey(const VoiceKey& k);
// Check the form and build the key. Empty string on success; otherwise the first problem, worded for the user, with the field named.
std::string keyFromForm(const KeyForm& f, VoiceKey& out);
// Parse an association number: blank = unset (-1); decimal or 0x hex; must be 0..max. Returns "" or the problem.
std::string parseAssociation(const std::string& text, long maxValue, long& out);

// ---- CSV / text import preview ----
struct PreviewRow {
    VoiceKey key;
    bool duplicate = false;   // an identical name+protocol+algorithm+key already exists in the store (it would be skipped)
};
struct CsvPreview {
    std::vector<PreviewRow> rows;
    std::vector<std::string> errors;   // one per bad line, "line N: ..."
    bool encryptedFile = false;        // the text is an OnAir encrypted export (needs the passphrase), not CSV
};
// Parse without changing anything. `existing` is the store's current list (for the duplicate flag).
CsvPreview previewCsv(const std::string& text, const std::vector<VoiceKey>& existing);
// The same checks for keys that did not come from CSV text (an encrypted export that was just opened).
CsvPreview previewKeyList(std::vector<VoiceKey> keys, const std::vector<VoiceKey>& existing, std::vector<std::string> errors = {});
// A preview row for the table, key masked: "club net | DMR | AES-256-OFB | ******** (32 bytes) | TG 101, CC 1".
std::string previewLine(const PreviewRow& r);
// Drop the duplicate rows and return the keys that would be added.
std::vector<VoiceKey> rowsToAdd(const CsvPreview& p);

// ---- per-call status (the core's strings) ----
enum class CallStatus { Clear, AddKey, Decrypted, Mismatch, Unsupported, Other };
CallStatus classifyStatus(const std::string& core);
// "name" out of "decrypted with key name"; "" for the other statuses.
std::string statusKeyName(const std::string& core);

// ---- the store state ----
enum class StoreState { NeedsNotice, Locked, Unlocked };
const char* storeStateText(StoreState s);

// The encrypted store's file inside the app's data folder (dataDir); the folder is voicekeys/ (dect2/voicekeys/key_guard.h).
std::string storeFilePath(const std::string& dataDir);
// A suggested name for an export, never an existing key-store name: onair-voice-keys.csv or .oavk.
std::string exportFileName(bool encrypted);

// The legal notice shown before the store works, one paragraph per entry (text from docs/KEYS.md).
const std::vector<std::string>& legalNotice();
// The warning shown above an export.
const char* exportWarning(bool encrypted);

} // namespace keyswin
