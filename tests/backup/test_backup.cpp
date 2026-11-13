// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Backup and restore: manifest, selection, safe extraction (malicious archives), encryption wrapper, automatic backup naming.
#include "dect2/backup/backup.h"
#include "dect2/mesh/mesh_crypto.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace dect2::backup;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void w16(std::string& s, size_t p, uint32_t v) { s[p] = (char)(v & 255); s[p + 1] = (char)((v >> 8) & 255); }
static void w32(std::string& s, size_t p, uint32_t v) { w16(s, p, v & 0xFFFF); w16(s, p + 2, v >> 16); }
static uint32_t r32(const std::string& s, size_t p) { return (uint8_t)s[p] | ((uint32_t)(uint8_t)s[p + 1] << 8) | ((uint32_t)(uint8_t)s[p + 2] << 16) | ((uint32_t)(uint8_t)s[p + 3] << 24); }
static size_t centralDir(const std::string& z) { return r32(z, z.size() - 22 + 16); }   // no zip comment in our archives

static std::string good() { return zipPack({{"manifest.txt", "x"}, {"data/layouts/a.ini", "hello world"}, {"data/memories.json", "[1,2,3]"}}); }

static void testPaths() {
    CHECK(safeRelPath("data/a/b.txt"));
    CHECK(safeRelPath("manifest.txt"));
    CHECK(!safeRelPath(""));
    CHECK(!safeRelPath("../x"));
    CHECK(!safeRelPath("a/../../x"));
    CHECK(!safeRelPath("a/.."));
    CHECK(!safeRelPath("/etc/passwd"));
    CHECK(!safeRelPath("\\windows\\x"));
    CHECK(!safeRelPath("C:/x"));
    CHECK(!safeRelPath("C:x"));
    CHECK(!safeRelPath("a\\b"));
    CHECK(!safeRelPath("a//b"));
    CHECK(!safeRelPath("./a"));
    CHECK(!safeRelPath("a/./b"));
    CHECK(!safeRelPath(std::string("a\0b", 3)));
    CHECK(!safeRelPath("a\nb"));
    CHECK(!safeRelPath("dir./x"));
    CHECK(!safeRelPath("a /x"));
    CHECK(!safeRelPath(std::string(300, 'a')));
    CHECK(safeRelPath("..a/b"));   // a name that merely starts with dots is fine
}

