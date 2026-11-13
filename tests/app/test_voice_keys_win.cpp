// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Keys window's pure logic (app/keys/keys_logic.*) and the guards that keep key files out of bug-report zips and key code out of the
// web server and the remote API (core/include/dect2/voicekeys/key_guard.h). Synthetic keys only: nothing here is a real key.
#include "keys/keys_logic.h"
#include "dect2/voicekeys/key_guard.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef DECT2_SOURCE_DIR
#define DECT2_SOURCE_DIR "."
#endif

using namespace keyswin;
using namespace dect2::voicekeys;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::string rep(const char* s, int n) { std::string o; for (int i = 0; i < n; i++) o += s; return o; }

static void testHex() {
    CHECK(hexProblem(Algorithm::Aes256Ofb, rep("ab", 32)).empty(), "aes256 ok");
    CHECK(hexProblem(Algorithm::Aes256Ofb, rep("ab:", 31) + "ab").empty(), "colon separators ok");
    CHECK(hexProblem(Algorithm::Aes128Ofb, rep("00 ", 16)).empty(), "aes128 spaced ok");
    CHECK(!hexProblem(Algorithm::Aes256Ofb, rep("ab", 16)).empty(), "aes256 too short");
    CHECK(hexProblem(Algorithm::Aes256Ofb, rep("ab", 16)).find("32") != std::string::npos, "message names the needed length");
    CHECK(!hexProblem(Algorithm::Aes128Ofb, rep("ab", 17)).empty(), "aes128 too long");
    CHECK(hexProblem(Algorithm::DesOfb, "0123456789abcdef").empty(), "des ok");
    CHECK(!hexProblem(Algorithm::DesOfb, "0123456789abcde").empty(), "odd digits rejected");
    CHECK(hexProblem(Algorithm::DesOfb, "0123456789abcde").find("odd") != std::string::npos, "odd message");
    CHECK(hexProblem(Algorithm::Rc4, "0102030405").empty(), "rc4 any length ok");
    CHECK(!hexProblem(Algorithm::Rc4, "").empty(), "empty rejected");
    CHECK(!hexProblem(Algorithm::Rc4, "01zz").empty(), "non-hex rejected");
    CHECK(hexProblem(Algorithm::Rc4, "01zz").find("'z'") != std::string::npos, "names the bad character");
    CHECK(hexProblem(Algorithm::Scrambler, "7fff").empty(), "scrambler 7fff ok");
    CHECK(!hexProblem(Algorithm::Scrambler, "8000").empty(), "scrambler needs 15 bits");
    CHECK(!hexProblem(Algorithm::Scrambler, "7f").empty(), "scrambler needs 2 bytes");
    CHECK(hexCounter(Algorithm::Aes128Ofb, "0011").find("2 of 16") != std::string::npos, "counter: %s", hexCounter(Algorithm::Aes128Ofb, "0011").c_str());
    CHECK(hexCounter(Algorithm::Aes128Ofb, "zz").empty(), "counter empty on bad hex");
    CHECK(lengthRule(Algorithm::Aes256Ofb).find("64 hex") != std::string::npos, "length rule");
}

static void testMask() {
    const std::string m = maskKey(32);
    CHECK(m.find("32 bytes") != std::string::npos, "mask shows length");
    CHECK(m.find_first_of("0123456789abcdef") == m.find("32"), "mask has no hex digits before the length: %s", m.c_str());
    VoiceKey k; k.name = "n"; k.protocol = Protocol::P25; k.algorithm = Algorithm::Aes256Ofb;
    parseKeyHex(k.algorithm, "deadbeef" + rep("00", 28), k.key);
    PreviewRow r; r.key = k;
    const std::string line = previewLine(r);
    CHECK(line.find("deadbeef") == std::string::npos && line.find("DEADBEEF") == std::string::npos, "preview line hides the key: %s", line.c_str());
    CHECK(line.find("32 bytes") != std::string::npos, "preview line shows the length");
}

