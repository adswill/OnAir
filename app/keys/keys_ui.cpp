// The Keys window (Tools > Voice keys; also reached from the "encrypted: add your key" text of every voice panel): the user's own
// decryption keys for digital voice (docs/KEYS.md). Nothing is decrypted until the first-use notice is accepted. The window never
// writes a key to a log or the console; keys show masked unless the user presses Reveal; the logic is in keys_logic.*.
#include "keys_ui.h"
#include "keys_logic.h"
#include "../app.h"
#include "../platform/platform.h"
#include "../platform/keychain.h"
#include "../ui/scale.h"
#include "../ui/theme.h"
#include "dect2/common/u8file.h"
#include "dect2/voicekeys/key_guard.h"
#include "dect2/voicekeys/key_persist.h"
#include "dect2/voicekeys/voice_keys.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace vk = dect2::voicekeys;
using keyswin::CallStatus;
using keyswin::VoiceKey;
using keyswin::Protocol;

namespace {

constexpr const char* kLegalPref = "voicekeys.legal";

void wipe(std::string& s) { std::fill(s.begin(), s.end(), '\0'); s.clear(); }
void wipe(char* b, size_t n) { std::memset(b, 0, n); }

struct State {
    bool inited = false;
    std::string keychain;          // "" when the system has none (passphrase fallback)
    bool unlocked = false;         // keys loaded into the store (or an empty store ready to fill)
    bool haveFile = false;
    bool fileNeedsPass = false;
    std::string passphrase;        // kept in memory for the session while unlocked (passphrase fallback only)
    std::string error;             // last problem, shown at the top
    std::string notice;            // last success, shown at the top
    std::set<int> revealed;
    // dialogs
    bool openEdit = false, openDelete = false, openClear = false, openImport = false, openExport = false;
    int editIndex = -1, deleteIndex = -1;
    keyswin::KeyForm form;
    std::string formError;
    bool formReveal = false;
    keyswin::CsvPreview preview;
    std::string importError;
    bool importNeedsPass = false;
    std::string importBytes;
    char pass[128] = "", pass2[128] = "";
    char importPass[128] = "";
    bool exportEncrypted = true;
    std::string exportError;
};
State S;

std::string filePath() { return keyswin::storeFilePath(plat::dataDir()); }

bool readFile(const std::string& path, std::string& out, size_t maxBytes = 4u << 20) {
    std::ifstream in(dect2::u8file(path), std::ios::binary);
    if (!in) return false;
    std::stringstream ss; ss << in.rdbuf();
    out = ss.str();
    return out.size() <= maxBytes;
}

keyswin::StoreState state() {
    if (!vk::store().legalAccepted()) return keyswin::StoreState::NeedsNotice;
    return S.unlocked ? keyswin::StoreState::Unlocked : keyswin::StoreState::Locked;
}

// Can the store be written right now? With a keychain: yes. Without: only once a passphrase is set this session.
bool canSave() { return S.unlocked && (vk::haveSecretProvider() || !S.passphrase.empty()); }

// Read the file into the store. pass is the passphrase for a passphrase file ("" for keychain files).
void tryLoad(const std::string& pass) {
    S.error.clear();
    std::string bytes;
    S.haveFile = readFile(filePath(), bytes) && !bytes.empty();
    S.fileNeedsPass = false;
    if (!S.haveFile) { S.unlocked = true; return; }   // nothing stored yet: an empty store, ready to fill
    bool needs = false;
    if (!vk::fileNeedsPassphrase(bytes, needs)) { S.error = "The key file is not an OnAir key file."; S.unlocked = false; return; }
    S.fileNeedsPass = needs;
    if (needs && pass.empty()) { S.unlocked = false; return; }   // locked: ask for the passphrase
    const std::string err = vk::loadFromFile(filePath(), pass);
    if (!err.empty()) { S.error = err; S.unlocked = false; return; }
    S.unlocked = true;
    S.passphrase = needs ? pass : std::string();
}

// Write the store. On a keychain failure fall back to the passphrase route and say so.
bool persist() {
    if (!canSave()) { S.error = "Set a passphrase first: there is no OS keychain on this system."; return false; }
    std::error_code ec;
    std::filesystem::create_directories(dect2::u8file(filePath()).parent_path(), ec);
    const std::string err = vk::saveToFile(filePath(), S.passphrase);
    if (!err.empty()) {
        S.error = err;
        if (err.find("keychain") != std::string::npos) {
            vk::setSecretProvider({}, {}, {});   // the keychain does not answer: use a passphrase instead
            S.keychain.clear();
            S.error += ". Choose a passphrase below to keep your keys instead.";
        }
        return false;
    }
    S.haveFile = true;
    return true;
}

void lockNow() {
    vk::store().clear();
    wipe(S.passphrase);
    S.revealed.clear();
    S.unlocked = false;
    S.formReveal = false;
    wipe(S.form.hex);
    S.notice.clear();
    wipe(S.pass, sizeof S.pass); wipe(S.pass2, sizeof S.pass2);
}

void init() {
    if (S.inited) return;
    S.inited = true;
    S.keychain = plat::installKeychainProvider();
    vk::store().setLegalAccepted(plat::prefs().getB(kLegalPref, false));
    if (vk::store().legalAccepted()) tryLoad(std::string());
}

void centre(float w, float h) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(w * gUi, vp->WorkSize.x * 0.95f), std::min(h * gUi, vp->WorkSize.y * 0.9f)), ImGuiCond_Appearing);
}

