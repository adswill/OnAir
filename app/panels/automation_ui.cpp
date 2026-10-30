// View > Automations: rules ("when this happens, do that") and Lua scripts that act on what the receivers decode. The rules are built from
// drop-down lists; an expression and a script editor are there for the rest. Recipes are single JSON files in <data folder>/automations.
// The engine itself (events, rules, limits, the sandbox) is in core/src/automation; the actions are in automation_sink.cpp.
#include "../app.h"
#include "automation_internal.h"
#include "dect2/automation/catalog.h"
#include "dect2/automation/expr.h"
#include "dect2/automation/recipes.h"
#include "dect2/automation/script.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace {
namespace au = dect2::automation;
namespace fs = std::filesystem;
using au::ActionSpec;
using au::Recipe;
using au::Rule;
using au::Script;

fs::path pth(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }

int resizeCb(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize((size_t)d->BufTextLen);
        d->Buf = s->data();
    }
    return 0;
}
bool txt(const char* id, std::string& s, float w = 0, ImGuiInputTextFlags fl = 0) {
    if (w > 0) ImGui::SetNextItemWidth(w * gUi);
    else if (w < 0) ImGui::SetNextItemWidth(-1);
    return ImGui::InputText(id, s.data(), s.capacity() + 1, fl | ImGuiInputTextFlags_CallbackResize, resizeCb, &s);
}
bool txtMulti(const char* id, std::string& s, ImVec2 size, ImGuiInputTextFlags fl = 0) {
    return ImGui::InputTextMultiline(id, s.data(), s.capacity() + 1, size, fl | ImGuiInputTextFlags_CallbackResize, resizeCb, &s);
}

struct Item {
    Recipe r;
    std::string err;               // why it is not active ("" when it is)
    std::string jsonText;          // the file view of the Advanced tab
    bool jsonLoaded = false;
};

struct State {
    bool loaded = false;
    std::vector<Item> items;
    int sel = -1;
    bool allowRun = false;
    std::set<std::string> confirmed;
    std::string msg, dry;
    bool dryOk = false;
    bool recipesOpen = false;
    bool dirty = true;             // the engine needs the list again
    std::string confirmId;         // the run confirmation dialog is for this rule
    double lastStation = 0;
    std::string jsonErr;
    int deleteAsk = -1;
};
State S;

std::string dirPath() { return plat::dataDir() + "/automations"; }
std::string confirmPath() { return dirPath() + "/confirmed_runs.txt"; }

std::string uniqueId(std::string id) {
    auto taken = [&](const std::string& x) { for (const Item& i : S.items) if ((i.r.kind == Recipe::KindRule ? i.r.rule.id : i.r.script.id) == x) return true; return false; };
    if (!taken(id)) return id;
    for (int n = 2; n < 1000; n++) {
        const std::string c = id.substr(0, 44) + "-" + std::to_string(n);
        if (!taken(c)) return c;
    }
    return id;
}
std::string& idOf(Item& i) { return i.r.kind == Recipe::KindRule ? i.r.rule.id : i.r.script.id; }
bool& enabledOf(Item& i) { return i.r.kind == Recipe::KindRule ? i.r.rule.enabled : i.r.script.enabled; }
std::string& nameOf(Item& i) { return i.r.kind == Recipe::KindRule ? i.r.rule.name : i.r.script.name; }

void saveItem(Item& i) {
    std::error_code ec;
    fs::create_directories(pth(dirPath()), ec);
    std::ofstream f(pth(dirPath() + "/" + idOf(i) + ".json"), std::ios::binary | std::ios::trunc);
    if (f) f << au::recipeToText(i.r);
}
void saveConfirmed() {
    std::error_code ec;
    fs::create_directories(pth(dirPath()), ec);
    std::ofstream f(pth(confirmPath()), std::ios::binary | std::ios::trunc);
    for (const std::string& c : S.confirmed) f << c << "\n";
}

void validate(Item& i) {
    if (i.r.kind == Recipe::KindRule) i.err = au::validateRule(i.r.rule);
    else i.err = i.r.script.code.empty() ? "the script is empty" : au::checkScriptSyntax(i.r.script.code);
}

void loadAll() {
    S.loaded = true;
    S.allowRun = plat::prefs().getB("automationAllowRun", false);
    {
        std::ifstream f(pth(confirmPath()));
        std::string l;
        while (std::getline(f, l)) if (l.size() == 16) S.confirmed.insert(l);
    }
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(pth(dirPath()), ec)) if (e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& p : files) {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        Item it;
        std::string err;
        if (!au::recipeFromText(ss.str(), it.r, err, true)) continue;   // not a recipe, or a damaged file: left alone
        validate(it);
        S.items.push_back(std::move(it));
    }
    S.dirty = true;
}

int tzOffset() {
    const time_t t = time(nullptr);
    struct tm l = *localtime(&t), g = *gmtime(&t);
    l.tm_isdst = 0; g.tm_isdst = 0;
    return (int)difftime(mktime(&l), mktime(&g));
}

void applyEngine() {
    std::vector<Rule> rules;
    std::vector<Script> scripts;
    for (Item& i : S.items) {
        if (!i.err.empty()) continue;
        if (i.r.kind == Recipe::KindRule) rules.push_back(i.r.rule); else scripts.push_back(i.r.script);
    }
    au::Engine& e = au::global();
    e.setRules(rules);
    e.setScripts(scripts);
    e.setConfirmedRuns(S.confirmed);
    e.setAllowRun(S.allowRun);
    S.dirty = false;
}

