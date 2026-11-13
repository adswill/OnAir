// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Updates in the window: a check when the program starts (every start, one retry on failure), the download in the background, a button in the top bar that tells
// the state, and a small window with the notes of the new version and the settings. The new version is put in place when the program closes,
// or at once on request.
#include "../app.h"
#include "dect2/engine/updater.h"
#include "../state/receivers.h"
#include "supporters_data.h"
#include <ctime>

namespace {
Updater& U(App& a) {
    if (!a.upd) { a.upd.reset(new Updater(ONAIR_VERSION)); a.upd->loadResult(); }   // loadResult: what the installation of an earlier start left behind
    return *a.upd;
}
bool resultDismissed = false;
// the version whose installation failed at the last start: not tried again by itself (the button still does)
bool failedBefore(const Updater::Status& st, const ReleaseInfo& r) { return st.lastResult.found && !st.lastResult.ok && compareVersions(r.version, st.lastResult.version) == 0; }
double nowS() { return (double)time(nullptr); }
// the mirror choices of the settings go to the updater (nothing is sent to a mirror unless they allow it)
void applyMirror(App& a) {
    MirrorConfig c;
    c.source = a.updSrc == 1 ? UpdateSource::GitHubOnly : a.updSrc == 2 ? UpdateSource::MirrorOnly : UpdateSource::Automatic;
    c.fallback = a.updMirrorOn;
    c.tmpl = a.updMirror;
    U(a).setMirror(c);
}
bool skipped(const App& a, const ReleaseInfo& r) { return !a.updSkip.empty() && compareVersions(r.version, a.updSkip) <= 0; }
}

void updateTick(App& a) {
    Updater& u = U(a);
    applyMirror(a);
    const Updater::Status st = u.status();
    static Updater::Phase lastPhase = Updater::Phase::Idle;
    static double startT = -1;
    static bool resultLogged = false;
    if (!resultLogged) {
        resultLogged = true;
        if (st.lastResult.found) {
            if (st.lastResult.ok) a.engine.log("updated to v" + st.lastResult.version);
            else a.engine.log("the update to v" + st.lastResult.version + " did not install (setup exit code " + std::to_string(st.lastResult.setupExit) + ")");
        }
    }
    if (startT < 0) startT = ImGui::GetTime();
    // a check a few seconds after every start (in the background), and one retry if it fails, e.g. the network was not up yet
    static int startAttempts = 0;
    static double failT = -1;
    if (startAttempts > 0 && startAttempts < 2 && st.phase == Updater::Phase::Failed && failT < 0) failT = ImGui::GetTime();
    if (st.phase == Updater::Phase::Idle || st.phase == Updater::Phase::Failed) {
        const int act = startupCheckAction(a.updCheck && !getenv("ONAIR_NO_UPDATE_CHECK"), ImGui::GetTime() - startT, startAttempts,
                                           failT >= 0 && st.phase == Updater::Phase::Failed, failT >= 0 ? ImGui::GetTime() - failT : 0);
        if (act) { startAttempts++; a.updStarted = true; u.check(a.updPre); }
    }
    if (st.phase != lastPhase) {
        lastPhase = st.phase;
        switch (st.phase) {
        case Updater::Phase::UpToDate: a.updLast = nowS(); savePrefs(a); a.engine.log(std::string("OnAir ") + ONAIR_VERSION + " is the newest version"); break;
        case Updater::Phase::Available:
            a.updLast = nowS(); savePrefs(a);
            a.engine.log("update available: OnAir " + st.release.version + (st.release.prerelease ? tr(" (beta)") : ""));
            if (a.updAuto && st.autoInstallable && !st.mirrorUnverified && !skipped(a, st.release) && !failedBefore(st, st.release)) { U(a).download(); a.engine.log("downloading OnAir " + st.release.version + " in the background"); }
            break;
        case Updater::Phase::Ready:
            if (st.manualInstall) a.engine.log("OnAir " + st.release.version + " is downloaded and checked. Install it with: " + st.manualCommand);
            else a.engine.log("OnAir " + st.release.version + " is ready: it is installed when you close the program");
            break;
        case Updater::Phase::Failed: a.engine.log("update: " + st.error); break;
        default: break;
        }
    }
}