static void testAssociation() {
    long v = 0;
    CHECK(parseAssociation("", 255, v).empty() && v == -1, "blank = unset");
    CHECK(parseAssociation("0x84", 255, v).empty() && v == 0x84, "hex assoc");
    CHECK(parseAssociation("101", 16777215, v).empty() && v == 101, "decimal assoc");
    CHECK(!parseAssociation("300", 255, v).empty(), "out of range");
    CHECK(!parseAssociation("-1", 255, v).empty(), "negative");
    CHECK(!parseAssociation("x1", 255, v).empty(), "not a number");
    VoiceKey k; k.keyId = 3; k.nac = 0x293; k.colourCode = 1; k.talkgroup = 101; k.algId = 0x84;
    CHECK(associationText(k) == "key id 3, algorithm 0x84, TG 101, NAC 0x293, CC 1", "assoc text: %s", associationText(k).c_str());
    CHECK(associationText(VoiceKey()) == "-", "no assoc");
}

static void testForm() {
    KeyForm f;
    f.name = "club net"; f.protocol = 1;   // P25
    f.algorithm = 0;                       // AES-256-OFB
    f.hex = rep("11", 32); f.keyId = "3"; f.nac = "0x293";
    VoiceKey k;
    CHECK(keyFromForm(f, k).empty(), "form ok: %s", keyFromForm(f, k).c_str());
    CHECK(k.protocol == Protocol::P25 && k.algorithm == Algorithm::Aes256Ofb && k.keyId == 3 && k.nac == 0x293 && k.key.size() == 32, "form fields");
    const KeyForm back = formFromKey(k);
    CHECK(back.protocol == f.protocol && back.algorithm == f.algorithm && back.keyId == "3", "round trip form");
    f.hex = rep("11", 16);
    CHECK(keyFromForm(f, k).find("Key:") == 0, "form bad key: %s", keyFromForm(f, k).c_str());
    f.hex = rep("11", 32); f.nac = "99999";
    CHECK(keyFromForm(f, k).find("NAC") == 0, "form bad nac");
    f.nac = ""; f.name = "  ";
    CHECK(keyFromForm(f, k).empty() && !k.name.empty(), "empty name gets a default");
}

static void testCsv() {
    const std::string key32 = rep("ab", 32);
    const std::string csv =
        "# comment\nname,protocol,algorithm,key,keyid,algid,talkgroup,unit,nac,cc,enabled\n"
        "club net,DMR,AES-256-OFB," + key32 + ",,,101,,,1,1\n"
        "sar,P25,AES-256-OFB," + key32 + ",3,0x84,,,0x293,,0\n"
        "bad hex,P25,AES-256-OFB,zz\n"
        "short,P25,AES-256-OFB,0011\n"
        "odd,NXDN,DES," + std::string("0123456789abcde") + "\n"
        "mystery,XYZ,AES-256-OFB," + key32 + "\n"
        "noalg,P25,," + key32 + "\n"
        "few,fields\n";
    VoiceKey existing; existing.name = "sar"; existing.protocol = Protocol::P25; existing.algorithm = Algorithm::Aes256Ofb;
    parseKeyHex(existing.algorithm, key32, existing.key);
    const CsvPreview p = previewCsv(csv, {existing});
    CHECK(p.rows.size() == 2, "2 good rows, got %zu", p.rows.size());
    CHECK(p.errors.size() == 6, "6 errors, got %zu", p.errors.size());
    CHECK(p.rows.size() == 2 && !p.rows[0].duplicate && p.rows[1].duplicate, "duplicate detection");
    CHECK(p.rows.size() == 2 && !p.rows[1].key.enabled, "enabled flag");
    CHECK(rowsToAdd(p).size() == 1, "only the new row is added");
    bool lineNo = false;
    for (auto& e : p.errors) if (e.find("line 5") != std::string::npos) lineNo = true;
    CHECK(lineNo, "errors carry the line number");
    for (auto& e : p.errors) CHECK(e.find(key32) == std::string::npos, "an error never repeats a key");
    const CsvPreview p2 = previewCsv("OAVK1\nxxxx", {});
    CHECK(p2.encryptedFile && p2.rows.empty(), "encrypted export detected");
    const CsvPreview p3 = previewCsv("", {});
    CHECK(p3.rows.empty() && p3.errors.empty(), "empty text");
}

