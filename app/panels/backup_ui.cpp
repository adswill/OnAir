#include "backup_ui.h"
#include "../i18n/i18n.h"
#include "../ui/scale.h"
#include "dect2/backup/backup.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace fs = std::filesystem;
namespace bk = dect2::backup;

namespace {

constexpr uintmax_t kMaxFile = 256u << 20;   // a single data file larger than this is left out (and said so)

struct State {
    // create
    std::map<std::string, bool> tick;       // item id -> ticked
    bool secrets = false;
    char pass[128] = {}, pass2[128] = {};
    std::string msg;                         // result of the last action (never holds a secret)
    bool msgBad = false;
    // restore
    std::string path;
    std::string raw;                         // the archive bytes as read
    std::vector<bk::Entry> entries;          // unpacked
    bk::Manifest man;
    bool loaded = false;
    std::string verMsg;
    bk::VersionState ver = bk::VersionState::Ok;
    std::map<std::string, bool> rtick;
    int mode = 0;                            // 0 merge, 1 replace
    char rpass[128] = {};
    bool unlocked = false;                   // secrets decrypted into secretEntries / secretPrefs
    std::vector<bk::Entry> secretEntries;
    std::vector<bk::PrefItem> secretPrefs;
    std::map<std::string, bool> confirmTick; // settings that need an explicit tick before a restore applies them (key -> ticked)
    std::string dry;                         // dry-run report
    bool confirmReplace = false;
    bool confirmReset = false;
    // automatic
    bool autoOn = false;
    int keep = 4;
    double nextCheck = 0;
    bool init = false;
};
State& S() { static State s; return s; }

std::string dataRoot() { return plat::dataDir(); }
std::string backupsDir() { return dataRoot() + "/backups"; }

bool readAll(const fs::path& p, std::string& out, uintmax_t maxBytes) {
    std::error_code ec;
    const uintmax_t n = fs::file_size(p, ec);
    if (ec || n > maxBytes) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign((size_t)n, '\0');
    in.read(&out[0], (std::streamsize)n);
    return (bool)in;
}

int64_t nowEpoch() { return (int64_t)std::time(nullptr); }

std::string isoNow() {
    const time_t t = std::time(nullptr);
    char b[32];
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
    return b;
}

// All data-folder files (relative, '/') that belong to item `d`. Symlinks are not followed.
void itemFiles(const bk::ItemDef& d, std::vector<std::string>& out) {
    const fs::path root = dect2::u8file(dataRoot());
    std::error_code ec;
    auto add = [&](const fs::path& p) {
        if (!fs::is_regular_file(p, ec) || fs::is_symlink(p, ec)) return;
        std::string rel = fs::relative(p, root, ec).generic_string();
        if (!ec && bk::safeRelPath(rel)) out.push_back(rel);
    };
    for (const std::string& pat : d.paths) {
        if (pat.empty()) continue;
        if (pat[0] == '*') {
            const std::string ext = pat.substr(1);
            for (fs::directory_iterator it(root, ec), e; !ec && it != e; it.increment(ec)) {
                const std::string n = it->path().filename().generic_string();
                if (n.size() > ext.size() && n.compare(n.size() - ext.size(), ext.size(), ext) == 0) add(it->path());
            }
        } else if (pat.back() == '/') {
            const fs::path dir = root / dect2::u8file(pat.substr(0, pat.size() - 1));
            if (!fs::is_directory(dir, ec) || fs::is_symlink(dir, ec)) continue;
            for (fs::recursive_directory_iterator it(dir, ec), e; !ec && it != e; it.increment(ec)) add(it->path());
        } else add(root / dect2::u8file(pat));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

std::vector<bk::PrefItem> collectPrefs() {
    std::vector<bk::PrefItem> all = plat::basePrefs().dumpAll();
    std::sort(all.begin(), all.end(), [](const bk::PrefItem& x, const bk::PrefItem& y) { return x.key < y.key; });
    for (const plat::Channel& c : plat::basePrefs().getChannels()) {
        char b[512];
        std::string nm = c.name, md = c.mode;
        for (char& ch : nm) if (ch == '|' || ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        for (char& ch : md) if (ch == '|' || ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        snprintf(b, sizeof b, "%.9g|%.9g|%s|%s|%.9g|%d|%d", c.freqMhz, c.bwMhz, nm.c_str(), md.c_str(), (double)c.snrDb, c.nServices, c.favourite ? 1 : 0);
        all.push_back({"channel", 'C', b});
    }
    return all;
}

std::string codecListing() {
    std::string o = "Plug-in codecs found in the OnAir codecs folder when this backup was made. The codec files themselves are not part of a backup:\n"
                    "install them again from your own source (see docs/CODECS.md).\n";
    std::error_code ec;
    std::vector<std::string> names;
    for (fs::directory_iterator it(dect2::u8file(dataRoot() + "/codecs"), ec), e; !ec && it != e; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        names.push_back(it->path().filename().generic_string() + "\t" + std::to_string(fs::file_size(it->path(), ec)) + " bytes");
    }
    std::sort(names.begin(), names.end());
    for (auto& n : names) o += n + "\n";
    if (names.empty()) o += "(none)\n";
    return o;
}

// Builds the archive bytes. secrets only when withSecrets and the passphrase is accepted. `err` is set on failure; `left` lists files skipped.
bool buildArchive(const std::set<std::string>& ids, bool withSecrets, const std::string& pass, std::string& bytes, std::string& err, std::vector<std::string>& left) {
    bk::Manifest m;
    m.format = bk::kFormat; m.app = ONAIR_VERSION; m.created = isoNow(); m.secrets = false;
    std::vector<bk::Entry> out, secretEntries;
    std::vector<bk::PrefItem> prefs = collectPrefs(), plain, secretPrefs;
    bk::splitSecretPrefs(prefs, plain, secretPrefs);
    if (withSecrets) {
        std::string why;
        if (!bk::passphraseOk(pass, why)) { err = why; return false; }
    }
    for (const bk::ItemDef& d : bk::catalog()) {
        if (!ids.count(d.id)) continue;
        if (std::string(d.id) == "settings") {
            const std::string t = bk::serializePrefs(plain);
            out.push_back({"settings.txt", t});
            m.items.push_back({d.id, 1, t.size()});
            if (withSecrets && !secretPrefs.empty()) secretEntries.push_back({"settings-secrets.txt", bk::serializePrefs(secretPrefs)});
            continue;
        }
        if (std::string(d.id) == "codecs") {
            const std::string t = codecListing();
            out.push_back({"codecs-listing.txt", t});
            m.items.push_back({d.id, 1, t.size()});
            continue;
        }
        if (d.secret && !withSecrets) continue;
        std::vector<std::string> files;
        itemFiles(d, files);
        bk::Manifest::Item it{d.id, 0, 0};
        for (const std::string& rel : files) {
            std::string data;
            if (!readAll(dect2::u8file(dataRoot() + "/" + rel), data, kMaxFile)) { left.push_back(rel); continue; }
            m.files.push_back({rel, data.size(), d.id});
            it.files++; it.bytes += data.size();
            if (d.secret) secretEntries.push_back({rel, std::move(data)});
            else out.push_back({"data/" + rel, std::move(data)});
        }
        if (it.files) m.items.push_back(it);
    }
    if (withSecrets && !secretEntries.empty()) {
        std::string blob;
        err = bk::sealSecrets(bk::packEntries(secretEntries), pass, blob);
        if (!err.empty()) return false;
        out.push_back({"secrets.enc", blob});
        m.secrets = true;
    }
    std::vector<bk::Entry> all;
    all.push_back({"manifest.txt", bk::writeManifest(m)});
    for (auto& e : out) all.push_back(std::move(e));
    bytes = bk::zipPack(all);
    return true;
}

bool writeArchive(const std::string& path, const std::string& bytes) { return bk::writeFileAtomic(path, bytes); }

std::set<std::string> defaultIds() {
    std::set<std::string> s;
    for (const bk::ItemDef& d : bk::catalog()) if (!d.secret) s.insert(d.id);
    return s;
}

// ---- automatic weekly backup (no secrets) ----
void autoTick(App&) {
    State& s = S();
    if (!s.init) {
        s.autoOn = plat::prefs().getB("backup.auto", false);
        s.keep = (int)std::max(1L, std::min(50L, plat::prefs().getI("backup.keep", 4)));
        s.init = true;
    }
    const double t = ImGui::GetTime();
    if (!s.autoOn || t < 180 || t < s.nextCheck) return;
    s.nextCheck = t + 3600;
    const int64_t now = nowEpoch();
    if (!bk::autoBackupDue((int64_t)plat::prefs().getD("backup.last", 0), now, 7)) return;
    std::string bytes, err; std::vector<std::string> left;
    if (!buildArchive(defaultIds(), false, "", bytes, err, left)) return;
    std::error_code ec;
    fs::create_directories(dect2::u8file(backupsDir()), ec);
    if (!writeArchive(backupsDir() + "/" + bk::autoBackupName(now), bytes)) return;
    plat::prefs().setD("backup.last", (double)now);
    std::vector<std::string> names;
    for (fs::directory_iterator it(dect2::u8file(backupsDir()), ec), e; !ec && it != e; it.increment(ec)) names.push_back(it->path().filename().generic_string());
    for (const std::string& n : bk::autoBackupsToDelete(names, s.keep)) fs::remove(dect2::u8file(backupsDir() + "/" + n), ec);
}

// ---- restore ----
void loadArchive(State& s, const std::string& path) {
    s.loaded = false; s.dry.clear(); s.unlocked = false; s.secretEntries.clear(); s.secretPrefs.clear(); s.rpass[0] = 0; s.confirmReplace = false; s.confirmTick.clear();
    s.msg.clear(); s.msgBad = true;
    if (!readAll(dect2::u8file(path), s.raw, (uintmax_t)bk::Limits().maxTotalBytes + (64u << 20))) { s.msg = tr("That file could not be read (or it is too large)."); return; }
    std::string e = bk::zipUnpack(s.raw, s.entries);
    if (!e.empty()) { s.msg = std::string(tr("Not a usable backup: ")) + e; return; }
    auto mi = std::find_if(s.entries.begin(), s.entries.end(), [](const bk::Entry& x) { return x.name == "manifest.txt"; });
    if (mi == s.entries.end() || !bk::parseManifest(mi->data, s.man)) { s.msg = tr("Not an OnAir backup (no readable manifest)."); return; }
    e = bk::checkManifestAgainst(s.man, s.entries);
    if (!e.empty()) { s.msg = std::string(tr("The backup does not match its manifest: ")) + e; return; }
    s.ver = bk::checkVersion(s.man, ONAIR_VERSION, s.verMsg);
    if (s.ver == bk::VersionState::Refuse) { s.msg = s.verMsg; return; }
    s.rtick.clear();
    for (const auto& it : s.man.items) s.rtick[it.id] = it.id != "codecs" && it.id != "voicekeys";
    s.path = path; s.loaded = true; s.msg.clear(); s.msgBad = false;
}

bool unlock(State& s) {
    s.secretEntries.clear(); s.secretPrefs.clear();
    auto it = std::find_if(s.entries.begin(), s.entries.end(), [](const bk::Entry& x) { return x.name == "secrets.enc"; });
    if (it == s.entries.end()) return true;
    std::string plain, e = bk::openSecrets(it->data, s.rpass, plain);
    if (!e.empty()) { s.msg = std::string(tr("Secrets: ")) + e; s.msgBad = true; return false; }
    if (!bk::unpackEntries(plain, s.secretEntries)) { s.msg = tr("The secrets block is damaged."); s.msgBad = true; return false; }
    for (const auto& x : s.secretEntries) if (x.name == "settings-secrets.txt") bk::parsePrefs(x.data, s.secretPrefs);
    std::fill(plain.begin(), plain.end(), '\0');
    s.unlocked = true;
    return true;
}

std::vector<bk::PlanOp> makePlan(State& s, bool& needSecrets) {
    std::set<std::string> ids;
    for (auto& t : s.rtick) if (t.second) ids.insert(t.first);
    needSecrets = ids.count("voicekeys") || ids.count("settings") ? s.man.secrets : false;
    std::set<std::string> existing;
    for (const auto& f : s.man.files) {
        std::error_code ec;
        if (fs::exists(dect2::u8file(dataRoot() + "/" + f.name), ec)) existing.insert(f.name);
    }
    return bk::planRestore(s.man, ids, s.mode == 0 ? bk::Mode::Merge : bk::Mode::Replace, existing);
}

void applyPref(const bk::PrefItem& p) {
    plat::Prefs& b = plat::basePrefs();
    switch (p.type) {
        case 'D': b.setD(p.key.c_str(), atof(p.value.c_str())); break;
        case 'I': b.setI(p.key.c_str(), atol(p.value.c_str())); break;
        case 'B': b.setB(p.key.c_str(), p.value == "1"); break;
        default: b.setS(p.key.c_str(), p.value); break;
    }
}

// Settings of the backup split by what a restore may do with them (see bk::prefRestorePolicy).
void restorePrefSets(State& s, std::vector<bk::PrefItem>& allow, std::vector<bk::PrefItem>& confirm, std::vector<std::string>& refused) {
    auto e = std::find_if(s.entries.begin(), s.entries.end(), [](const bk::Entry& x) { return x.name == "settings.txt"; });
    std::vector<bk::PrefItem> plain;
    if (e != s.entries.end() && bk::parsePrefs(e->data, plain)) bk::classifyRestorePrefs(plain, false, allow, confirm, refused);
    if (s.unlocked) bk::classifyRestorePrefs(s.secretPrefs, true, allow, confirm, refused);
}

std::string doRestore(State& s, bool dryRun) {
    bool needSecrets = false;
    std::vector<bk::PlanOp> plan = makePlan(s, needSecrets);
    const bool wantSettings = s.rtick["settings"], wantKeys = s.rtick["voicekeys"];
    const bk::Mode mode = s.mode == 0 ? bk::Mode::Merge : bk::Mode::Replace;
    if ((wantKeys || (wantSettings && s.man.secrets)) && !s.unlocked) {
        if (wantKeys || s.rpass[0]) { if (!unlock(s)) return s.msg; }
    }
    // the files: normal ones from the archive entries, voice keys from the decrypted block
    std::vector<bk::Entry> src = s.entries;
    for (const auto& e : s.secretEntries) if (e.name != "settings-secrets.txt") src.push_back({"data/" + e.name, e.data});
    plan.erase(std::remove_if(plan.begin(), plan.end(), [&](const bk::PlanOp& o) {
        const std::string it = bk::itemOfPath(o.name);
        return it == "voicekeys" && !s.unlocked;
    }), plan.end());
    // settings
    std::vector<bk::PrefItem> prefs;
    std::set<std::string> have;
    std::string prefNote;
    if (wantSettings) {
        auto e = std::find_if(s.entries.begin(), s.entries.end(), [](const bk::Entry& x) { return x.name == "settings.txt"; });
        { std::vector<bk::PrefItem> chk; if (e != s.entries.end() && !bk::parsePrefs(e->data, chk)) return tr("settings.txt in the backup is damaged."); }
        std::vector<bk::PrefItem> allow, confirm;
        std::vector<std::string> refused;
        restorePrefSets(s, allow, confirm, refused);
        prefs = allow;
        int skippedConfirm = 0;
        for (const bk::PrefItem& p : confirm) { auto t = s.confirmTick.find(p.key); if (t != s.confirmTick.end() && t->second) prefs.push_back(p); else skippedConfirm++; }
        for (const auto& k : plat::basePrefs().dumpAll()) have.insert(k.key);
        prefs = bk::selectPrefs(prefs, mode, have);
        prefNote = std::to_string(skippedConfirm) + " " + tr("settings that need a tick were left out") + ", " + std::to_string(refused.size()) + " " + tr("refused");
    }
    if (dryRun) {
        bk::ExtractReport r = bk::extractTo(dataRoot(), src, plan, true);
        std::string o = std::to_string(r.written) + " " + tr("files would be written") + ", " + std::to_string(r.skipped) + " " + tr("kept as they are") + ", " + std::to_string(prefs.size()) + " " + tr("settings would be set") + ". " + prefNote;
        for (const auto& e : r.errors) o += "\n" + e;
        return o;
    }
    // safety backup of the current state first (no secrets), kept in the backups folder
    {
        std::set<std::string> all;
        for (const bk::ItemDef& d : bk::catalog()) if (!d.secret) all.insert(d.id);
        std::string bytes, err; std::vector<std::string> left;
        std::error_code ec;
        fs::create_directories(dect2::u8file(backupsDir()), ec);
        const std::string safety = backupsDir() + "/onair-before-restore-" + bk::autoBackupName(nowEpoch()).substr(11);
        if (!buildArchive(all, false, "", bytes, err, left) || !writeArchive(safety, bytes))
            return std::string(tr("Could not make the safety backup, so nothing was restored: ")) + err;
    }
    bk::ExtractReport r = bk::extractTo(dataRoot(), src, plan, false);
    int nPref = 0;
    plat::Prefs& b = plat::basePrefs();
    std::vector<plat::Channel> chans;
    bool haveChannels = false;
    for (const bk::PrefItem& p : prefs) {
        if (p.type == 'C') {
            plat::Channel c; std::vector<std::string> f; std::string t;
            for (char ch : p.value) { if (ch == '|') { f.push_back(t); t.clear(); } else t += ch; }
            f.push_back(t);
            if (f.size() >= 7) {
                c.freqMhz = atof(f[0].c_str()); c.bwMhz = atof(f[1].c_str()); c.name = f[2]; c.mode = f[3];
                c.snrDb = (float)atof(f[4].c_str()); c.nServices = atoi(f[5].c_str()); c.favourite = f[6] == "1";
                if (c.freqMhz > 0) chans.push_back(c);
            }
            haveChannels = true;
        } else { applyPref(p); nPref++; }
    }
    if (haveChannels && (mode == bk::Mode::Replace || b.getChannels().empty())) b.setChannels(chans);
    b.flush();
    std::string o = std::to_string(r.written) + " " + tr("files restored") + ", " + std::to_string(nPref) + " " + tr("settings set") + ". " + prefNote + " " +
                    tr("Close OnAir and start it again now, without changing anything first: a running OnAir would write its old settings over the restored ones.");
    for (const auto& e : r.errors) o += "\n" + e;
    return o;
}

void helpText(const char* t) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(t)); }

} // namespace

void backupWindow(App& a) {
    autoTick(a);
    State& s = S();
    if (!a.backupOpen) return;
    if (s.tick.empty()) for (const bk::ItemDef& d : bk::catalog()) s.tick[d.id] = !d.secret;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(700 * gUi, vp->WorkSize.x * 0.95f), std::min(760 * gUi, vp->WorkSize.y * 0.9f)), ImGuiCond_Appearing);
    if (!ImGui::Begin(trw("Backup and restore"), &a.backupOpen)) { ImGui::End(); return; }