void changed(Item& i) {
    validate(i);
    i.jsonLoaded = false;
    saveItem(i);
    S.dirty = true;
}

std::string clockText(double t) {
    if (t <= 0) return "-";
    const time_t tt = (time_t)t;
    char b[40];
    snprintf(b, sizeof b, "%s", dect2::timefmt::clock((int64_t)tt).c_str());
    return b;
}

// ---------- the pieces of the rule editor ----------
struct TrigOpt { std::string trigger, label; };
const std::vector<TrigOpt>& triggerOptions() {
    static std::vector<TrigOpt> v;
    if (!v.empty()) return v;
    v.push_back({"*", "Any event"});
    std::set<std::string> fams;
    for (const au::EventInfo& e : au::eventCatalog()) fams.insert(std::string(e.type).substr(0, std::string(e.type).find('.')));
    for (const au::EventInfo& e : au::eventCatalog()) v.push_back({e.type, std::string(e.type) + "  " + e.title});
    return v;
}

const char* const kOpLabels[] = {N_("equals"), N_("is not"), N_("is less than"), N_("is at most"), N_("is more than"), N_("is at least"), N_("contains"), N_("starts with"), N_("ends with"), N_("matches (* ?)"), N_("is one of (a, b, c)"), N_("has a value")};
const char* const kActLabels[] = {N_("Notification"), N_("Sound alert"), N_("Record IQ"), N_("Record audio"), N_("Save the rewind"), N_("Tune"), N_("Recall a memory"), N_("Write a CSV log row"), N_("Webhook"), N_("Save a screenshot"), N_("Run a program (advanced)")};
const char* const kPresets[] = {"generic", "discord", "slack", "ntfy", "telegram"};
const char* const kPresetLabels[] = {N_("Generic JSON"), "Discord", "Slack", "ntfy.sh", "Telegram"};

std::vector<std::string> fieldsOf(const std::string& trigger) {
    std::vector<std::string> o;
    if (const au::EventInfo* e = au::findEventInfo(trigger)) for (const au::FieldInfo& f : e->fields) o.push_back(f.name);
    return o;
}

bool comboStr(const char* id, std::string& cur, const std::vector<std::string>& opts, bool allowEmpty = false, const char* emptyLabel = "") {
    bool ch = false;
    if (ImGui::BeginCombo(id, cur.empty() ? emptyLabel : cur.c_str())) {
        if (allowEmpty && ImGui::Selectable(emptyLabel, cur.empty())) { cur.clear(); ch = true; }
        for (const std::string& o : opts) if (ImGui::Selectable(o.c_str(), o == cur)) { cur = o; ch = true; }
        ImGui::EndCombo();
    }
    return ch;
}

struct Edit { bool ch = false; };

// the event type a test uses for a trigger (a family or "any" takes its first type)
std::string sampleType(const std::string& trigger) {
    if (trigger == "*") return "adsb.aircraft";
    for (const au::EventInfo& e : au::eventCatalog()) if (au::triggerMatches(trigger, e.type)) return e.type;
    return "adsb.aircraft";
}

void editParam(Edit& e, ActionSpec& a, const char* key, const char* label, float w = 300) {
    std::string v = a.p.count(key) ? a.p[key] : std::string();
    ImGui::SetNextItemWidth(w * gUi);
    ImGui::PushID(key);
    ImGui::TextDisabled("%s", tr(label));
    ImGui::SameLine(130 * gUi);
    if (txt("##v", v, w)) { if (v.empty()) a.p.erase(key); else a.p[key] = v; e.ch = true; }
    ImGui::PopID();
}

int actIndex(const std::string& t) {
    const auto& ts = au::actionTypes();
    for (size_t i = 0; i < ts.size(); i++) if (ts[i] == t) return (int)i;
    return 0;
}

void defaultsFor(ActionSpec& a) {
    a.p.clear();
    if (a.type == "notify") { a.p["title"] = "{{rule}}"; a.p["body"] = "{{type}}"; }
    else if (a.type == "record_iq" || a.type == "record_audio") a.p["seconds"] = "60";
    else if (a.type == "save_rewind") a.p["seconds"] = "10";
    else if (a.type == "log") a.p["file"] = "automation";
    else if (a.type == "webhook") { a.p["preset"] = "generic"; a.p["url"] = "https://"; }
    else if (a.type == "tune") a.p["freq_mhz"] = "100.0";
}