static void testZip() {
    std::vector<Entry> out;
    const std::string z = good();
    CHECK(zipUnpack(z, out).empty());
    CHECK(out.size() == 3 && out[1].name == "data/layouts/a.ini" && out[1].data == "hello world");
    CHECK(!zipUnpack(z.substr(0, 10), out).empty());
    CHECK(!zipUnpack(z.substr(0, z.size() - 5), out).empty());
    CHECK(!zipUnpack(std::string(100, 'x'), out).empty());
    CHECK(!zipUnpack("", out).empty());
    // names
    for (const char* bad : {"../evil", "/abs/evil", "a/../../evil", "C:/evil", "a\\b", "data/../../evil"}) {
        std::string r = zipUnpack(zipPack({{bad, "x"}}), out);
        CHECK(!r.empty());
        CHECK(out.empty());
    }
    // duplicate names
    CHECK(!zipUnpack(zipPack({{"a", "1"}, {"a", "2"}}), out).empty());
    // compressed entry (method 8) in the central directory
    { std::string m = z; w16(m, centralDir(m) + 10, 8); CHECK(!zipUnpack(m, out).empty()); }
    // method 8 in the local header only
    { std::string m = z; w16(m, 8, 8); CHECK(!zipUnpack(m, out).empty()); }
    // the encrypted flag
    { std::string m = z; w16(m, centralDir(m) + 8, 1); CHECK(!zipUnpack(m, out).empty()); }
    // claimed sizes: larger than the archive, and a huge "uncompressed" size
    { std::string m = z; const size_t c = centralDir(m); w32(m, c + 20, 0x7FFFFFFF); w32(m, c + 24, 0x7FFFFFFF); CHECK(!zipUnpack(m, out).empty()); }
    { std::string m = z; const size_t c = centralDir(m); w32(m, c + 24, 5); CHECK(!zipUnpack(m, out).empty()); }   // csz != usz
    // zip64 markers
    { std::string m = z; const size_t c = centralDir(m); w32(m, c + 20, 0xFFFFFFFFu); w32(m, c + 24, 0xFFFFFFFFu); CHECK(!zipUnpack(m, out).empty()); }
    // local header offset pointing outside / into the directory
    { std::string m = z; w32(m, centralDir(m) + 42, (uint32_t)m.size() + 100); CHECK(!zipUnpack(m, out).empty()); }
    { std::string m = z; w32(m, centralDir(m) + 42, (uint32_t)centralDir(m)); CHECK(!zipUnpack(m, out).empty()); }
    // two entries pointing at the same data (overlap)
    {
        std::string m = z; const size_t c = centralDir(m);
        const size_t second = c + 46 + 12;   // "manifest.txt" is 12 bytes: the next central record
        w32(m, second + 42, 0);
        CHECK(!zipUnpack(m, out).empty());
    }
    // bad CRC
    { std::string m = z; m[30 + 12] ^= 0x55; CHECK(!zipUnpack(m, out).empty()); }
    // too many entries declared
    { std::string m = z; w16(m, m.size() - 22 + 10, 60000); w16(m, m.size() - 22 + 8, 60000); CHECK(!zipUnpack(m, out).empty()); }
    // limits: one big entry, total, count, name length
    { Limits l; l.maxEntryBytes = 5; CHECK(!zipUnpack(z, out, l).empty()); }
    { Limits l; l.maxTotalBytes = 15; CHECK(!zipUnpack(z, out, l).empty()); }
    { Limits l; l.maxEntries = 2; CHECK(!zipUnpack(z, out, l).empty()); }
    { Limits l; l.maxNameLen = 8; CHECK(!zipUnpack(z, out, l).empty()); }
    // garbage in front or a comment-free archive with the directory inside the data
    { std::string m = std::string(50, 'z') + z; CHECK(!zipUnpack(m, out).empty()); }
    // random corruption never crashes
    for (size_t i = 0; i < z.size(); i++) { std::string m = z; m[i] ^= 0xFF; zipUnpack(m, out); }
    for (size_t n = 0; n < z.size(); n++) zipUnpack(z.substr(0, n), out);
}

static void testManifest() {
    Manifest m;
    m.format = 1; m.app = "0.3.0"; m.created = "2026-10-22T10:00:00Z"; m.secrets = true;
    m.items = {{"layouts", 1, 11}, {"voicekeys", 1, 5}};
    m.files = {{"layouts/a.ini", 11, "layouts"}, {"voicekeys/keys.oavk", 5, "voicekeys"}};
    Manifest r;
    CHECK(parseManifest(writeManifest(m), r));
    CHECK(r.format == 1 && r.app == "0.3.0" && r.secrets && r.items.size() == 2 && r.files.size() == 2 && r.files[0].name == "layouts/a.ini" && r.files[1].bytes == 5);
    CHECK(!parseManifest("nonsense", r));
    CHECK(!parseManifest("OnAir backup\nformat=abc\n", r));
    CHECK(!parseManifest("OnAir backup\nformat=1\nfile\ta\tx\tb\n", r));
    CHECK(!parseManifest("OnAir backup\napp=1\n", r));   // no format

    std::vector<Entry> e = {{"manifest.txt", "m"}, {"data/layouts/a.ini", "hello world"}, {"secrets.enc", "s"}};
    CHECK(checkManifestAgainst(m, e).empty());
    e.push_back({"data/other.txt", "x"});
    CHECK(!checkManifestAgainst(m, e).empty());                       // an unlisted entry
    e.pop_back();
    e[1].data = "short";
    CHECK(!checkManifestAgainst(m, e).empty());                       // size differs
    e[1].data = "hello world";
    e.pop_back();
    CHECK(!checkManifestAgainst(m, e).empty());                       // secrets.enc missing
    Manifest bad = m; bad.files.push_back({"../x", 1, "layouts"});
    CHECK(!checkManifestAgainst(bad, e).empty());
    Manifest bad2 = m; bad2.files.push_back({"x", 1, "nosuchitem"});
    CHECK(!checkManifestAgainst(bad2, e).empty());
    // a file outside the places its item may use (here: the codecs folder, whose libraries are loaded at run time) is refused
    Manifest bad3 = m; bad3.files.push_back({"codecs/libmbe.dylib", 4, "layouts"});
    std::vector<Entry> e3 = e; e3.push_back({"data/codecs/libmbe.dylib", "evil"}); e3.push_back({"secrets.enc", "s"});
    CHECK(!checkManifestAgainst(bad3, e3).empty());

    std::string msg;
    Manifest v = m;
    CHECK(checkVersion(v, "0.3.0", msg) == VersionState::Ok);
    CHECK(checkVersion(v, "0.3.1", msg) == VersionState::Ok);
    CHECK(checkVersion(v, "0.2.9", msg) == VersionState::Warn && !msg.empty());
    v.format = 2;
    CHECK(checkVersion(v, "0.3.0", msg) == VersionState::Refuse);
    v.format = 0;
    CHECK(checkVersion(v, "0.3.0", msg) == VersionState::Refuse);
    CHECK(compareVersions("0.10.0", "0.9.9") > 0 && compareVersions("1.0", "1.0.0") == 0 && compareVersions("0.2.9", "3.0") < 0);
}

