// Updates in the window: a check when the program starts (and once a day), the download in the background, a button in the top bar that tells
// the state, and a small window with the notes of the new version and the settings. The new version is put in place when the program closes,
// or at once on request.
#include "app.h"
#include "dect2/updater.h"
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
bool skipped(const App& a, const ReleaseInfo& r) { return !a.updSkip.empty() && compareVersions(r.version, a.updSkip) <= 0; }
}

void updateTick(App& a) {
    Updater& u = U(a);
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
    // a check a few seconds after the start, then once a day
    if (a.updCheck && st.phase == Updater::Phase::Idle && !a.updStarted && ImGui::GetTime() - startT > 6 && !getenv("ONAIR_NO_UPDATE_CHECK")) {
        a.updStarted = true;
        if (nowS() - a.updLast > 20 * 3600 || getenv("ONAIR_FORCE_UPDATE_CHECK")) u.check(a.updPre);
    }
    if (st.phase != lastPhase) {
        lastPhase = st.phase;
        switch (st.phase) {
        case Updater::Phase::UpToDate: a.updLast = nowS(); savePrefs(a); a.engine.log(std::string("OnAir ") + ONAIR_VERSION + " is the newest version"); break;
        case Updater::Phase::Available:
            a.updLast = nowS(); savePrefs(a);
            a.engine.log("update available: OnAir " + st.release.version + (st.release.prerelease ? " (pre-release)" : ""));
            if (a.updAuto && st.autoInstallable && !skipped(a, st.release) && !failedBefore(st, st.release)) { U(a).download(); a.engine.log("downloading OnAir " + st.release.version + " in the background"); }
            break;
        case Updater::Phase::Ready: a.engine.log("OnAir " + st.release.version + " is ready: it is installed when you close the program"); break;
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
    std::string err;
    return a.upd->apply(false, &err);
}

void updateButton(App& a) {
    Updater& u = U(a);
    const Updater::Status st = u.status();
    char label[64];
    ImVec4 col(0.62f, 0.68f, 0.75f, 1);
    switch (st.phase) {
    case Updater::Phase::Checking: snprintf(label, sizeof label, "checking…"); break;
    case Updater::Phase::Available: snprintf(label, sizeof label, "update %s", st.release.version.c_str()); col = ImVec4(0.95f, 0.75f, 0.2f, 1); if (skipped(a, st.release)) { snprintf(label, sizeof label, "v%s", ONAIR_VERSION); col = ImVec4(0.62f, 0.68f, 0.75f, 1); } break;
    case Updater::Phase::Downloading: snprintf(label, sizeof label, "downloading %.0f%%", st.total ? 100.0 * (double)st.done / (double)st.total : 0.0); col = ImVec4(0.4f, 0.75f, 0.95f, 1); break;
    case Updater::Phase::Preparing: snprintf(label, sizeof label, "preparing…"); col = ImVec4(0.4f, 0.75f, 0.95f, 1); break;
    case Updater::Phase::Ready: snprintf(label, sizeof label, "restart to update"); col = ImVec4(0.35f, 0.9f, 0.45f, 1); break;
    case Updater::Phase::Failed: snprintf(label, sizeof label, "v%s", ONAIR_VERSION); col = ImVec4(0.95f, 0.5f, 0.35f, 1); break;
    default: snprintf(label, sizeof label, "v%s", ONAIR_VERSION); break;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    if (ImGui::SmallButton(label)) { ImGui::OpenPopup("##updates"); }
    ImGui::PopStyleColor();
    const ImVec2 bmax = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Updates");
    { static const bool dbg = getenv("ONAIR_OPEN_UPDATES") != nullptr; if (dbg && st.phase != Updater::Phase::Idle && st.phase != Updater::Phase::Checking && !ImGui::IsPopupOpen("##updates")) ImGui::OpenPopup("##updates"); }   // for screenshots
    ImGui::SetNextWindowPos(ImVec2(bmax.x + 70 * gUi, bmax.y + 4), ImGuiCond_Always, ImVec2(1, 0));   // under the button, its right edge near the window's
    ImGui::SetNextWindowSizeConstraints(ImVec2(400 * gUi, 0), ImVec2(520 * gUi, 520 * gUi));
    if (ImGui::BeginPopup("##updates")) {
        ImGui::Text("OnAir %s", ONAIR_VERSION);
        ImGui::SameLine(0, 10 * gUi);
        ImGui::TextDisabled("(%s)", installKindName(st.kind));
        ImGui::Separator();
        if (st.lastResult.found && !st.lastResult.ok && !resultDismissed) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1), "The update to v%s did not install (setup exit code %d).", st.lastResult.version.c_str(), st.lastResult.setupExit);
            ImGui::PopTextWrapPos();
            if (ImGui::Button("Download page")) u.openReleasePage();
            ImGui::SameLine();
            if (ImGui::Button("Dismiss")) resultDismissed = true;
            ImGui::Separator();
        }
        switch (st.phase) {
        case Updater::Phase::Checking: ImGui::TextUnformatted("Asking GitHub for the newest version…"); break;
        case Updater::Phase::UpToDate: ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1), "You have the newest version."); break;
        case Updater::Phase::Failed: ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1), "%s", st.error.c_str()); break;
        case Updater::Phase::Available:
        case Updater::Phase::Downloading:
        case Updater::Phase::Preparing:
        case Updater::Phase::Ready: {
            ImGui::Text("Version %s is available%s.", st.release.version.c_str(), st.release.prerelease ? " (pre-release)" : "");
            if (!st.release.notes.empty()) {
                ImGui::BeginChild("##notes", ImVec2(0, 150 * gUi), true);
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(st.release.notes.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndChild();
            }
            if (st.phase == Updater::Phase::Downloading) ImGui::ProgressBar(st.total ? (float)((double)st.done / (double)st.total) : 0.f, ImVec2(-1, 0));
            else if (st.phase == Updater::Phase::Preparing) ImGui::TextUnformatted("Checking and preparing the new version…");
            else if (st.phase == Updater::Phase::Ready) {
                ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1), "Downloaded and checked. It is installed when you close OnAir.");
                if (ImGui::Button("Restart now and update")) {
                    std::string err;
                    if (u.apply(true, &err)) glfwSetWindowShouldClose(gWindow, 1); else a.engine.log(err.compare(0, 6, "Update") == 0 ? err : "update: " + err);
                }
                if (!st.applyError.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1), "%s", st.applyError.c_str()); ImGui::PopTextWrapPos(); }
                if (st.kind == InstallKind::WindowsInstall) ImGui::TextDisabled("Windows asks for permission to run the installer.");
                if (st.kind == InstallKind::LinuxDeb) ImGui::TextDisabled("The system asks for your password to install the package.");
            } else {
                if (st.autoInstallable) { if (ImGui::Button("Download and install")) u.download(); }
                else { ImGui::TextDisabled("This copy of OnAir cannot replace itself."); }
                ImGui::SameLine();
                if (ImGui::Button("Release page")) u.openReleasePage();
                ImGui::SameLine();
                if (ImGui::Button("Skip this version")) { a.updSkip = st.release.version; savePrefs(a); }
            }
            break;
        }
        default: ImGui::TextUnformatted("Not checked yet."); break;
        }
        const bool busy = st.phase == Updater::Phase::Checking || st.phase == Updater::Phase::Downloading || st.phase == Updater::Phase::Preparing;
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Check now")) { a.updSkip.clear(); u.check(a.updPre); }
        ImGui::EndDisabled();
        ImGui::Separator();
        bool ch = false;
        ch |= ImGui::Checkbox("Check for updates automatically", &a.updCheck);
        ch |= ImGui::Checkbox("Download and install them by themselves", &a.updAuto);
        ch |= ImGui::Checkbox("Include pre-releases", &a.updPre);
        if (ch) savePrefs(a);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Only github.com is contacted. Downloads are checked against the SHA-256 GitHub lists for the file.");
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }
}