// Called when the program is closing: puts the downloaded version in place. Returns true if a helper was started.
bool updateOnExit(App& a) {
    if (!a.upd) return false;
    const Updater::Status st = a.upd->status();
    if (st.phase != Updater::Phase::Ready || !a.updAuto) return false;
    if (st.release.fromMirror && !st.release.officialVerified) return false;   // only the mirror vouches for it: installed only by the button, never by itself
    std::string err;
    return a.upd->apply(false, &err);
}

void supportButton() {
    if (ImGui::SmallButton(tr("Support"))) dect2::openUrl("https://ko-fi.com/adswill");
    if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted(tr("Support OnAir on Ko-fi"));
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("Thank you to our supporters:"));
        for (const char* n : kSupporters) ImGui::TextUnformatted(n);
        ImGui::EndTooltip();
    }
}

void updateButton(App& a) {
    Updater& u = U(a);
    const Updater::Status st = u.status();
    char label[64];
    ImVec4 col(0.62f, 0.68f, 0.75f, 1);
    switch (st.phase) {
    case Updater::Phase::Checking: snprintf(label, sizeof label, "%s", tr("checking…")); break;
    case Updater::Phase::Available: snprintf(label, sizeof label, tr("update %s"), st.release.version.c_str()); col = ImVec4(0.95f, 0.75f, 0.2f, 1); if (skipped(a, st.release)) { snprintf(label, sizeof label, "v%s", ONAIR_VERSION); col = ImVec4(0.62f, 0.68f, 0.75f, 1); } break;
    case Updater::Phase::Downloading: snprintf(label, sizeof label, tr("downloading %.0f%%"), st.total ? 100.0 * (double)st.done / (double)st.total : 0.0); col = ImVec4(0.4f, 0.75f, 0.95f, 1); break;
    case Updater::Phase::Preparing: snprintf(label, sizeof label, "%s", tr("preparing…")); col = ImVec4(0.4f, 0.75f, 0.95f, 1); break;
    case Updater::Phase::Ready: snprintf(label, sizeof label, "%s", tr("restart to update")); col = ImVec4(0.35f, 0.9f, 0.45f, 1); break;
    case Updater::Phase::Failed: snprintf(label, sizeof label, "v%s", ONAIR_VERSION); col = ImVec4(0.95f, 0.5f, 0.35f, 1); break;
    default: snprintf(label, sizeof label, "v%s", ONAIR_VERSION); break;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    if (ImGui::SmallButton(label)) { ImGui::OpenPopup("##updates"); }
    ImGui::PopStyleColor();
    const ImVec2 bmax = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Updates"));
    { static const bool dbg = getenv("ONAIR_OPEN_UPDATES") != nullptr; if (dbg && st.phase != Updater::Phase::Idle && st.phase != Updater::Phase::Checking && !ImGui::IsPopupOpen("##updates")) ImGui::OpenPopup("##updates"); }   // for screenshots
    ImGui::SetNextWindowPos(ImVec2(bmax.x + 70 * gUi, bmax.y + 4), ImGuiCond_Always, ImVec2(1, 0));   // under the button, its right edge near the window's
    ImGui::SetNextWindowSizeConstraints(ImVec2(400 * gUi, 0), ImVec2(520 * gUi, 520 * gUi));
    if (ImGui::BeginPopup("##updates")) {
        ImGui::Text(tr("OnAir %s"), ONAIR_VERSION);
        ImGui::SameLine(0, 10 * gUi);
        ImGui::TextDisabled("(%s)", installKindName(st.kind));
        ImGui::Separator();
        if (st.lastResult.found && !st.lastResult.ok && !resultDismissed) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1), tr("The update to v%s did not install (setup exit code %d)."), st.lastResult.version.c_str(), st.lastResult.setupExit);
            ImGui::PopTextWrapPos();
            if (ImGui::Button(tr("Download page"))) u.openReleasePage();
            ImGui::SameLine();
            if (ImGui::Button(tr("Dismiss"))) resultDismissed = true;
            ImGui::Separator();
        }
        switch (st.phase) {
        case Updater::Phase::Checking: ImGui::TextUnformatted(tr("Asking GitHub for the newest version…")); break;
        case Updater::Phase::UpToDate: ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1), "%s", tr("You have the newest version.")); break;
        case Updater::Phase::Failed:
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1), "%s", st.error.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TextDisabled("%s", tr("Nothing was changed. You can try again, or take the package from the release page."));
            if (ImGui::SmallButton(tr("Release page##updfail"))) u.openReleasePage();
            break;
        case Updater::Phase::Available:
        case Updater::Phase::Downloading:
        case Updater::Phase::Preparing:
        case Updater::Phase::Ready: {
            ImGui::Text(tr("Version %s is available%s."), st.release.version.c_str(), st.release.prerelease ? tr(" (beta)") : "");
            if (!st.release.notes.empty()) {
                ImGui::BeginChild("##notes", ImVec2(0, 150 * gUi), true);
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(st.release.notes.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndChild();
            }
            if (st.phase == Updater::Phase::Downloading) {
                ImGui::ProgressBar(st.total ? (float)((double)st.done / (double)st.total) : 0.f, ImVec2(-1, 0),
                                   st.total ? (std::to_string(st.done >> 20) + " of " + std::to_string(st.total >> 20) + " MB").c_str() : nullptr);
            }
            else if (st.phase == Updater::Phase::Preparing) ImGui::TextUnformatted(tr("Checking and preparing the new version…"));
            else if (st.phase == Updater::Phase::Ready && st.manualInstall) {   // a .deb: checked and waiting, the user runs the command
                ImGui::PushTextWrapPos(0);
                ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1), "%s", tr("Downloaded and checked against its SHA-256. OnAir does not install packages by itself: run this in a terminal."));
                ImGui::PopTextWrapPos();
                char cmd[1100];
                snprintf(cmd, sizeof cmd, "%s", st.manualCommand.c_str());
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##debcmd", cmd, sizeof cmd, ImGuiInputTextFlags_ReadOnly);
                if (ImGui::Button(tr("Copy command"))) ImGui::SetClipboardText(st.manualCommand.c_str());
                ImGui::SameLine();
                if (ImGui::Button(tr("Release page##manual"))) u.openReleasePage();
                ImGui::TextDisabled("%s", tr("Then start OnAir again."));
            }
            else if (st.phase == Updater::Phase::Ready) {
                ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1), "%s", tr("Downloaded and checked. It is installed when you close OnAir."));
                if (ImGui::Button(tr("Restart now and update"))) {
                    std::string err;
                    if (u.apply(true, &err)) glfwSetWindowShouldClose(gWindow, 1); else a.engine.log(err.compare(0, 6, "Update") == 0 ? err : "update: " + err);
                }
                if (!st.applyError.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1), "%s", st.applyError.c_str()); ImGui::PopTextWrapPos(); }
                if (st.kind == InstallKind::WindowsInstall) ImGui::TextDisabled("%s", tr("Windows asks for permission to run the installer."));
            } else {
                if (st.release.fromMirror) {
                    ImGui::PushTextWrapPos(0);
                    if (st.mirrorUnverified) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1), "%s", tr("The list of versions came through the mirror and GitHub could not be reached to confirm the checksum: only the mirror vouches for this package. Nothing is installed without your confirmation."));
                    else ImGui::TextDisabled("%s", tr("The list came through the mirror; the checksum was confirmed by GitHub."));
                    ImGui::PopTextWrapPos();
                }
                if (st.mirrorUnverified && (st.autoInstallable || st.manualInstall)) ImGui::Checkbox(tr("I understand, download it anyway"), &a.updConfirm);
                if (st.autoInstallable || st.manualInstall) {
                    ImGui::BeginDisabled(st.mirrorUnverified && !a.updConfirm);
                    if (ImGui::Button(st.manualInstall ? tr("Download and check") : tr("Download and install"))) u.download(st.mirrorUnverified && a.updConfirm);
                    ImGui::EndDisabled();
                }
                else { ImGui::TextDisabled("%s", tr("This copy of OnAir cannot replace itself.")); }
                ImGui::SameLine();
                if (ImGui::Button(tr("Release page##avail"))) u.openReleasePage();
                ImGui::SameLine();
                if (ImGui::Button(tr("Skip this version"))) { a.updSkip = st.release.version; savePrefs(a); }
            }
            break;
        }
        default: ImGui::TextUnformatted(tr("Not checked yet.")); break;
        }
        const bool busy = st.phase == Updater::Phase::Checking || st.phase == Updater::Phase::Downloading || st.phase == Updater::Phase::Preparing;
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(tr("Check now"))) { a.updSkip.clear(); u.check(a.updPre); }
        ImGui::EndDisabled();
        ImGui::Separator();
        bool ch = false;
        ch |= ImGui::Checkbox(tr("Check for updates automatically"), &a.updCheck);
        ch |= ImGui::Checkbox(tr("Download and install them by themselves"), &a.updAuto);
        ch |= ImGui::Checkbox(tr("Include beta versions"), &a.updPre);
        if (ch) savePrefs(a);
        {
            const char* items[3] = {tr("Automatic: GitHub"), tr("GitHub only"), tr("Mirror only")};
            ImGui::SetNextItemWidth(200 * gUi);
            if (ImGui::BeginCombo(tr("Update source"), items[std::max(0, std::min(2, a.updSrc))])) {
                for (int i = 0; i < 3; i++) if (ImGui::Selectable(items[i], a.updSrc == i)) { a.updSrc = i; savePrefs(a); }
                ImGui::EndCombo();
            }
            if (a.updSrc == 0 && ImGui::Checkbox(tr("Use the mirror when GitHub cannot be reached"), &a.updMirrorOn)) savePrefs(a);
            if (a.updSrc == 2 || (a.updSrc == 0 && a.updMirrorOn)) {
                char buf[400];
                snprintf(buf, sizeof buf, "%s", a.updMirror.c_str());
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputTextWithHint("##updmirror", tr("Mirror address (https://host/{url} or a base address)"), buf, sizeof buf)) { a.updMirror = buf; savePrefs(a); }
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (a.updSrc == 2 || (a.updSrc == 0 && a.updMirrorOn)) ImGui::TextWrapped("%s", tr("A mirror is a third-party web address you chose. It sees that you check for updates and which version you run. It is used only after GitHub fails (not at all with GitHub only), the package is checked against the checksum from GitHub when GitHub can be reached, and nothing is installed from a mirror without your confirmation."));
        else ImGui::TextWrapped("%s", tr("Only github.com is contacted. Downloads are checked against the SHA-256 GitHub lists for the file."));
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }
}