static void testSelection() {
    CHECK(itemOfPath("layouts/x.ini") == "layouts");
    CHECK(itemOfPath("memories.json") == "memories");
    CHECK(itemOfPath("my.onairpreset") == "presets");
    CHECK(itemOfPath("sub/my.onairpreset").empty());
    CHECK(itemOfPath("presets.json") == "presets");
    CHECK(itemOfPath("voicekeys/keys.oavk") == "voicekeys");
    CHECK(itemOfPath("codecs/onair-codec-ambe.dylib").empty());   // codec binaries are never part of a backup
    CHECK(itemOfPath("recordings/x.cf32").empty());
    CHECK(findItem("voicekeys") && findItem("voicekeys")->secret && !findItem("layouts")->secret && !findItem("nope"));

    Manifest m;
    m.files = {{"layouts/a.ini", 1, "layouts"}, {"memories.json", 1, "memories"}, {"onair_ham.adi", 1, "logbook"}};
    std::set<std::string> ids = {"layouts", "logbook"}, ex = {"layouts/a.ini"};
    auto plan = planRestore(m, ids, Mode::Replace, ex);
    CHECK(plan.size() == 2 && plan[0].kind == PlanOp::Write && plan[0].existed && plan[1].name == "onair_ham.adi");
    plan = planRestore(m, ids, Mode::Merge, ex);
    CHECK(plan.size() == 2 && plan[0].kind == PlanOp::SkipExisting && plan[1].kind == PlanOp::Write);
    CHECK(planRestore(m, {}, Mode::Replace, ex).empty());

    std::vector<PrefItem> in = {{"freqMhz", 'D', "100.5"}, {"mode", 'S', "a\tb\nc\\d"}, {"channel", 'C', "1|2|x"}};
    std::vector<PrefItem> back;
    CHECK(parsePrefs(serializePrefs(in), back) && back.size() == 3 && back[1].value == "a\tb\nc\\d" && back[2].type == 'C');
    CHECK(!parsePrefs("junk", back));
    CHECK(!parsePrefs("OnAir settings 1\nX\tk\tv\n", back));
    CHECK(selectPrefs(in, Mode::Replace, {"freqMhz"}).size() == 3);
    auto mp = selectPrefs(in, Mode::Merge, {"freqMhz"});
    CHECK(mp.size() == 2 && mp[0].key == "mode");

    CHECK(isSecretPrefKey("ham.up.qrzKey") && isSecretPrefKey("ham.up.clubPw") && isSecretPrefKey("ham.up.eqslPw") && isSecretPrefKey("feed.token") && isSecretPrefKey("x.password"));
    CHECK(isSecretPrefKey("ham.up.clubApi") && isSecretPrefKey("something.apiKey"));
    CHECK(!isSecretPrefKey("freqMhz") && !isSecretPrefKey("specxBookmarks") && !isSecretPrefKey("ham.up.qrz"));
    std::vector<PrefItem> all = {{"ham.up.qrzKey", 'S', "SECRET1"}, {"freqMhz", 'D', "1"}, {"channel", 'C', "x"}}, plain, sec;
    splitSecretPrefs(all, plain, sec);
    CHECK(plain.size() == 2 && sec.size() == 1 && sec[0].value == "SECRET1");
    CHECK(serializePrefs(plain).find("SECRET1") == std::string::npos);
}