void legalBody(App& a) {
    ImGui::TextColored(pal::warnAmber(), "%s", tr("Before you use voice keys"));
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    for (const auto& p : keyswin::legalNotice()) { ImGui::TextUnformatted(tr(p.c_str())); ImGui::Spacing(); }
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    if (ImGui::Button(tr("I understand and accept"))) {
        plat::prefs().setB(kLegalPref, true);
        plat::prefs().flush();
        vk::store().setLegalAccepted(true);
        tryLoad(std::string());
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Not now"))) a.voiceKeysOpen = false;
}

// A passphrase field pair for choosing a passphrase.
bool passphraseSetter(const char* button) {
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::InputText("##pp1", S.pass, sizeof S.pass, ImGuiInputTextFlags_Password);
    ImGui::SameLine(); ImGui::TextUnformatted(tr("Passphrase"));
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::InputText("##pp2", S.pass2, sizeof S.pass2, ImGuiInputTextFlags_Password);
    ImGui::SameLine(); ImGui::TextUnformatted(tr("Again"));
    bool done = false;
    if (ImGui::Button(button)) {
        if (std::strlen(S.pass) < 8) S.error = "The passphrase needs at least 8 characters.";
        else if (std::strcmp(S.pass, S.pass2) != 0) S.error = "The two passphrases are not the same.";
        else { S.error.clear(); done = true; }
    }
    return done;
}

void lockedBody() {
    ImGui::PushTextWrapPos(0);
    if (S.fileNeedsPass) ImGui::TextUnformatted(tr("Your keys are locked with a passphrase. Type it to unlock them. Until then nothing is decrypted."));
    else ImGui::TextUnformatted(tr("Your keys are locked: the key file could not be opened with this computer's keychain."));
    ImGui::PopTextWrapPos();
    if (S.fileNeedsPass) {
        ImGui::SetNextItemWidth(260 * gUi);
        const bool enter = ImGui::InputText("##unlockpass", S.importPass, sizeof S.importPass, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button(tr("Unlock")) || enter) {
            std::string p = S.importPass;
            wipe(S.importPass, sizeof S.importPass);
            tryLoad(p);
            wipe(p);
        }
    } else if (ImGui::Button(tr("Try again"))) tryLoad(std::string());
}

void editDialog() {
    if (S.openEdit) { ImGui::OpenPopup("Key##voicekeyedit"); S.openEdit = false; }
    centre(520, 520);
    bool open = true;
    if (!ImGui::BeginPopupModal("Key##voicekeyedit", &open)) return;
    using namespace keyswin;
    auto& f = S.form;
    char buf[256];
    auto text = [&](const char* label, std::string& v, float w) {
        snprintf(buf, sizeof buf, "%s", v.c_str());
        ImGui::SetNextItemWidth(w * gUi);
        if (ImGui::InputText(label, buf, sizeof buf)) v = buf;
        wipe(buf, sizeof buf);
    };
    text(tr("Name##k"), f.name, 260);
    auto combo = [&](const char* label, int& idx, int n, auto name) {
        ImGui::SetNextItemWidth(260 * gUi);
        if (ImGui::BeginCombo(label, name(idx))) {
            for (int i = 0; i < n; i++) if (ImGui::Selectable(name(i), i == idx)) idx = i;
            ImGui::EndCombo();
        }
    };
    combo(tr("Protocol##k"), f.protocol, (int)protocolChoices().size(), [](int i) { return i == 0 ? "any" : vk::protocolName(protocolChoices()[i]); });
    combo(tr("Algorithm##k"), f.algorithm, (int)algorithmChoices().size(), [](int i) { return vk::algorithmName(algorithmChoices()[i]); });
    const Algorithm alg = algorithmChoices()[std::clamp(f.algorithm, 0, (int)algorithmChoices().size() - 1)];
    ImGui::TextDisabled("%s", keyswin::lengthRule(alg).c_str());
    if (alg == Algorithm::HyteraBp || alg == Algorithm::HyteraEnhanced)
        ImGui::TextWrapped("%s", tr("Hytera privacy is not an ETSI standard: the method follows the open-source DSD-FME. Enter your own key. It is checked against DSD-FME's algorithm only, not yet against a live Hytera call, and the DMR receiver does not apply it to live audio yet."));
    // the key, hidden unless Reveal is on
    snprintf(buf, sizeof buf, "%s", f.hex.c_str());
    ImGui::SetNextItemWidth(420 * gUi);
    if (ImGui::InputText("##khex", buf, sizeof buf, S.formReveal ? 0 : ImGuiInputTextFlags_Password)) f.hex = buf;
    wipe(buf, sizeof buf);
    ImGui::SameLine(); ImGui::TextUnformatted(tr("Key (hex)"));
    ImGui::Checkbox(tr("Reveal##keyform"), &S.formReveal);
    const std::string counter = keyswin::hexCounter(alg, f.hex);
    if (!counter.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", counter.c_str()); }
    const std::string hp = f.hex.empty() ? std::string() : keyswin::hexProblem(alg, f.hex);
    if (!hp.empty()) ImGui::TextColored(pal::badRed(), "%s", hp.c_str());
    ImGui::SeparatorText(tr("Use this key for calls with (optional)"));
    ImGui::TextDisabled("%s", tr("Leave blank to match any. A call that carries a key id uses only the keys with that id."));
    text(tr("Key id##k"), f.keyId, 120);
    text(tr("Algorithm id (e.g. 0x84)##k"), f.algId, 120);
    text(tr("Talkgroup##k"), f.talkgroup, 120);
    text(tr("Unit / radio id##k"), f.unit, 120);
    if (f.protocol == 0 || protocolChoices()[f.protocol] == Protocol::P25) text(tr("NAC (P25)##k"), f.nac, 120);
    if (f.protocol == 0 || protocolChoices()[f.protocol] == Protocol::Dmr) text(tr("Colour code (DMR)##k"), f.cc, 120);
    ImGui::Checkbox(tr("Enabled##keyform"), &f.enabled);
    if (!S.formError.empty()) ImGui::TextColored(pal::badRed(), "%s", S.formError.c_str());
    ImGui::Separator();
    const bool saveClicked = ImGui::Button(tr("Save key"));
    ImGui::SameLine();
    const bool cancel = ImGui::Button(tr("Cancel"));
    if (saveClicked) {
        VoiceKey k;
        S.formError = keyFromForm(f, k);
        if (S.formError.empty()) {
            if (S.editIndex >= 0) vk::store().update(S.editIndex, k); else vk::store().add(k);
            if (persist()) S.notice = std::string("Saved key \"") + k.name + "\".";
            wipe(S.form.hex);
            S.formReveal = false;
            ImGui::CloseCurrentPopup();
        }
    }
    if (cancel || !open) { wipe(S.form.hex); S.formReveal = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void confirmDialogs() {
    if (S.openDelete) { ImGui::OpenPopup("Delete key##vkdel"); S.openDelete = false; }
    if (ImGui::BeginPopupModal("Delete key##vkdel", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto keys = vk::store().list();
        if (S.deleteIndex >= 0 && S.deleteIndex < (int)keys.size()) {
            ImGui::Text("%s \"%s\"?", tr("Delete the key"), keys[S.deleteIndex].name.c_str());
            ImGui::TextDisabled("%s", tr("This cannot be undone."));
            if (ImGui::Button(tr("Delete"))) {
                vk::store().remove(S.deleteIndex);
                S.revealed.clear();
                persist();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
        }
        if (ImGui::Button((std::string(tr("Cancel")) + "##c275").c_str())) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (S.openClear) { ImGui::OpenPopup("Clear all keys##vkclear"); S.openClear = false; }
    if (ImGui::BeginPopupModal("Clear all keys##vkclear", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", tr("Delete ALL voice keys?"));
        ImGui::TextDisabled("%s", tr("Every key is removed from this computer. Export them first if you want a copy."));
        if (ImGui::Button(tr("Delete all keys"))) {
            vk::store().clear();
            S.revealed.clear();
            persist();
            S.notice = "All keys deleted.";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void startImport() {
    const std::string path = plat::openFileDialog();
    if (path.empty()) return;
    S.importError.clear();
    S.importNeedsPass = false;
    wipe(S.importPass, sizeof S.importPass);
    if (!readFile(path, S.importBytes, 1u << 20)) { S.error = "That file could not be read (or it is over 1 MB)."; return; }
    S.preview = keyswin::previewCsv(S.importBytes, vk::store().list());
    S.importNeedsPass = S.preview.encryptedFile;
    S.openImport = true;
}

void importDialog() {
    if (S.openImport) { ImGui::OpenPopup("Import keys##vkimp"); S.openImport = false; }
    centre(760, 480);
    bool open = true;
    if (!ImGui::BeginPopupModal("Import keys##vkimp", &open)) return;
    auto close = [&] { wipe(S.importBytes); S.preview = {}; wipe(S.importPass, sizeof S.importPass); ImGui::CloseCurrentPopup(); };
    if (S.importNeedsPass) {
        ImGui::TextUnformatted(tr("This is an OnAir export locked with a passphrase. Type it to see what is inside."));
        ImGui::SetNextItemWidth(260 * gUi);
        const bool enter = ImGui::InputText("##imppass", S.importPass, sizeof S.importPass, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button(tr("Open")) || enter) {
            std::vector<VoiceKey> keys;
            const std::string err = vk::decryptStore(S.importBytes, {}, S.importPass, keys);
            wipe(S.importPass, sizeof S.importPass);
            if (!err.empty()) S.importError = err;
            else { S.preview = keyswin::previewKeyList(std::move(keys), vk::store().list()); S.importNeedsPass = false; S.importError.clear(); }
        }
        if (!S.importError.empty()) ImGui::TextColored(pal::badRed(), "%s", S.importError.c_str());
        if (ImGui::Button((std::string(tr("Cancel")) + "##c326").c_str())) close();
        ImGui::EndPopup();
        return;
    }
    const auto toAdd = keyswin::rowsToAdd(S.preview);
    ImGui::Text("%d %s, %d %s, %d %s", (int)toAdd.size(), tr("to import"), (int)(S.preview.rows.size() - toAdd.size()), tr("already there"), (int)S.preview.errors.size(), tr("problems"));
    ImGui::TextDisabled("%s", tr("Keys are shown masked. Nothing is imported until you press Import."));
    if (ImGui::BeginChild("##imppreview", ImVec2(0, -70 * gUi), true)) {
        for (const auto& r : S.preview.rows) {
            const std::string line = keyswin::previewLine(r);
            if (r.duplicate) ImGui::TextDisabled("%s", line.c_str()); else ImGui::TextUnformatted(line.c_str());
        }
        for (const auto& e : S.preview.errors) ImGui::TextColored(pal::badRed(), "%s", e.c_str());
        if (S.preview.rows.empty() && S.preview.errors.empty()) ImGui::TextDisabled("%s", tr("No keys found in that file. Columns: name,protocol,algorithm,key,keyid,algid,talkgroup,unit,nac,cc,enabled"));
    }
    ImGui::EndChild();
    ImGui::BeginDisabled(toAdd.empty() || !canSave());
    if (ImGui::Button(tr("Import"))) {
        for (const auto& k : toAdd) vk::store().add(k);
        if (persist()) S.notice = "Imported " + std::to_string(toAdd.size()) + " key(s).";
        close();
    }
    ImGui::EndDisabled();
    if (!canSave()) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("Set a passphrase first.")); }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel")) || !open) close();
    ImGui::EndPopup();
}

void exportDialog() {
    if (S.openExport) { ImGui::OpenPopup("Export keys##vkexp"); S.openExport = false; S.exportError.clear(); wipe(S.pass, sizeof S.pass); wipe(S.pass2, sizeof S.pass2); }
    centre(560, 360);
    bool open = true;
    if (!ImGui::BeginPopupModal("Export keys##vkexp", &open)) return;
    ImGui::PushStyleColor(ImGuiCol_Text, pal::warnAmber());
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(tr(keyswin::exportWarning(S.exportEncrypted)));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Spacing();
    if (ImGui::RadioButton(tr("Locked with a passphrase (.oavk)"), S.exportEncrypted)) S.exportEncrypted = true;
    if (ImGui::RadioButton(tr("Plain text CSV (not protected)"), !S.exportEncrypted)) S.exportEncrypted = false;
    bool go = false;
    if (S.exportEncrypted) {
        ImGui::SetNextItemWidth(240 * gUi); ImGui::InputText("##ep1", S.pass, sizeof S.pass, ImGuiInputTextFlags_Password);
        ImGui::SameLine(); ImGui::TextUnformatted(tr("Passphrase (8 or more characters)"));
        ImGui::SetNextItemWidth(240 * gUi); ImGui::InputText("##ep2", S.pass2, sizeof S.pass2, ImGuiInputTextFlags_Password);
        ImGui::SameLine(); ImGui::TextUnformatted(tr("Again"));
    }
    if (!S.exportError.empty()) ImGui::TextColored(pal::badRed(), "%s", S.exportError.c_str());
    if (ImGui::Button(tr("Save as..."))) {
        S.exportError.clear();
        if (S.exportEncrypted && std::strlen(S.pass) < 8) S.exportError = "The passphrase needs at least 8 characters.";
        else if (S.exportEncrypted && std::strcmp(S.pass, S.pass2) != 0) S.exportError = "The two passphrases are not the same.";
        else go = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel")) || !open) { wipe(S.pass, sizeof S.pass); wipe(S.pass2, sizeof S.pass2); ImGui::CloseCurrentPopup(); }
    if (go) {
        const std::string path = plat::saveFileDialog(keyswin::exportFileName(S.exportEncrypted).c_str());
        if (!path.empty()) {
            std::string bytes;
            std::string err;
            if (S.exportEncrypted) err = vk::encryptStore(vk::store().list(), {}, S.pass, bytes);
            else bytes = vk::store().exportCsv();
            if (err.empty()) {
                std::ofstream o(dect2::u8file(path), std::ios::binary | std::ios::trunc);
                o.write(bytes.data(), (std::streamsize)bytes.size());
                if (!o) err = "The file could not be written.";
            }
            wipe(bytes);
            if (err.empty()) { S.notice = "Exported " + std::to_string(vk::store().count()) + " key(s). The file holds your keys: keep it private."; wipe(S.pass, sizeof S.pass); wipe(S.pass2, sizeof S.pass2); ImGui::CloseCurrentPopup(); }
            else S.exportError = err;
        }
    }
    ImGui::EndPopup();
}

void keyTable() {
    const auto keys = vk::store().list();
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##voicekeys", 8, fl, ImVec2(0, -4 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("On"), ImGuiTableColumnFlags_WidthFixed, 36 * gUi);
        ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn(tr("Protocol"), ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn(tr("Algorithm"), ImGuiTableColumnFlags_WidthFixed, 100 * gUi);
        ImGui::TableSetupColumn(tr("Key id"), ImGuiTableColumnFlags_WidthFixed, 56 * gUi);
        ImGui::TableSetupColumn(tr("Talkgroup / ID / NAC / CC"), ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn(tr("Key"), ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)keys.size(); i++) {
            const VoiceKey& k = keys[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool en = k.enabled;
            if (ImGui::Checkbox("##en", &en)) { VoiceKey u = k; u.enabled = en; vk::store().update(i, u); persist(); }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(k.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(k.protocol == Protocol::Any ? "any" : vk::protocolName(k.protocol));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(vk::algorithmName(k.algorithm));
            ImGui::TableNextColumn(); if (k.keyId >= 0) ImGui::Text("%d", k.keyId); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            VoiceKey a = k; a.keyId = -1;
            ImGui::TextUnformatted(keyswin::associationText(a).c_str());
            ImGui::TableNextColumn();
            const bool shown = S.revealed.count(i) > 0;
            if (shown) ImGui::TextWrapped("%s", k.keyHex().c_str());
            else ImGui::TextDisabled("%s", keyswin::maskKey(k.key.size()).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(shown ? tr("Hide") : tr("Reveal"))) { if (shown) S.revealed.erase(i); else S.revealed.insert(i); }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(tr("Edit"))) {
                S.form = keyswin::formFromKey(k); S.editIndex = i; S.formError.clear(); S.formReveal = false; S.openEdit = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Delete"))) { S.deleteIndex = i; S.openDelete = true; }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

}  // namespace

void voiceKeysTick(App&) { init(); }

void voiceKeysWindow(App& a) {
    init();
    if (!a.voiceKeysOpen) { S.revealed.clear(); return; }
    centre(960, 560);
    if (!ImGui::Begin(trw("Voice keys"), &a.voiceKeysOpen)) { ImGui::End(); return; }
    const keyswin::StoreState st = state();
    // status line
    ImGui::TextColored(st == keyswin::StoreState::Unlocked ? pal::okGreen() : pal::warnAmber(), "%s", tr(keyswin::storeStateText(st)));
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", S.keychain.empty() ? tr("protected by a passphrase") : (std::string(tr("protected by the ")) + S.keychain).c_str());
    if (!S.error.empty()) ImGui::TextColored(pal::badRed(), "%s", S.error.c_str());
    if (!S.error.empty() && S.haveFile && !S.fileNeedsPass && S.error.find("keychain") != std::string::npos) {
        // the keychain item is gone (keychain reset): the old file can never open again; the user may move it aside to start a new store
        ImGui::TextWrapped("%s", tr("If you reset or wiped the keychain, the old key file cannot be opened any more. You can move it aside (it is kept as keys.oavk.discarded) and start a new store."));
        if (ImGui::Button(tr("Move the old key file aside"))) {
            const std::string e = vk::discardKeyFile(filePath());
            if (e.empty()) { S.haveFile = false; S.error.clear(); S.unlocked = true; S.notice = "The old key file was moved aside. You can add keys again."; }
            else S.error = e;
        }
    }
    if (S.error.empty() && !S.notice.empty()) ImGui::TextColored(pal::okGreen(), "%s", S.notice.c_str());
    ImGui::Separator();

    if (st == keyswin::StoreState::NeedsNotice) { legalBody(a); ImGui::End(); return; }
    if (st == keyswin::StoreState::Locked) { lockedBody(); ImGui::End(); return; }

    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("OnAir decrypts a call only with a key you add here. A call that names a key id uses only keys with that id; a call that does not is tried against your enabled keys for its protocol, in this order. OnAir never searches for a key."));
    ImGui::PopTextWrapPos();
    if (!vk::haveSecretProvider() && S.passphrase.empty()) {
        ImGui::SeparatorText(tr("Choose a passphrase"));
        ImGui::TextWrapped("%s", tr("This system has no keychain OnAir can use, so your keys are locked with a passphrase you type each time OnAir starts. If you forget it the keys cannot be recovered."));
        if (passphraseSetter(tr("Set passphrase"))) { S.passphrase = S.pass; wipe(S.pass, sizeof S.pass); wipe(S.pass2, sizeof S.pass2); persist(); }
        ImGui::Separator();
    }
    ImGui::BeginDisabled(!canSave());
    if (ImGui::Button(tr("Add key..."))) { S.form = keyswin::KeyForm(); S.editIndex = -1; S.formError.clear(); S.formReveal = false; S.openEdit = true; }
    ImGui::SameLine();
    if (ImGui::Button(tr("Import..."))) startImport();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(vk::store().count() == 0);
    if (ImGui::Button(tr("Export..."))) S.openExport = true;
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear all..."))) S.openClear = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Lock now"))) lockNow();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Remove the keys from memory. Nothing is decrypted until you unlock them again."));
    keyTable();
    editDialog();
    confirmDialogs();
    importDialog();
    exportDialog();
    ImGui::End();
}

bool voiceKeysStatus(App& a, const std::string& core) {
    const CallStatus c = keyswin::classifyStatus(core);
    if (c == CallStatus::Clear) return false;
    if (c == CallStatus::Decrypted) { ImGui::TextColored(pal::okGreen(), "%s", core.c_str()); return true; }
    if (c == CallStatus::Unsupported || c == CallStatus::Other) { ImGui::TextColored(pal::warnAmber(), "%s", core.c_str()); return true; }
    ImGui::TextColored(c == CallStatus::Mismatch ? pal::badRed() : pal::warnAmber(), "%s", core.c_str());
    ImGui::SameLine();
    ImGui::PushID("vkstat");
    if (ImGui::SmallButton(c == CallStatus::Mismatch ? tr("Check keys...") : tr("Add key..."))) a.voiceKeysOpen = true;
    ImGui::PopID();
    return true;
}

void voiceKeysLink(App& a) {
    ImGui::PushID("vklink");
    if (ImGui::SmallButton(tr("Voice keys..."))) a.voiceKeysOpen = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Encrypted calls are decrypted only with a key you add yourself."));
    ImGui::PopID();
}
