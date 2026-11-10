// The daily digest: see digest_ui.h.
#include "app.h"
#include "adsb/digest_ui.h"
#include "dect2/automation/engine.h"
#include <ctime>
#include <map>

namespace {
std::map<std::string, std::function<std::string()>>& reg() { static std::map<std::string, std::function<std::string()>> m; return m; }
std::string gSentDay;
}

void digestRegister(const std::string& key, std::function<std::string()> line) { reg()[key] = std::move(line); }

std::string digestText() {
    std::string t;
    for (auto& kv : reg()) { const std::string l = kv.second ? kv.second() : ""; if (!l.empty()) t += (t.empty() ? "" : "; ") + l; }
    return t;
}

void digestTick() {
    static double last = 0;
    const double g = ImGui::GetTime();
    if (g - last < 20) return;
    last = g;
    namespace au = dect2::automation;
    if (!au::wants(au::FSession)) return;
    const time_t now = time(nullptr);
    struct tm l = *localtime(&now);
    const int at = (int)plat::prefs().getI("digestMinute", 21 * 60);   // minutes after local midnight
    char day[16]; std::strftime(day, sizeof day, "%Y-%m-%d", &l);
    if (gSentDay.empty()) gSentDay = plat::prefs().getS("digestSentDay", "");
    if (l.tm_hour * 60 + l.tm_min < at || gSentDay == day) return;
    gSentDay = day;
    plat::prefs().setS("digestSentDay", day);
    const std::string text = digestText();
    au::Event e("session.digest");
    e.at((double)now, "", 0);
    e.str("day", day).str("text", text.empty() ? "nothing heard today" : text).flag("empty", text.empty());
    au::emit(std::move(e));
}