static void testCrypto() {
    // PBKDF2-HMAC-SHA256 vectors (RFC 7914 / common test vectors)
    uint8_t k[32];
    const uint8_t salt[4] = {'s', 'a', 'l', 't'};
    pbkdf2Sha256("password", salt, 4, 1, k, 32);
    CHECK(dect2::meshcrypto::hexEncode(k, 32) == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    pbkdf2Sha256("password", salt, 4, 2, k, 32);
    CHECK(dect2::meshcrypto::hexEncode(k, 32) == "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");

    const std::string plain = "voicekey=TOPSECRET123\nqrz=KEY";
    std::string blob, back;
    CHECK(sealSecrets(plain, "short", blob, 2000).size() > 0);                 // too short
    CHECK(sealSecrets(plain, "correct horse", blob, 2000).empty());
    CHECK(blob.find("TOPSECRET123") == std::string::npos);
    CHECK(openSecrets(blob, "correct horse", back).empty() && back == plain);
    CHECK(!openSecrets(blob, "wrong passphrase", back).empty() && back.empty());
    CHECK(!openSecrets(blob, "", back).empty());
    for (size_t i = 0; i < blob.size(); i++) { std::string m = blob; m[i] ^= 1; CHECK(!openSecrets(m, "correct horse", back).empty()); }   // any change is caught
    CHECK(!openSecrets(blob.substr(0, blob.size() - 1), "correct horse", back).empty());
    CHECK(!openSecrets(blob.substr(0, 20), "correct horse", back).empty());
    CHECK(!openSecrets("", "correct horse", back).empty());
    std::string blob2;
    CHECK(sealSecrets(plain, "correct horse", blob2, 2000).empty() && blob2 != blob);   // fresh salt and iv each time
    // absurd round counts in a forged header are refused, not run
    { std::string m = blob; m[6 + 16] = (char)0xFF; m[6 + 16 + 1] = (char)0xFF; m[6 + 16 + 2] = (char)0xFF; m[6 + 16 + 3] = (char)0x7F; CHECK(!openSecrets(m, "correct horse", back).empty()); }

    std::vector<Entry> e = {{"voicekeys/keys.oavk", std::string("\0\1\2", 3)}, {"settings-secrets.txt", "x"}}, e2;
    CHECK(unpackEntries(packEntries(e), e2) && e2.size() == 2 && e2[0].data == e[0].data);
    CHECK(!unpackEntries(packEntries(e).substr(0, 10), e2));
    CHECK(!unpackEntries(packEntries({{"../x", "y"}}), e2));
    std::string forged = packEntries(e); forged[0] = (char)0xFF; forged[1] = (char)0xFF;
    CHECK(!unpackEntries(forged, e2));
}

static void testExtract() {
    const fs::path root = fs::temp_directory_path() / "onair_backup_test_root";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    std::vector<Entry> ent = {{"data/layouts/a.ini", "A"}, {"data/memories.json", "M"}, {"data/../evil.txt", "E"}};
    std::vector<PlanOp> plan = {{"layouts/a.ini", PlanOp::Write, false}, {"memories.json", PlanOp::SkipExisting, true}, {"../evil.txt", PlanOp::Write, false},
                                {"/abs.txt", PlanOp::Write, false}, {"missing.txt", PlanOp::Write, false}};
    ExtractReport dry = extractTo(root.string(), ent, plan, true);
    CHECK(dry.written == 1 && dry.skipped == 1 && dry.errors.size() == 3);
    CHECK(!fs::exists(root / "layouts" / "a.ini"));                  // a dry run writes nothing
    ExtractReport r = extractTo(root.string(), ent, plan, false);
    CHECK(r.written == 1 && fs::exists(root / "layouts" / "a.ini") && !fs::exists(root / "memories.json"));
    CHECK(!fs::exists(root.parent_path() / "evil.txt"));
    { std::ifstream in(root / "layouts" / "a.ini"); std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); CHECK(s == "A"); }   // closed: Windows cannot replace an open file
    CHECK(!fs::exists(root / "layouts" / "a.ini.onairtmp"));          // atomic: no temp file left
    // replace overwrites
    ent[0].data = "B";
    plan = {{"layouts/a.ini", PlanOp::Write, true}};
    CHECK(extractTo(root.string(), ent, plan, false).written == 1);
    { std::ifstream i2(root / "layouts" / "a.ini"); std::string t((std::istreambuf_iterator<char>(i2)), std::istreambuf_iterator<char>()); CHECK(t == "B"); }
#ifndef _WIN32
    // a symlink inside the data folder must not lead the write out of it
    const fs::path outside = fs::temp_directory_path() / "onair_backup_test_outside";
    fs::remove_all(outside, ec); fs::create_directories(outside, ec);
    fs::create_directory_symlink(outside, root / "link", ec);
    ExtractReport sl = extractTo(root.string(), {{"data/link/pwn.txt", "P"}}, {{"link/pwn.txt", PlanOp::Write, false}}, false);
    CHECK(sl.written == 0 && sl.errors.size() == 1 && !fs::exists(outside / "pwn.txt"));
    fs::remove_all(outside, ec);
#endif
    fs::remove_all(root, ec);
}