// "Clear all" next to the version: empties the decoded lists of every mode, on every open receiver, after a question
void clearAllButtonAt(App& a, bool sameLine);
void clearAllButton(App& a) { clearAllButtonAt(a, true); }
void clearAllButtonAt(App& a, bool sameLine) {
    if (sameLine) ImGui::SameLine(0, 6 * gUi);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.6f, 0.45f, 1));
    if (ImGui::SmallButton(tr("Clear all##clearall"))) ImGui::OpenPopup("##clearallask");
    ImGui::PopStyleColor();
    const ImVec2 bmax = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Clears all previous data: the decoded lists of every mode (aircraft, ships, messages, pictures and so on), on every open receiver. The saved copy on disk is emptied too."));
    ImGui::SetNextWindowPos(ImVec2(bmax.x, bmax.y + 4), ImGuiCond_Appearing, ImVec2(1, 0));
    if (ImGui::BeginPopup("##clearallask")) {
        ImGui::PushTextWrapPos(340 * gUi);
        ImGui::TextUnformatted(tr("Are you sure? This clears all previous data and cannot be undone."));
        ImGui::PopTextWrapPos();
        if (ImGui::Button(tr("Yes, clear all"))) {
            a.engine.clearAllResults();
            marineFaxClearUi();
            for (App* r : receiversAll()) if (r != &a) r->engine.clearAllResults();
            a.engine.log("results: all previous data cleared");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