static void testStatus() {
    CHECK(classifyStatus("encrypted: add your key") == CallStatus::AddKey, "add key");
    CHECK(classifyStatus("decrypted with key club net") == CallStatus::Decrypted, "decrypted");
    CHECK(statusKeyName("decrypted with key club net") == "club net", "key name");
    CHECK(statusKeyName("key mismatch").empty(), "no name");
    CHECK(classifyStatus("key mismatch") == CallStatus::Mismatch, "mismatch");
    CHECK(classifyStatus("") == CallStatus::Clear && classifyStatus("clear") == CallStatus::Clear, "clear");
    CHECK(classifyStatus("encrypted: unsupported algorithm") == CallStatus::Unsupported, "unsupported");
    CHECK(storeFilePath("/data").find("voicekeys/keys.oavk") != std::string::npos, "store path");
    CHECK(isKeyMaterialPath(storeFilePath("/data")), "the store path is key material");
    CHECK(isKeyMaterialPath(exportFileName(true)) && isKeyMaterialPath(exportFileName(false)), "export names are key material");
    CHECK(!legalNotice().empty(), "legal notice text");
}

static void testGuard() {
    CHECK(isKeyMaterialPath("keys.oavk"), "oavk");
    CHECK(isKeyMaterialPath("C:\\Users\\x\\AppData\\Local\\OnAir\\voicekeys\\keys.oavk"), "windows path");
    CHECK(isKeyMaterialPath("/home/x/.local/share/OnAir/VoiceKeys/anything.bin"), "inside the key folder, any case");
    CHECK(isKeyMaterialPath("/tmp/Onair-Voice-Keys-2026.csv"), "export csv by prefix");
    CHECK(isKeyMaterialPath("backup.OAVK"), "extension case");
    CHECK(!isKeyMaterialPath("diagnostics.txt") && !isKeyMaterialPath("README.txt") && !isKeyMaterialPath("onair.log"), "report files are not key material");
    CHECK(!isKeyMaterialPath("/data/voicekeys-notes/readme.txt"), "a similarly named folder is fine");
    std::vector<std::string> names = {"diagnostics.txt", "keys.oavk", "a/voicekeys/x.txt", "onair-voice-keys.csv", "README.txt"};
    dropKeyMaterial(names);
    CHECK(names.size() == 2 && names[0] == "diagnostics.txt" && names[1] == "README.txt", "dropKeyMaterial leaves the report files");
}

static std::string slurp(const fs::path& p) { std::ifstream in(p, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }

// Code guard: nothing the web server or the remote API is built from may reach the key store, and the zip writer must apply the exclusion.
static void testSourceGuards() {
    const fs::path root = DECT2_SOURCE_DIR;
    if (!fs::exists(root / "core" / "src" / "net")) { printf("note: source tree not found, source guards skipped\n"); return; }
    std::vector<fs::path> files;
    for (const char* dir : {"core/src/net", "core/src/results", "core/include/dect2/net", "app/map"})
        for (auto& e : fs::recursive_directory_iterator(root / dir)) if (e.is_regular_file()) files.push_back(e.path());
    files.push_back(root / "app" / "platform" / "remote_api_app.cpp");
    int checked = 0;
    for (const auto& f : files) {
        const std::string ext = f.extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".mm") continue;
        const std::string s = slurp(f);
        if (s.empty()) continue;
        checked++;
        for (const char* bad : {"voicekeys", "KeyStore", "legalAccepted", "keyHex", "key_persist", "voice_decrypt", "keys_logic"})
            CHECK(s.find(bad) == std::string::npos, "%s must not mention %s (web server / remote API never touch voice keys)", f.string().c_str(), bad);
    }
    CHECK(checked > 5, "scanned %d web/API files", checked);
    const std::string zip = slurp(root / "app" / "panels" / "diag_ui.cpp");
    CHECK(zip.find("isKeyMaterialPath(f.first)") != std::string::npos, "the zip writer applies the key-file exclusion");
    // the Keys window may not hand keys to the log
    const std::string ui = slurp(root / "app" / "keys" / "keys_ui.cpp");
    CHECK(!ui.empty(), "keys_ui.cpp found");
    CHECK(ui.find("engine.log") == std::string::npos && ui.find("fprintf(") == std::string::npos && ui.find(" printf(") == std::string::npos &&
              ui.find("std::cout") == std::string::npos && ui.find("std::cerr") == std::string::npos && ui.find("puts(") == std::string::npos,
          "the Keys window never writes to a log or the console");
}

int main() {
    testHex(); testMask(); testAssociation(); testForm(); testCsv(); testStatus(); testGuard(); testSourceGuards();
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("voice keys window logic: ok\n");
    return 0;
}