static void testAuto() {
    CHECK(autoBackupName(0) == "onair-auto-19700101-000000.onairbackup");
    CHECK(autoBackupName(1792663200) == "onair-auto-20261022-100000.onairbackup");
    CHECK(isAutoBackupName(autoBackupName(1792663200)));
    CHECK(!isAutoBackupName("my-backup.onairbackup") && !isAutoBackupName("onair-auto-2026.onairbackup") && !isAutoBackupName("onair-auto-20261022x100000.onairbackup"));
    CHECK(autoBackupDue(0, 100, 7));
    CHECK(!autoBackupDue(1000, 1000 + 6 * 86400, 7));
    CHECK(autoBackupDue(1000, 1000 + 7 * 86400, 7));
    CHECK(!autoBackupDue(5000, 1000, 7));
    std::vector<std::string> n = {autoBackupName(300000), autoBackupName(100000), "mine.onairbackup", autoBackupName(200000), autoBackupName(400000)};
    auto del = autoBackupsToDelete(n, 2);
    CHECK(del.size() == 2 && del[0] == autoBackupName(100000) && del[1] == autoBackupName(200000));
    CHECK(autoBackupsToDelete(n, 10).empty());
    CHECK(autoBackupsToDelete(n, 0).size() == 3);   // keep is at least one
}

