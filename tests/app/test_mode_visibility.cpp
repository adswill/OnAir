// Mode visibility rules: parse/join, hidden groups and modes, active mode, favourites, search, tree ticks, presets.
#include "../../app/ui/mode_visibility.h"
#include <cstdio>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
using namespace modevis;
int main() {
    check(parseList("a|b||c").size() == 3 && parseList("").empty(), "parse");
    check(joinList(parseList("b|a")) == "a|b", "join sorted");
    Rules r;
    check(!hidden(r, "G", "M", false), "default shows all");
    r.hiddenModes.insert("M");
    check(hidden(r, "G", "M", false) && !hidden(r, "G", "N", false), "single mode");
    check(!hidden(r, "G", "M", true), "active never hidden");
    check(listed(r, "G", "M", false, true) && !listed(r, "G", "M", false, false), "search lists hidden");
    Rules f;
    f.favsOnly = true; f.favs.insert("A");
    check(!hidden(f, "G", "A", false) && hidden(f, "G", "B", false) && !hidden(f, "G", "B", true), "favourites only");
    f.favsOnly = false;
    check(!hidden(f, "G", "B", false), "favourites off");

    const std::vector<std::string> mods = {"x", "y", "z"};
    Rules t;
    setGroupShown(t, "G", mods, false);
    check(groupState(t, "G", mods) == GroupState::Hidden && hidden(t, "G", "x", false), "group hidden");
    setModeShown(t, "G", "y", mods, true);
    check(!hidden(t, "G", "y", false) && hidden(t, "G", "x", false) && hidden(t, "G", "z", false), "tick one in hidden group");
    check(groupState(t, "G", mods) == GroupState::Mixed, "mixed");
    setGroupShown(t, "G", mods, true);
    check(groupState(t, "G", mods) == GroupState::Shown && isDefault(t), "group back");
    setModeShown(t, "G", "x", mods, false);
    check(groupState(t, "G", mods) == GroupState::Mixed && hidden(t, "G", "x", false), "untick one");
    hideAll(t, {"G", "H"});
    check(hidden(t, "H", "q", false) && t.hiddenModes.empty(), "hide all");
    showAll(t);
    check(isDefault(t), "show all");
    t.favs.insert("k"); t.favsOnly = true;
    reset(t);
    check(isDefault(t) && t.favs.empty(), "reset");

    const std::vector<std::string> gr = {"TV", "BROADCAST RADIO", "AVIATION", "MARITIME", "SATELLITE & NAVIGATION", "AMATEUR - DIGITAL", "AMATEUR - VOICE & PACKET", "GENERAL & TOOLS", "PLUGINS", "NEW THING"};
    auto h = presetHiddenGroups("Ham", gr);
    check(!h.count("AMATEUR - DIGITAL") && !h.count("AMATEUR - VOICE & PACKET") && !h.count("GENERAL & TOOLS") && !h.count("PLUGINS") && h.count("TV") && h.count("AVIATION") && h.count("NEW THING"), "ham");
    h = presetHiddenGroups("Aviation", gr);
    check(!h.count("AVIATION") && h.count("MARITIME") && !h.count("PLUGINS"), "aviation");
    h = presetHiddenGroups("Marine", gr);
    check(!h.count("MARITIME") && !h.count("SATELLITE & NAVIGATION") && h.count("TV"), "marine");
    h = presetHiddenGroups("Broadcast", gr);
    check(!h.count("TV") && !h.count("BROADCAST RADIO") && h.count("MARITIME"), "broadcast");
    check(presetHiddenGroups("Everything", gr).empty() && presetHiddenGroups("nope", gr).empty(), "everything");
    Rules p; p.favs.insert("F"); p.favsOnly = true; p.hiddenModes.insert("m");
    applyPreset(p, "Marine", gr);
    check(p.hiddenModes.empty() && p.favs.count("F") && p.favsOnly, "preset keeps favourites");
    check(containsNoCase("Aviation", "avi") && containsNoCase("x", ""), "search");
    {   // the saved copy: text round trip, the file on disk (a fresh "start" reads it back), the replace-refused path, a file that is not ours
        Rules a;
        a.hiddenModes = {"DVB-T2", "ADS-B", "FM (stereo)"}; a.hiddenGroups = {"AMATEUR - DIGITAL", "PLUGINS"}; a.favs = {"DAB+"}; a.favsOnly = true;
        Rules b;
        check(fromText(toText(a), b) && b.hiddenModes == a.hiddenModes && b.hiddenGroups == a.hiddenGroups && b.favs == a.favs && b.favsOnly, "text round trip");
        check(fromText(toText(Rules()), b) && isDefault(b), "default round trip");
        check(!fromText("hello\n", b) && !fromText("hiddenModes=a\n", b), "not ours / cut short");
        check(fromText("hiddenModes=x\r\nhiddenGroups=\r\nfavs=\r\nfavsOnly=0\r\n", b) && b.hiddenModes.count("x") == 1, "CRLF file (edited in Notepad)");
        namespace fs = std::filesystem;
        const std::string dir = (fs::temp_directory_path() / "onair_modevis_test").string(), path = dir + "/sub/modes-shown.txt";
        fs::remove_all(fs::path(dir));
        Rules c;
        check(!readFile(path, c), "no file yet");
        check(writeFile(path, a), "write creates the folder");
        check(readFile(path, c) && c.hiddenModes == a.hiddenModes && c.favsOnly, "read back after a restart");
        a.hiddenModes.erase("ADS-B"); a.favsOnly = false;
        check(writeFile(path, a) && readFile(path, c) && !c.hiddenModes.count("ADS-B") && !c.favsOnly, "a later change replaces the earlier");
        // a directory where the temp file goes: the temp write fails, nothing is claimed saved
        fs::create_directories(fs::path(dir + "/blocked/modes-shown.txt.tmp"));
        check(!writeFile(dir + "/blocked/modes-shown.txt", a), "unwritable target reports false");
        // the destination cannot be renamed over (a directory): the in-place write fails too and reports it
        fs::create_directories(fs::path(dir + "/dest/modes-shown.txt"));
        check(!writeFile(dir + "/dest/modes-shown.txt", a), "refused replace and in-place both fail: false");
        fs::remove_all(fs::path(dir));
    }
    if (!fails) printf("ok\n");
    return fails;
}