void editAction(Edit& e, Rule& r, size_t idx, App& root) {
    ActionSpec& a = r.actions[idx];
    ImGui::PushID((int)idx);
    ImGui::SetNextItemWidth(230 * gUi);
    const int cur = actIndex(a.type);
    if (ImGui::BeginCombo("##type", tr(kActLabels[cur]))) {
        for (size_t k = 0; k < au::actionTypes().size(); k++) if (ImGui::Selectable(tr(kActLabels[k]), (int)k == cur)) { a.type = au::actionTypes()[k]; defaultsFor(a); e.ch = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Remove"))) { r.actions.erase(r.actions.begin() + (ptrdiff_t)idx); e.ch = true; ImGui::PopID(); return; }
    ImGui::Indent(14 * gUi);
    const std::string& t = a.type;
    if (t == "notify") { editParam(e, a, "title", N_("Title")); editParam(e, a, "body", N_("Text")); }
    else if (t == "sound") editParam(e, a, "file", N_("WAV file (empty: beeps)"));
    else if (t == "record_iq" || t == "record_audio") editParam(e, a, "seconds", N_("Seconds"), 90);
    else if (t == "save_rewind") editParam(e, a, "seconds", N_("Seconds to keep after the event"), 90);
    else if (t == "tune") { editParam(e, a, "freq_mhz", N_("Frequency, MHz"), 120); editParam(e, a, "mode", N_("Mode id (optional)"), 160); }
    else if (t == "memory") editParam(e, a, "name", N_("Memory name"));
    else if (t == "log") { editParam(e, a, "file", N_("File name"), 200); editParam(e, a, "row", N_("Columns (optional)")); }
    else if (t == "webhook") {
        std::string pr = a.p.count("preset") ? a.p["preset"] : "generic";
        int pi = 0;
        for (int k = 0; k < 5; k++) if (pr == kPresets[k]) pi = k;
        ImGui::TextDisabled("%s", tr("Service"));
        ImGui::SameLine(130 * gUi);
        ImGui::SetNextItemWidth(200 * gUi);
        if (ImGui::BeginCombo("##preset", tr(kPresetLabels[pi]))) {
            for (int k = 0; k < 5; k++) if (ImGui::Selectable(tr(kPresetLabels[k]), k == pi)) { a.p["preset"] = kPresets[k]; e.ch = true; }
            ImGui::EndCombo();
        }
        if (pr == "ntfy") { editParam(e, a, "topic", N_("Topic")); editParam(e, a, "title", N_("Title")); editParam(e, a, "text", N_("Text")); editParam(e, a, "url", N_("Server (optional)")); }
        else if (pr == "telegram") { editParam(e, a, "token", N_("Bot token")); editParam(e, a, "chat_id", N_("Chat id")); editParam(e, a, "text", N_("Text")); }
        else if (pr == "discord" || pr == "slack") { editParam(e, a, "url", N_("Webhook address")); editParam(e, a, "text", N_("Text")); }
        else { editParam(e, a, "url", N_("Address")); editParam(e, a, "body", N_("JSON body (optional)")); }
    } else if (t == "run") {
        editParam(e, a, "program", N_("Program"));
        {
            std::string v = a.p.count("args") ? a.p["args"] : std::string();
            ImGui::TextDisabled("%s", tr("Arguments, one per line"));
            if (txtMulti("##args", v, ImVec2(300 * gUi, 56 * gUi))) { if (v.empty()) a.p.erase("args"); else a.p["args"] = v; e.ch = true; }
        }
        ImGui::TextColored(pal::warnAmber(), "%s", tr("Runs a program on this computer. Off until you allow it below and confirm this rule."));
        const bool ok = S.allowRun && S.confirmed.count(au::runFingerprint(r));
        if (ok) ImGui::TextColored(pal::okGreen(), "%s", tr("Confirmed"));
        else if (ImGui::SmallButton(tr("Confirm this rule..."))) S.confirmId = r.id;
    }
    ImGui::Unindent(14 * gUi);
    ImGui::Separator();
    ImGui::PopID();
    (void)root;
}

void ruleEditor(Item& it, App& root) {
    Rule& r = it.r.rule;
    Edit e;
    ImGui::PushID("rule");
    ImGui::TextDisabled("%s", tr("Name"));
    ImGui::SameLine(130 * gUi);
    if (txt("##name", r.name, 320)) e.ch = true;
    ImGui::TextDisabled("%s", tr("Note"));
    ImGui::SameLine(130 * gUi);
    if (txt("##note", r.note, -1)) e.ch = true;
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", tr("When this happens"));
    const auto& opts = triggerOptions();
    std::string curLabel = r.trigger;
    for (const TrigOpt& o : opts) if (o.trigger == r.trigger) curLabel = o.label;
    ImGui::SetNextItemWidth(460 * gUi);
    if (ImGui::BeginCombo("##trigger", curLabel.c_str())) {
        for (const TrigOpt& o : opts) if (ImGui::Selectable(o.trigger == "*" ? tr("Any event") : o.label.c_str(), o.trigger == r.trigger)) { r.trigger = o.trigger; e.ch = true; }
        ImGui::EndCombo();
    }
    const std::vector<std::string> fields = fieldsOf(r.trigger);
    if (fields.empty()) ImGui::TextDisabled("%s", tr("Pick one event type to choose its fields from a list."));
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", tr("and only if"));
    for (size_t k = 0; k < r.conds.size(); k++) {
        Rule::Cond& c = r.conds[k];
        ImGui::PushID((int)k);
        if (fields.empty()) { if (txt("##f", c.field, 130)) e.ch = true; }
        else { ImGui::SetNextItemWidth(130 * gUi); if (comboStr("##f", c.field, fields)) e.ch = true; }
        ImGui::SameLine();
        int oi = 0;
        for (size_t q = 0; q < au::condOps().size(); q++) if (au::condOps()[q] == c.op) oi = (int)q;
        ImGui::SetNextItemWidth(170 * gUi);
        if (ImGui::BeginCombo("##o", tr(kOpLabels[oi]))) {
            for (size_t q = 0; q < au::condOps().size(); q++) if (ImGui::Selectable(tr(kOpLabels[q]), (int)q == oi)) { c.op = au::condOps()[q]; e.ch = true; }
            ImGui::EndCombo();
        }
        if (c.op != "has") { ImGui::SameLine(); if (txt("##v", c.value, 150)) e.ch = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Remove"))) { r.conds.erase(r.conds.begin() + (ptrdiff_t)k); e.ch = true; ImGui::PopID(); break; }
        ImGui::PopID();
    }
    if (ImGui::SmallButton(tr("Add a condition"))) { r.conds.push_back({fields.empty() ? "" : fields[0], "==", ""}); e.ch = true; }
    if (r.conds.size() > 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo("##any", tr(r.condAny ? "any of the conditions" : "all of the conditions"))) {
            if (ImGui::Selectable(tr("all of the conditions"), !r.condAny)) { r.condAny = false; e.ch = true; }
            if (ImGui::Selectable(tr("any of the conditions"), r.condAny)) { r.condAny = true; e.ch = true; }
            ImGui::EndCombo();
        }
    }
    const std::string eff = au::conditionExpr(r);
    if (!eff.empty()) { ImGui::PushTextWrapPos(); ImGui::TextDisabled("%s", eff.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", tr("Limits"));
    {
        int cd = (int)r.cooldownSec;
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputInt(tr("Quiet after firing, seconds"), &cd, 10, 60)) { r.cooldownSec = std::max(0, std::min(604800, cd)); e.ch = true; }
        ImGui::TextDisabled("%s", tr("separately for each"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170 * gUi);
        if (fields.empty()) { if (txt("##key", r.cooldownKey, 150)) e.ch = true; }
        else if (comboStr("##key", r.cooldownKey, fields, true, tr("(the whole rule)"))) e.ch = true;
        int mh = r.maxPerHour;
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputInt(tr("At most per hour (0: no limit)"), &mh, 1, 10)) { r.maxPerHour = std::max(0, std::min(10000, mh)); e.ch = true; }
        bool win = r.windowFromHour >= 0;
        if (ImGui::Checkbox(tr("Only between these local hours"), &win)) { r.windowFromHour = win ? 8 : -1; r.windowToHour = win ? 22 : -1; e.ch = true; }
        if (win) {
            ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
            if (ImGui::InputInt("##wf", &r.windowFromHour, 1, 1)) { r.windowFromHour = std::max(0, std::min(23, r.windowFromHour)); e.ch = true; }
            ImGui::SameLine(); ImGui::TextDisabled("%s", tr("to"));
            ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
            if (ImGui::InputInt("##wt", &r.windowToHour, 1, 1)) { r.windowToHour = std::max(0, std::min(23, r.windowToHour)); e.ch = true; }
        }
        const char* geoLabels[] = {N_("No location test"), N_("Within a distance of my receiver"), N_("Within a distance of a point"), N_("Inside an area (polygon)")};
        int gi = r.geo == Rule::GeoOff ? 0 : r.geo == Rule::GeoPolygon ? 3 : r.geoHome ? 1 : 2;
        ImGui::SetNextItemWidth(300 * gUi);
        if (ImGui::BeginCombo("##geo", tr(geoLabels[gi]))) {
            for (int k = 0; k < 4; k++) if (ImGui::Selectable(tr(geoLabels[k]), k == gi)) {
                r.geo = k == 0 ? Rule::GeoOff : k == 3 ? Rule::GeoPolygon : Rule::GeoCircle;
                r.geoHome = k == 1;
                if (k == 3 && r.geoPolygon.empty()) r.geoPolygon = "54.0,25.0; 54.0,26.0; 55.0,26.0; 55.0,25.0";
                e.ch = true;
            }
            ImGui::EndCombo();
        }
        if (r.geo == Rule::GeoCircle) {
            double rad = r.geoRadiusKm;
            ImGui::SetNextItemWidth(110 * gUi);
            if (ImGui::InputDouble(tr("Distance, km"), &rad, 1, 10, "%.1f")) { r.geoRadiusKm = std::max(0.1, std::min(20000.0, rad)); e.ch = true; }
            if (!r.geoHome) {
                ImGui::SetNextItemWidth(120 * gUi); if (ImGui::InputDouble(tr("Latitude"), &r.geoLat, 0, 0, "%.4f")) e.ch = true;
                ImGui::SameLine(); ImGui::SetNextItemWidth(120 * gUi); if (ImGui::InputDouble(tr("Longitude"), &r.geoLon, 0, 0, "%.4f")) e.ch = true;
            } else if (!receiverLocationSet()) ImGui::TextColored(pal::warnAmber(), "%s", tr("The receiver position is not set (ADS-B window)."));
        } else if (r.geo == Rule::GeoPolygon) {
            ImGui::TextDisabled("%s", tr("Points as lat,lon; lat,lon; ..."));
            if (txt("##poly", r.geoPolygon, -1)) e.ch = true;
        }
        if (r.geo != Rule::GeoOff) {
            int io = r.geoInside ? 0 : 1;
            ImGui::SetNextItemWidth(200 * gUi);
            if (ImGui::BeginCombo("##io", tr(io == 0 ? "when it is inside" : "when it is outside"))) {
                if (ImGui::Selectable(tr("when it is inside"), io == 0)) { r.geoInside = true; e.ch = true; }
                if (ImGui::Selectable(tr("when it is outside"), io == 1)) { r.geoInside = false; e.ch = true; }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Checkbox(tr("only as it comes in"), &r.geoEnterOnly)) e.ch = true;
            if (r.geoEnterOnly && r.cooldownKey.empty()) ImGui::TextColored(pal::warnAmber(), "%s", tr("Choose what to tell apart in \"separately for each\" so every thing has its own state."));
        }
    }
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s", tr("Then do"));
    for (size_t k = 0; k < r.actions.size(); k++) { const size_t before = r.actions.size(); editAction(e, r, k, root); if (r.actions.size() != before) break; }
    if (r.actions.size() < 8) {
        ImGui::SetNextItemWidth(230 * gUi);
        if (ImGui::BeginCombo("##addact", tr("Add an action..."))) {
            for (size_t k = 0; k < au::actionTypes().size(); k++) if (ImGui::Selectable(tr(kActLabels[k]))) {
                ActionSpec a; a.type = au::actionTypes()[k]; defaultsFor(a); r.actions.push_back(a); e.ch = true;
            }
            ImGui::EndCombo();
        }
    }
    if (!fields.empty()) {
        std::string list;
        for (const std::string& f : fields) list += "{{" + f + "}} ";
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled("%s %s{{rule}} {{type}} {{freq_mhz}} {{time_iso}}", tr("Text can use:"), list.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    if (!it.err.empty()) { ImGui::PushTextWrapPos(); ImGui::TextColored(pal::badRed(), "%s %s", tr("Not active:"), it.err.c_str()); ImGui::PopTextWrapPos(); }
    if (ImGui::Button(tr("Test (dry run)"))) {
        const au::Event ev = au::sampleEvent(sampleType(r.trigger), au::wallNow());
        const au::FireReport rep = au::global().test(r, ev, false);
        S.dry.clear();
        S.dryOk = rep.matched;
        if (!rep.matched) S.dry = std::string(tr("Does not fire for the sample event:")) + " " + rep.why;
        else for (const std::string& a : rep.actions) S.dry += a + "\n";
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Shows what the rule would do with a made-up event. Nothing is sent or started."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Fire with sample event"))) {
        const au::Event ev = au::sampleEvent(sampleType(r.trigger), au::wallNow());
        const au::FireReport rep = au::global().test(r, ev, true);
        S.dry.clear();
        S.dryOk = rep.matched;
        if (!rep.matched) S.dry = std::string(tr("Does not fire for the sample event:")) + " " + rep.why;
        else for (const std::string& a : rep.actions) S.dry += a + "\n";
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Really does the actions, once, with a made-up event."));
    if (!S.dry.empty()) { ImGui::PushTextWrapPos(); ImGui::TextColored(S.dryOk ? pal::okGreen() : pal::warnAmber(), "%s", S.dry.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::PopID();
    if (e.ch) changed(it);
}

// ---------- scripts ----------
struct CaptureApi : au::ScriptApi {
    std::string out;
    bool act(const std::string& k, const std::map<std::string, std::string>& p, std::string&) override {
        out += k + ":";
        for (const auto& kv : p) { std::string v = kv.second.substr(0, 120); for (char& c : v) if (c == '\n') c = ' '; out += " " + kv.first + "=" + v; }
        out += "\n";
        return true;
    }
    void note(const std::string& t) override { out += "log: " + t + "\n"; }
    bool station(double& la, double& lo) override { if (!receiverLocationSet()) return false; receiverLocation(la, lo); return true; }
    double now() override { return au::wallNow(); }
    int tzOffsetSec() override { return tzOffset(); }
};

void scriptEditor(Item& it) {
    Script& s = it.r.script;
    bool ch = false;
    ImGui::PushID("script");
    ImGui::TextDisabled("%s", tr("Name"));
    ImGui::SameLine(130 * gUi);
    if (txt("##name", s.name, 320)) ch = true;
    ImGui::TextDisabled("%s", tr("Note"));
    ImGui::SameLine(130 * gUi);
    if (txt("##note", s.note, -1)) ch = true;
    ImGui::TextColored(pal::heading(), "%s", tr("Receives these events"));
    bool all = s.events.empty();
    if (ImGui::Checkbox(tr("Every event"), &all)) { s.events.clear(); if (!all) s.events.push_back("adsb.*"); ch = true; }
    if (!all) {
        static const char* const fams[] = {"adsb", "acars", "vdl2", "hfdl", "pager", "ais", "aprs", "sonde", "gnss", "lora", "ble", "rid", "dsc", "signal", "receiver", "satellite", "time"};
        int n = 0;
        for (const char* f : fams) {
            const std::string t = std::string(f) + ".*";
            bool on = std::find(s.events.begin(), s.events.end(), t) != s.events.end();
            if (n++) ImGui::SameLine();
            if (ImGui::Checkbox(f, &on)) {
                if (on) s.events.push_back(t); else s.events.erase(std::remove(s.events.begin(), s.events.end(), t), s.events.end());
                if (s.events.empty()) s.events.push_back(t);
                ch = true;
            }
        }
    }
    ImGui::TextColored(pal::heading(), "%s", tr("Lua script"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::BeginCombo("##ex", tr("Insert an example..."))) {
        for (const Recipe& r : au::builtinRecipes()) if (r.kind == Recipe::KindScript && ImGui::Selectable(r.script.name.c_str())) { s.code = r.script.code; if (s.events.empty()) s.events = r.script.events; ch = true; }
        ImGui::EndCombo();
    }
    if (txtMulti("##code", s.code, ImVec2(-1, std::max(180.f, ImGui::GetContentRegionAvail().y - 190 * gUi)), ImGuiInputTextFlags_AllowTabInput)) ch = true;
    if (!it.err.empty()) { ImGui::PushTextWrapPos(); ImGui::TextColored(pal::badRed(), "%s", it.err.c_str()); ImGui::PopTextWrapPos(); }
    const std::string rt = au::global().scriptError(s.id);
    if (it.err.empty() && !rt.empty()) { ImGui::PushTextWrapPos(); ImGui::TextColored(pal::warnAmber(), "%s %s", tr("Last error:"), rt.c_str()); ImGui::PopTextWrapPos(); }
    if (ImGui::Button(tr("Test with a sample event")) && it.err.empty()) {
        CaptureApi api;
        au::ScriptHost host(s, &api);
        au::Event ev = au::sampleEvent(sampleType(s.events.empty() ? "*" : s.events[0]), au::wallNow());
        bool ok = host.loaded() && host.onEvent(ev);
        S.dry = ok ? (api.out.empty() ? std::string(tr("The script ran and did nothing for this event.")) : api.out) : host.error();
        S.dryOk = ok;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("What can a script do?"))) ImGui::OpenPopup("scripthelp");
    if (ImGui::BeginPopup("scripthelp")) {
        ImGui::PushTextWrapPos(560 * gUi);
        ImGui::TextUnformatted(tr("Define function on_event(e). e.type, e.mode, e.freq, e.time and the event's fields (e.callsign, e.squawk ...).\n\nActions: notify(title, text), sound(file), record_iq(seconds), record_audio(seconds), save_rewind(seconds), tune(hz, mode), memory(name), webhook(url, text_or_table), screenshot(), log(text), log_row(file, a, b, ...).\nHelpers: distance_km(lat, lon), bearing_deg(lat, lon), now(), hour(), json(table).\nText: s:contains(x), s:starts_with(x), s:ends_with(x), s:matches('a*b'), s:replace(a, b). No io, os, load, require, pcall or pattern matching; 1 million instructions, 250 ms and 16 MB per script."));
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    if (!S.dry.empty()) { ImGui::PushTextWrapPos(); ImGui::TextColored(S.dryOk ? pal::okGreen() : pal::warnAmber(), "%s", S.dry.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::PopID();
    if (ch) changed(it);
}

void advancedTab(Item& it) {
    if (it.r.kind == Recipe::KindRule) {
        Rule& r = it.r.rule;
        ImGui::TextColored(pal::heading(), "%s", tr("Expression (and'ed to the conditions above)"));
        ImGui::TextDisabled("%s", tr("Fields, and or not, comparisons, contains startswith endswith matches in, arithmetic, distance_km() within() inside() hour() ..."));
        std::string w = r.when;
        if (txt("##when", w, -1)) { r.when = w; changed(it); }
        au::ExprError ee;
        const std::string eff = au::conditionExpr(r);
        if (!au::Expr::parse(eff, ee)) { ImGui::TextColored(pal::badRed(), "%s", ee.text().c_str()); }
        else ImGui::TextColored(pal::okGreen(), "%s", tr("Expression is fine."));
        ImGui::Spacing();
    }
    ImGui::TextColored(pal::heading(), "%s", tr("The recipe file"));
    ImGui::TextDisabled("%s", tr("Edit as text and apply, or copy it to share."));
    if (!it.jsonLoaded) { it.jsonText = au::recipeToText(it.r); it.jsonLoaded = true; S.jsonErr.clear(); }
    txtMulti("##json", it.jsonText, ImVec2(-1, std::max(160.f, ImGui::GetContentRegionAvail().y - 70 * gUi)), ImGuiInputTextFlags_AllowTabInput);
    if (ImGui::Button(tr("Apply"))) {
        Recipe r; std::string err;
        if (au::recipeFromText(it.jsonText, r, err, true) && r.kind == it.r.kind) {
            const std::string oldId = idOf(it);
            it.r = r;
            idOf(it) = oldId;   // the file keeps its name
            changed(it);
            S.jsonErr.clear();
        } else S.jsonErr = err.empty() ? "the kind (rule / script) cannot change" : err;
    }
    if (!S.jsonErr.empty()) { ImGui::SameLine(); ImGui::TextColored(pal::badRed(), "%s", S.jsonErr.c_str()); }
}

void logTab() {
    const auto f = au::global().firings();
    if (ImGui::SmallButton(tr("Clear"))) au::global().clearFirings();
    ImGui::SameLine();
    ImGui::TextDisabled(tr("%d firings. Dropped events: %llu"), (int)f.size(), (unsigned long long)au::global().dropped());
    const std::string hs = automationapp::webhookStatus();
    if (!hs.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", hs.c_str()); }
    if (ImGui::BeginChild("##logbox", ImVec2(0, 0), true)) {
        for (size_t i = f.size(); i-- > 0;) {
            const au::Firing& x = f[i];
            ImGui::PushID((int)i);
            const ImVec4 col = !x.ok ? pal::badRed() : x.test ? pal::warnAmber() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::TextColored(col, "%s  %s%s  %s", clockText(x.time).c_str(), x.test ? "[test] " : "", x.rule.c_str(), x.summary.c_str());
            ImGui::Indent(12 * gUi);
            for (const std::string& a : x.actions) { ImGui::PushTextWrapPos(); ImGui::TextDisabled("%s", a.c_str()); ImGui::PopTextWrapPos(); }
            if (!x.error.empty()) ImGui::TextColored(pal::badRed(), "%s", x.error.c_str());
            ImGui::Unindent(12 * gUi);
            ImGui::PopID();
        }
        if (f.empty()) ImGui::TextDisabled("%s", tr("Nothing has fired yet."));
    }
    ImGui::EndChild();
}

void addItem(Recipe r, bool select = true) {
    Item it;
    it.r = std::move(r);
    idOf(it) = uniqueId(au::makeId(nameOf(it)));
    enabledOf(it) = false;
    validate(it);
    saveItem(it);
    S.items.push_back(std::move(it));
    if (select) S.sel = (int)S.items.size() - 1;
    S.dirty = true;
}

Recipe newRule() {
    Recipe r;
    r.kind = Recipe::KindRule;
    r.rule.name = "New rule";
    r.rule.trigger = "pager.message";
    r.rule.conds = {{"text", "contains", "fire"}};
    ActionSpec a; a.type = "notify"; defaultsFor(a);
    r.rule.actions = {a};
    return r;
}
Recipe newScript() {
    Recipe r;
    r.kind = Recipe::KindScript;
    r.script.name = "New script";
    r.script.events = {"adsb.aircraft"};
    r.script.code = "function on_event(e)\n  if e.squawk == 7700 then\n    notify('Emergency', (e.callsign or e.hex) .. ' squawks 7700')\n  end\nend\n";
    return r;
}

void runConfirmPopup() {
    if (!S.confirmId.empty()) { ImGui::OpenPopup(trw("Run a program?")); }
    if (ImGui::BeginPopupModal(trw("Run a program?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        Item* it = nullptr;
        for (Item& i : S.items) if (i.r.kind == Recipe::KindRule && i.r.rule.id == S.confirmId) it = &i;
        if (!it) { S.confirmId.clear(); ImGui::CloseCurrentPopup(); }
        else {
            ImGui::PushTextWrapPos(520 * gUi);
            ImGui::TextUnformatted(tr("This rule will start the program below, on this computer, every time it fires, with text from the decoded signals in its arguments. Anyone who can send such a signal can influence it. Only allow it if you wrote it or trust the one who did."));
            ImGui::PopTextWrapPos();
            ImGui::Separator();
            for (const ActionSpec& a : it->r.rule.actions) if (a.type == "run") {
                ImGui::Text("%s %s", tr("Program:"), a.p.count("program") ? a.p.at("program").c_str() : "");
                ImGui::Text("%s %s", tr("Arguments:"), a.p.count("args") ? a.p.at("args").c_str() : "");
            }
            ImGui::Separator();
            if (ImGui::Button(tr("Allow this rule to run it"))) {
                S.confirmed.insert(au::runFingerprint(it->r.rule));
                saveConfirmed();
                S.dirty = true;
                S.confirmId.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { S.confirmId.clear(); ImGui::CloseCurrentPopup(); }
        }
        ImGui::EndPopup();
    }
}

void recipesPopup() {
    if (S.recipesOpen) { ImGui::OpenPopup(trw("Recipes")); S.recipesOpen = false; }
    ImGui::SetNextWindowSize(ImVec2(620 * gUi, 440 * gUi), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::BeginPopupModal(trw("Recipes"), &open)) {
        ImGui::TextDisabled("%s", tr("Ready-made rules and scripts. Adding one makes a copy that you can edit; it starts switched off."));
        if (ImGui::BeginChild("##rl", ImVec2(0, -34 * gUi), true)) {
            int k = 0;
            for (const Recipe& r : au::builtinRecipes()) {
                ImGui::PushID(k++);
                ImGui::TextColored(pal::heading(), "%s%s", r.title().c_str(), r.kind == Recipe::KindScript ? " (Lua)" : "");
                ImGui::PushTextWrapPos();
                ImGui::TextDisabled("%s", (r.kind == Recipe::KindRule ? r.rule.note : r.script.note).c_str());
                ImGui::PopTextWrapPos();
                if (ImGui::SmallButton(tr("Add a copy"))) { addItem(r); }
                ImGui::Separator();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        if (ImGui::Button(tr("Close"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// Removes the selected rule or script and its file (#334: the button was only on the Manage tab, which people did not find)
void deleteSelected() {
    if (S.sel < 0 || S.sel >= (int)S.items.size()) return;
    std::error_code ec;
    fs::remove(pth(dirPath() + "/" + idOf(S.items[(size_t)S.sel]) + ".json"), ec);
    S.items.erase(S.items.begin() + S.sel);
    S.sel = -1; S.deleteAsk = -1; S.dirty = true;
}

void windowPanel(App& a) {
    firstWindowSize(980 * gUi, 640 * gUi);
    if (!ImGui::Begin(trw("Automations"), &a.automationOpen)) { ImGui::End(); return; }
    if (ImGui::Button(tr("New rule"))) addItem(newRule());
    ImGui::SameLine();
    if (ImGui::Button(tr("New script"))) addItem(newScript());
    ImGui::SameLine();
    if (ImGui::Button(tr("Recipes..."))) S.recipesOpen = true;
    ImGui::SameLine();
    if (ImGui::Button(tr("Import..."))) {
        const std::string p = plat::openFileDialog();
        if (!p.empty()) {
            std::ifstream f(pth(p), std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            Recipe r; std::string err;
            if (au::recipeFromText(ss.str(), r, err)) { addItem(r); S.msg = std::string(tr("Imported, switched off. Look it over, then switch it on.")); }
            else S.msg = std::string(tr("Not imported:")) + " " + err;
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(S.sel < 0 || S.sel >= (int)S.items.size());
    if (ImGui::Button(tr("Export..."))) {
        Item& it = S.items[(size_t)S.sel];
        const std::string p = plat::saveFileDialog((idOf(it) + ".json").c_str());
        if (!p.empty()) {
            std::ofstream f(pth(p), std::ios::binary | std::ios::trunc);
            Recipe copy = it.r;
            if (copy.kind == Recipe::KindRule) copy.rule.enabled = true; else copy.script.enabled = true;
            if (f) { f << au::recipeToText(copy); S.msg = std::string(tr("Saved to")) + " " + p; } else S.msg = tr("Cannot write the file.");
        }
    }
    ImGui::SameLine();
    if (S.deleteAsk != S.sel || S.sel < 0) {
        if (ImGui::Button(tr("Delete..."))) S.deleteAsk = S.sel;
    } else {
        if (ImGui::Button(tr("Really delete"))) deleteSelected();
        ImGui::SameLine();
        if (ImGui::Button(tr("Keep"))) S.deleteAsk = -1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Allow running programs"), &S.allowRun)) { plat::prefs().setB("automationAllowRun", S.allowRun); S.dirty = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Off. A rule may start a program only when this is on and you confirmed that rule."));
    if (!S.msg.empty()) { ImGui::TextDisabled("%s", S.msg.c_str()); }
    ImGui::Separator();
    ImGui::BeginChild("##list", ImVec2(270 * gUi, 0), true);
    for (size_t k = 0; k < S.items.size(); k++) {
        Item& it = S.items[k];
        ImGui::PushID((int)k);
        bool en = enabledOf(it);
        if (ImGui::Checkbox("##en", &en)) {
            enabledOf(it) = en;
            if (en && it.r.kind == Recipe::KindRule) for (const ActionSpec& x : it.r.rule.actions) if (x.type == "run" && !S.confirmed.count(au::runFingerprint(it.r.rule))) S.confirmId = it.r.rule.id;
            changed(it);
        }
        ImGui::SameLine();
        const au::RuleStats st = au::global().stats(idOf(it));
        char lbl[200];
        snprintf(lbl, sizeof lbl, "%s%s", nameOf(it).c_str(), it.r.kind == Recipe::KindScript ? " (Lua)" : "");
        if (ImGui::Selectable(lbl, S.sel == (int)k)) { S.sel = (int)k; S.dry.clear(); S.deleteAsk = -1; }
        if (!it.err.empty()) { ImGui::SameLine(); ImGui::TextColored(pal::badRed(), "!"); }
        ImGui::Indent(28 * gUi);
        ImGui::TextDisabled(tr("last fired %s, %d times"), clockText(st.lastFired).c_str(), st.fired);
        ImGui::Unindent(28 * gUi);
        ImGui::PopID();
    }
    if (S.items.empty()) ImGui::TextDisabled("%s", tr("No rules yet. Make one, or add a recipe."));
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##edit", ImVec2(0, 0), false);
    if (S.sel >= 0 && S.sel < (int)S.items.size()) {
        Item& it = S.items[(size_t)S.sel];
        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem(trw("Edit"))) { ImGui::BeginChild("##ed", ImVec2(0, 0), false); if (it.r.kind == Recipe::KindRule) ruleEditor(it, a); else scriptEditor(it); ImGui::EndChild(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(trw("Advanced"))) { advancedTab(it); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(trw("Log"))) { logTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(trw("Manage"))) {
                if (ImGui::Button(tr("Duplicate"))) { Recipe c = it.r; (c.kind == Recipe::KindRule ? c.rule.name : c.script.name) += " copy"; addItem(c); }
                ImGui::SameLine();
                if (S.deleteAsk != S.sel) { if (ImGui::Button(tr("Delete..."))) S.deleteAsk = S.sel; }
                else {
                    if (ImGui::Button(tr("Really delete"))) deleteSelected();
                    ImGui::SameLine();
                    if (ImGui::Button(tr("Keep"))) S.deleteAsk = -1;
                }
                ImGui::TextDisabled("%s %s/%s.json", tr("File:"), dirPath().c_str(), S.sel >= 0 && S.sel < (int)S.items.size() ? idOf(S.items[(size_t)S.sel]).c_str() : "");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    } else {
        ImGui::TextDisabled("%s", tr("Choose a rule on the left, or make a new one."));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", tr("Rules: when something is decoded (an aircraft squawks 7700, a pager message says fire, a vessel enters an area ...), do something (notify, record, tune, log, call a webhook). Scripts do the same in Lua."));
        ImGui::Spacing();
        logTab();
    }
    ImGui::EndChild();
    runConfirmPopup();
    recipesPopup();
    ImGui::End();
}
} // namespace

void automationWindow(App& a) {
    if (a.rxId != 1) {   // the automations belong to the program, not to one receiver: RX 1's window
        if (App* f = receiverById(1)) { f->automationOpen = a.automationOpen; automationWindow(*f); a.automationOpen = f->automationOpen; }
        return;
    }
    au::Engine& eng = au::global();
    static bool started = false;
    if (!S.loaded) loadAll();
    if (!started) {
        started = true;
        eng.setSink(&automationapp::sink());
        automationapp::autoRecordInstall();
        eng.setLogDir(dirPath() + "/logs");
        eng.start();
    }
    const double t = ImGui::GetTime();
    if (t - S.lastStation > 2.0) {   // the receiver position and the time zone can change while the program runs
        S.lastStation = t;
        double la = 0, lo = 0;
        const bool set = receiverLocationSet();
        if (set) receiverLocation(la, lo);
        eng.setStation(set, la, lo);
        eng.setTzOffset(tzOffset());
    }
    if (S.dirty) applyEngine();
    automationapp::uiTick(a);
    automationapp::drawBanner();
    if (a.automationOpen) windowPanel(a);
}