static void testMalicious() {
    // every file of a crafted manifest must sit where its item keeps files, never in a code or library location
    auto mk = [](const std::string& name, const std::string& item) {
        Manifest m; m.format = 1; m.app = "3.0";
        m.files = {{name, 4, item}};
        return m;
    };
    auto entries = [](const std::string& name) { return std::vector<Entry>{{"manifest.txt", "m"}, {"data/" + name, "evil"}}; };
    const char* bad[] = {"codecs/libmbe.dylib", "codecs/libmbe.so", "codecs/x.dll", "plugins/a.so", "Codecs/lib.dylib", "layouts/evil.dylib", "layouts/evil.dll", "layouts/run.sh",
                         "layouts/a.exe", "layouts/libx.so.1", "automations/x.so", "automations/x.bat", "layouts/a.ini.onairtmp"};
    for (const char* n : bad) {
        for (const char* item : {"layouts", "presets", "scheduler", "codecs", "settings"}) {
            Manifest m = mk(n, item);
            CHECK(!checkManifestAgainst(m, entries(n)).empty());   // wrong item, or a forbidden place or extension
        }
    }
    CHECK(!restorablePath("codecs/libmbe.dylib") && !restorablePath("plugins/x") && !restorablePath("a/b.dll") && !restorablePath("../x"));
    CHECK(restorablePath("layouts/a.ini") && restorablePath("memories.json") && restorablePath("automations/rule.json"));
    // an item that does not match the file's place
    CHECK(!checkManifestAgainst(mk("memories.json", "layouts"), entries("memories.json")).empty());
    CHECK(checkManifestAgainst(mk("memories.json", "memories"), entries("memories.json")).empty());
    // the same file listed twice
    { Manifest m = mk("memories.json", "memories"); m.files.push_back(m.files[0]); CHECK(!checkManifestAgainst(m, entries("memories.json")).empty()); }
    // a plan and the writer refuse a forbidden path even when the caller builds the plan by hand
    const fs::path root = fs::temp_directory_path() / "onair_backup_test_mal";
    std::error_code ec;
    fs::remove_all(root, ec); fs::create_directories(root, ec);
    std::vector<Entry> ent = {{"data/codecs/libmbe.dylib", "evil"}, {"data/plugins/p.so", "evil"}, {"data/layouts/ok.ini", "ok"}};
    std::vector<PlanOp> plan = {{"codecs/libmbe.dylib", PlanOp::Write, false}, {"plugins/p.so", PlanOp::Write, false}, {"layouts/ok.ini", PlanOp::Write, false}};
    ExtractReport r = extractTo(root.string(), ent, plan, false);
    CHECK(r.written == 1 && r.errors.size() == 2 && !fs::exists(root / "codecs") && !fs::exists(root / "plugins") && fs::exists(root / "layouts" / "ok.ini"));
    // planRestore drops files that do not belong to the item
    { Manifest m = mk("codecs/libmbe.dylib", "layouts"); CHECK(planRestore(m, {"layouts"}, Mode::Replace, {}).empty()); }
#ifndef _WIN32
    // the target itself a symlink: refused, nothing written through it
    const fs::path outside = fs::temp_directory_path() / "onair_backup_test_mal_out";
    fs::remove_all(outside, ec); fs::create_directories(outside, ec);
    { std::ofstream o(outside / "victim.txt"); o << "keep"; }
    fs::create_directories(root / "layouts", ec);
    fs::create_symlink(outside / "victim.txt", root / "layouts" / "link.ini", ec);
    ExtractReport sl = extractTo(root.string(), {{"data/layouts/link.ini", "PWN"}}, {{"layouts/link.ini", PlanOp::Write, true}}, false);
    CHECK(sl.written == 0 && sl.errors.size() == 1);
    { std::ifstream i(outside / "victim.txt"); std::string t((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>()); CHECK(t == "keep"); }
    fs::remove_all(outside, ec);
#endif
    fs::remove_all(root, ec);
}

static void testRestorePrefs() {
    // exposure, servers, uploads, transmit, paths and consents need a tick; unknown keys too; credentials in the plain part are dropped
    for (const char* k : {"apiOn", "apiLan", "apiPort", "shareOn", "shareLan", "shareControl", "feedLan", "feedAprsServer", "monNet", "monBindAll", "iridiumNetHost", "mapServer", "rcUrl",
                          "ham.up.qrz", "ham.up.auto", "automationAllowRun", "pluginsSafeMode", "txEnabled", "txNoticeAccepted", "tx.kaal.text", "tvrec_dir", "vi.accepted", "rotHost",
                          "a_brand_new_key", "update.channel"})
        CHECK(prefRestorePolicy(k, 'S', false) == PrefPolicy::Confirm);
    for (const char* k : {"apiToken", "ham.up.qrzKey", "ham.up.clubPw", "feedAprsPass", "someSecret", "x_password"}) CHECK(prefRestorePolicy(k, 'S', false) == PrefPolicy::Never);
    CHECK(prefRestorePolicy("", 'S', false) == PrefPolicy::Never && prefRestorePolicy("a b", 'S', false) == PrefPolicy::Never);
    CHECK(prefRestorePolicy(std::string(200, 'a'), 'S', false) == PrefPolicy::Never);
    for (const char* k : {"wfMin", "specMax", "genGain", "uiTypography", "adsbZoom", "freqFav"}) CHECK(prefRestorePolicy(k, 'D', false) == PrefPolicy::Allow);
    CHECK(prefRestorePolicy("channel", 'C', false) == PrefPolicy::Allow);
    // the passphrase block never applies silently, even for a harmless name
    CHECK(prefRestorePolicy("wfMin", 'D', true) == PrefPolicy::Confirm && prefRestorePolicy("ham.up.qrzKey", 'S', true) == PrefPolicy::Confirm);
    std::vector<PrefItem> in = {{"wfMin", 'D', "1"}, {"apiOn", 'B', "1"}, {"apiToken", 'S', "x"}, {"mapServer", 'S', "http://evil"}, {"channel", 'C', "1|1|a|b|0|0|0"}};
    std::vector<PrefItem> allow, confirm; std::vector<std::string> refused;
    classifyRestorePrefs(in, false, allow, confirm, refused);
    CHECK(allow.size() == 2 && confirm.size() == 2 && refused.size() == 1 && refused[0] == "apiToken");
}

int main() {
    testMalicious();
    testRestorePrefs();
    testPaths();
    testZip();
    testManifest();
    testSelection();
    testCrypto();
    testExtract();
    testAuto();
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("backup OK\n");
    return 0;
}