    ImGui::SeparatorText(tr("Make a backup"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(tr("One .onairbackup file (a plain zip) with what you tick below. Recordings, models and the codec files are not included."));
    ImGui::PopTextWrapPos();
    for (const bk::ItemDef& d : bk::catalog()) {
        if (d.secret) continue;
        ImGui::Checkbox(tr(d.label), &s.tick[d.id]);
    }
    ImGui::Spacing();
    ImGui::Checkbox(tr("Include secrets (voice keys, QRZ / Club Log / eQSL and other credentials)"), &s.secrets);
    if (s.secrets) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.78f, 0.25f, 1));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(tr("Warning: secrets are encrypted with your passphrase (PBKDF2, AES-256, HMAC). Without the passphrase they cannot be recovered, and OnAir cannot reset it. Keep the file private. Voice keys that are protected by this computer's keychain will not open on another computer."));
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::InputText(tr("Passphrase"), s.pass, sizeof s.pass, ImGuiInputTextFlags_Password);
        ImGui::InputText(tr("Passphrase again"), s.pass2, sizeof s.pass2, ImGuiInputTextFlags_Password);
    }
    std::string why;
    const bool passOk = !s.secrets || (std::string(s.pass) == s.pass2 && bk::passphraseOk(s.pass, why));
    if (s.secrets && !passOk) {
        ImGui::TextDisabled("%s", std::string(s.pass) != s.pass2 ? tr("The two passphrases differ.") : tr("The passphrase needs at least 8 characters."));
    }
    ImGui::BeginDisabled(!passOk);
    if (ImGui::Button(tr("Save backup ..."))) {
        const std::string path = plat::saveFileDialog("onair-backup.onairbackup");
        if (!path.empty()) {
            std::set<std::string> ids;
            for (auto& t : s.tick) if (t.second) ids.insert(t.first);
            if (s.secrets) ids.insert("voicekeys");
            std::string bytes, err; std::vector<std::string> left;
            if (!buildArchive(ids, s.secrets, s.pass, bytes, err, left)) { s.msg = err; s.msgBad = true; }
            else if (!writeArchive(path, bytes)) { s.msg = tr("The file could not be written."); s.msgBad = true; }
            else {
                s.msg = std::string(tr("Backup saved: ")) + path + (s.secrets ? std::string(" (") + tr("secrets encrypted") + ")" : std::string(" (") + tr("no secrets") + ")");
                if (!left.empty()) s.msg += "; " + std::to_string(left.size()) + " " + tr("files were too large or unreadable and were left out");
                s.msgBad = false;
            }
        }
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::SeparatorText(tr("Automatic backup"));
    if (ImGui::Checkbox(tr("Back up once a week (without secrets)"), &s.autoOn)) plat::prefs().setB("backup.auto", s.autoOn);
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::InputInt(tr("Keep the newest"), &s.keep)) { s.keep = std::max(1, std::min(50, s.keep)); plat::prefs().setI("backup.keep", s.keep); }
    ImGui::TextDisabled("%s %s", tr("Folder:"), backupsDir().c_str());

    ImGui::Spacing();
    ImGui::SeparatorText(tr("Restore from a backup"));
    if (ImGui::Button(tr("Open backup ..."))) {
        const std::string p = plat::openFileDialog();
        if (!p.empty()) loadArchive(s, p);
    }
    if (s.loaded) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", s.path.c_str());
        ImGui::Text("%s %s, %s %s", tr("Made by OnAir"), s.man.app.c_str(), tr("on"), s.man.created.c_str());
        if (s.ver == bk::VersionState::Warn) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.78f, 0.25f, 1)); ImGui::TextWrapped("%s", s.verMsg.c_str()); ImGui::PopStyleColor(); }
        ImGui::TextUnformatted(tr("Tick what to restore:"));
        for (const auto& it : s.man.items) {
            const bk::ItemDef* d = bk::findItem(it.id);
            if (!d) continue;
            const bool info = it.id == "codecs";
            ImGui::BeginDisabled(info);
            char lbl[256]; snprintf(lbl, sizeof lbl, "%s  (%d %s, %llu KB)", tr(d->label), it.files, tr("files"), (unsigned long long)(it.bytes / 1024));
            ImGui::Checkbox(lbl, &s.rtick[it.id]);
            ImGui::EndDisabled();
        }
        if (s.man.secrets) {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", tr("This backup holds encrypted secrets (voice keys, credentials). Enter its passphrase to restore them; leave it empty to restore everything else."));
            ImGui::InputText(tr("Backup passphrase"), s.rpass, sizeof s.rpass, ImGuiInputTextFlags_Password);
            ImGui::SameLine();
            if (ImGui::Button(tr("Unlock"))) { if (unlock(s)) { s.msg = tr("Secrets unlocked."); s.msgBad = false; s.rtick["voicekeys"] = true; } }
            if (s.unlocked) ImGui::TextDisabled("%s", tr("Secrets unlocked."));
        }
        if (s.rtick["settings"]) {
            std::vector<bk::PrefItem> allow, confirm; std::vector<std::string> refused;
            restorePrefSets(s, allow, confirm, refused);
            if (!refused.empty()) ImGui::TextDisabled("%d %s", (int)refused.size(), tr("settings in this backup are never restored (credentials in the plain part, odd names)."));
            if (!confirm.empty()) {
                ImGui::Spacing();
                ImGui::TextWrapped("%s", tr("These settings can open the program to the network, change server addresses, uploads, transmit options or paths. They are NOT restored unless you tick them:"));
                for (const bk::PrefItem& p : confirm) ImGui::Checkbox(p.key.c_str(), &s.confirmTick[p.key]);
            }
        }
        ImGui::RadioButton(tr("Merge: keep what is here, add what is missing"), &s.mode, 0);
        ImGui::RadioButton(tr("Replace: overwrite with the backup's copy"), &s.mode, 1);
        if (ImGui::Button(tr("Dry run (change nothing)"))) { s.dry = doRestore(s, true); s.confirmReplace = false; }
        helpText("Shows how many files and settings would be written, without touching anything.");
        ImGui::SameLine();
        const bool replace = s.mode == 1;
        if (ImGui::Button(replace && !s.confirmReplace ? tr("Restore ... (overwrites)") : tr("Restore now"))) {
            if (replace && !s.confirmReplace) s.confirmReplace = true;
            else { s.msg = doRestore(s, false); s.msgBad = false; s.confirmReplace = false; }
        }
        helpText("A safety backup of the current state (without secrets) is saved in the backups folder first.");
        if (s.confirmReplace) ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", tr("Click Restore now again to overwrite. A safety backup is made first."));
        if (!s.dry.empty()) ImGui::TextWrapped("%s", s.dry.c_str());
    }
    ImGui::SeparatorText(tr("Reset all settings"));
    ImGui::TextWrapped("%s", tr("Starts OnAir again with the default settings: every mode on its own frequency, no saved layout or radio choice. Recordings, memories and other files in the data folder stay. The old settings are kept in a file."));
    if (settingsResetRequested()) {
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s", tr("The settings are reset the next time OnAir starts: close OnAir and open it again."));
        if (ImGui::Button(tr("Cancel the reset"))) requestSettingsReset(false);
    } else {
        if (ImGui::Button(s.confirmReset ? tr("Click again to reset all settings") : tr("Reset all settings ..."))) {
            if (!s.confirmReset) s.confirmReset = true;
            else { s.confirmReset = false; if (!requestSettingsReset(true)) { s.msg = tr("The reset could not be written to the data folder."); s.msgBad = true; } }
        }
        helpText("Also available as the start option --reset-settings. docs/RESET.md lists where OnAir keeps its settings.");
    }
    if (!s.msg.empty()) {
        ImGui::Spacing();
        if (s.msgBad) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1));
        ImGui::TextWrapped("%s", s.msg.c_str());
        if (s.msgBad) ImGui::PopStyleColor();
    }
    ImGui::End();
}
