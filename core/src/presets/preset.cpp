#include "dect2/presets/preset.h"
#include "dect2/engine/retired_modes.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace dect2::presets {
namespace J = automation::json;

const std::vector<SettingSpec>& settingSpecs() {
    static const std::vector<SettingSpec> t = {
        {"freqMhz",        Kind::Num,  0.001, 1000000.0, Group::Tuning,   false},
        {"bandwidthMhz",   Kind::Num,  0.0001, 100.0,    Group::Tuning,   false},
        {"sampleRate",     Kind::Num,  10e3, 1e9,        Group::Radio,    false},
        {"gainDb",         Kind::Num,  -20, 120,         Group::Gain,     false},
        {"autoGain",       Kind::Flag, 0, 1,             Group::Gain,     false},
        {"lnaDb",          Kind::Num,  0, 40,            Group::Gain,     false},
        {"vgaDb",          Kind::Num,  0, 62,            Group::Gain,     false},
        {"ampOn",          Kind::Flag, 0, 1,             Group::Gain,     false},
        {"biasTee",        Kind::Flag, 0, 1,             Group::Radio,    false},
        {"antenna",        Kind::Text, 0, 0,             Group::Radio,    false},
        {"radio",          Kind::Text, 0, 0,             Group::Radio,    false},   // a hint only, never switches the radio
        {"squelchDb",      Kind::Num,  -140, 20,         Group::Audio,    false},
        {"audio.volume",   Kind::Num,  0, 2,             Group::Audio,    false},
        {"audio.muted",    Kind::Flag, 0, 1,             Group::Audio,    false},
        {"audio.hpHz",     Kind::Num,  0, 20000,         Group::Audio,    false},
        {"audio.lpHz",     Kind::Num,  0, 24000,         Group::Audio,    false},
        {"audio.sqOpenDb", Kind::Num,  -140, 20,         Group::Audio,    false},
        {"audio.sqHystDb", Kind::Num,  0, 40,            Group::Audio,    false},
        {"audio.sqHangMs", Kind::Num,  0, 10000,         Group::Audio,    false},
        {"audio.shiftHz",  Kind::Num,  -1000, 1000,      Group::Audio,    false},
        {"audio.ctcssHz",  Kind::Num,  0, 300,           Group::Audio,    false},
        {"audio.latencyMs",Kind::Num,  0, 5000,          Group::Audio,    false},
        {"spec.refDb",     Kind::Num,  -200, 100,        Group::Spectrum, false},
        {"spec.rangeDb",   Kind::Num,  5, 250,           Group::Spectrum, false},
        {"spec.avg",       Kind::Num,  0, 1000,          Group::Spectrum, false},
        {"spec.zoom",      Kind::Num,  0, 1,             Group::Spectrum, false},
        {"spec.fftSize",   Kind::Num,  64, 1 << 20,      Group::Spectrum, false},
        {"loc.lat",        Kind::Num,  -90, 90,          Group::Location, true},
        {"loc.lon",        Kind::Num,  -180, 180,        Group::Location, true},
    };
    return t;
}

const SettingSpec* findSpec(const std::string& key) {
    static const SettingSpec opt = {"opt.", Kind::Num, -1e12, 1e12, Group::Option, false};
    for (const auto& s : settingSpecs()) if (key == s.key) return &s;
    if (key.size() > 4 && key.compare(0, 4, "opt.") == 0 && key.size() <= 64) return &opt;
    return nullptr;
}

bool looksSecret(const std::string& key) {
    std::string k;
    for (char c : key) k += (char)std::tolower((unsigned char)c);
    for (const char* w : {"key", "token", "password", "passwd", "secret", "apikey", "auth", "credential", "cookie", "bearer"})
        if (k.find(w) != std::string::npos) return true;
    return false;
}

std::string validate(const std::string& key, const Value& v) {
    const SettingSpec* s = findSpec(key);
    if (!s) return "unknown setting '" + key + "'";
    if (looksSecret(key)) return "'" + key + "' is not stored in presets";
    if (key.compare(0, 4, "opt.") == 0) {   // mode options: a finite number, a flag or a short text
        if (v.kind == Kind::Num && !std::isfinite(v.num)) return key + " is not a finite number";
        if (v.kind == Kind::Text && v.text.size() > 256) return key + " is too long";
        if (v.kind == Kind::Num && (v.num < s->lo || v.num > s->hi)) return key + " is out of range";
        return {};
    }
    if (v.kind != s->kind) return key + " has the wrong type";
    if (v.kind == Kind::Num) {
        if (!std::isfinite(v.num)) return key + " is not a finite number";
        if (v.num < s->lo || v.num > s->hi) return key + " is out of range (" + std::to_string(v.num) + ")";
    }
    if (v.kind == Kind::Text && v.text.size() > 128) return key + " is too long";
    return {};
}

ApplyResult applyPreset(const Preset& p, const RadioCaps& c) {
    ApplyResult r;
    auto skip = [&](const std::string& note) { r.notes.push_back(note); ++r.skipped; };
    for (const auto& [k, v] : p.values) {
        const std::string why = validate(k, v);
        if (!why.empty()) {
            r.notes.push_back(why);
            findSpec(k) && !looksSecret(k) ? ++r.rejected : ++r.skipped;
            continue;
        }
        if (k == "radio") { continue; }   // hint only
        if (k == "freqMhz" && (v.num < c.minMhz || v.num > c.maxMhz)) { skip("frequency is outside this radio's range"); continue; }
        if (k == "sampleRate" && (v.num < c.minRate || v.num > c.maxRate)) { skip("sample rate is not available on this radio"); continue; }
        if (k == "gainDb" && (v.num < c.minGainDb || v.num > c.maxGainDb)) { skip("gain is outside this radio's range"); continue; }
        if (k == "autoGain" && v.flag && !c.hasAutoGain) { skip("this radio has no automatic gain"); continue; }
        if (k == "biasTee" && v.flag && !c.hasBiasTee) { skip("this radio cannot power the antenna"); continue; }
        if (k == "antenna") {
            if (std::find(c.antennas.begin(), c.antennas.end(), v.text) == c.antennas.end()) { skip("antenna '" + v.text + "' is not on this radio"); continue; }
        }
        r.apply[k] = v;
    }
    return r;
}

// ---------------------------------------------------------------- JSON

static J::Value valueToJson(const Value& v) {
    switch (v.kind) {
    case Kind::Num: return J::Value::number(v.num);
    case Kind::Flag: return J::Value::boolean(v.flag);
    default: return J::Value::string(v.text);
    }
}

static bool jsonToValue(const J::Value& j, Value& v) {
    switch (j.type) {
    case J::Value::Num: v = Value::n(j.n); return true;
    case J::Value::Bool: v = Value::f(j.b); return true;
    case J::Value::Str: v = Value::t(j.s); return true;
    default: return false;
    }
}

static void copyExtra(const J::Value& from, J::Value& to) {
    if (from.type != J::Value::Obj) return;
    for (const auto& [k, v] : from.o) if (!looksSecret(k)) to.set(k, v);
}

J::Value toJson(const Preset& p, const ExportOptions& o) {
    J::Value j = J::Value::object();
    copyExtra(p.extra, j);   // first, so the known fields below win
    j.set("format", J::Value::string("onair-preset"));
    j.set("version", J::Value::number(std::max(p.version, kSchemaVersion)));
    j.set("name", J::Value::string(p.name));
    j.set("mode", J::Value::string(p.mode));
    J::Value vals = J::Value::object();
    for (const auto& [k, v] : p.values) {
        if (looksSecret(k)) continue;
        const SettingSpec* s = findSpec(k);
        if (s && s->sensitive && !o.includeLocation) continue;
        vals.set(k, valueToJson(v));
    }
    if (p.extraValues.type == J::Value::Obj)
        for (const auto& [k, v] : p.extraValues.o) if (!looksSecret(k) && !(k.compare(0, 4, "loc.") == 0 && !o.includeLocation)) vals.set(k, v);
    j.set("values", vals);
    return j;
}

bool fromJson(const J::Value& j, Preset& out, std::string& err) {
    err.clear();
    if (j.type != J::Value::Obj) { err = "not a preset"; return false; }
    if (j.str("format") != "onair-preset") { err = "not an OnAir preset file"; return false; }
    Preset p;
    p.version = (int)j.num("version", 1);
    if (p.version < 1) { err = "bad version"; return false; }
    p.name = j.str("name");
    p.mode = j.str("mode");
    if (p.name.empty() || p.name.size() > 80) { err = "the preset has no usable name"; return false; }
    if (p.mode.empty() || p.mode.size() > 40) { err = "the preset has no mode"; return false; }
    p.extra = J::Value::object();
    for (const auto& [k, v] : j.o) {
        if (k == "format" || k == "version" || k == "name" || k == "mode" || k == "values") continue;
        if (!looksSecret(k)) p.extra.set(k, v);
    }
    p.extraValues = J::Value::object();
    if (const J::Value* vals = j.get("values"); vals && vals->type == J::Value::Obj) {
        for (const auto& [k, jv] : vals->o) {
            if (looksSecret(k)) { err += "dropped '" + k + "'; "; continue; }
            if (!findSpec(k)) { p.extraValues.set(k, jv); continue; }   // newer version's key: kept, not applied
            Value v;
            if (!jsonToValue(jv, v)) { err += "dropped '" + k + "' (bad type); "; continue; }
            const std::string why = validate(k, v);
            if (!why.empty()) { err += "dropped: " + why + "; "; continue; }
            p.values[k] = v;
        }
    }
    out = std::move(p);
    return true;
}

std::string exportText(const Preset& p, const ExportOptions& o) { return J::dump(toJson(p, o), true) + "\n"; }

bool importText(const std::string& text, Preset& out, std::string& err) {
    J::Value j;
    if (!J::parse(text, j, err)) return false;
    Preset p;
    if (!fromJson(j, p, err)) return false;
    p.builtin = false;
    out = std::move(p);
    return true;
}

void mergeUnknown(Preset& edited, const Preset& stored) {
    if (stored.extra.type == J::Value::Obj) {
        if (edited.extra.type != J::Value::Obj) edited.extra = J::Value::object();
        for (const auto& [k, v] : stored.extra.o) if (!edited.extra.get(k)) edited.extra.set(k, v);
    }
    if (stored.extraValues.type == J::Value::Obj) {
        if (edited.extraValues.type != J::Value::Obj) edited.extraValues = J::Value::object();
        for (const auto& [k, v] : stored.extraValues.o) if (!edited.extraValues.get(k)) edited.extraValues.set(k, v);
    }
    edited.version = std::max(edited.version, stored.version);
}

// ---------------------------------------------------------------- store

const Preset* Store::find(const std::string& id) const {
    for (const auto& p : user) if (p.id == id) return &p;
    for (const auto& p : builtinPresets()) if (p.id == id) return &p;
    return nullptr;
}

bool Store::isFavourite(const std::string& id) const { return std::find(favourite.begin(), favourite.end(), id) != favourite.end(); }

std::vector<const Preset*> Store::forMode(const std::string& mode) const {
    std::vector<const Preset*> r;
    for (const auto& p : user) if (liveModeId(p.mode) == mode) r.push_back(&p);   // a preset of a 4.0 type entry belongs to its receiver (retired_modes.h)
    for (const auto& p : builtinPresets()) if (p.mode == mode) r.push_back(&p);
    std::stable_sort(r.begin(), r.end(), [&](const Preset* a, const Preset* b) {
        const bool fa = isFavourite(a->id), fb = isFavourite(b->id);
        if (fa != fb) return fa;
        if (a->builtin != b->builtin) return !a->builtin;
        return a->name < b->name;
    });
    return r;
}

std::vector<const Preset*> Store::favourites() const {
    std::vector<const Preset*> r;
    for (const auto& id : favourite) if (const Preset* p = find(id)) r.push_back(p);
    return r;
}

std::string Store::add(Preset p) {
    p.builtin = false;
    if (p.name.empty()) p.name = "Preset";
    const std::string base = p.name;
    for (int n = 2;; ++n) {   // a unique name within the mode
        bool taken = false;
        for (const Preset* q : forMode(p.mode)) if (q->name == p.name) taken = true;
        if (!taken) break;
        p.name = base + " " + std::to_string(n);
    }
    int n = 1;
    for (;; ++n) {
        const std::string id = "user:" + std::to_string(n);
        if (!find(id)) { p.id = id; break; }
    }
    user.push_back(std::move(p));
    return user.back().id;
}

bool Store::rename(const std::string& id, const std::string& name) {
    if (name.empty() || name.size() > 80) return false;
    for (auto& p : user) {
        if (p.id != id) continue;
        for (const Preset* q : forMode(p.mode)) if (q->id != id && q->name == name) return false;
        p.name = name;
        return true;
    }
    return false;
}

bool Store::remove(const std::string& id) {
    auto it = std::find_if(user.begin(), user.end(), [&](const Preset& p) { return p.id == id; });
    if (it == user.end()) return false;
    user.erase(it);
    recent.erase(std::remove(recent.begin(), recent.end(), id), recent.end());
    favourite.erase(std::remove(favourite.begin(), favourite.end(), id), favourite.end());
    for (auto d = defaultFor.begin(); d != defaultFor.end();) d = d->second == id ? defaultFor.erase(d) : std::next(d);
    return true;
}

bool Store::setFavourite(const std::string& id, bool fav) {
    if (!find(id)) return false;
    favourite.erase(std::remove(favourite.begin(), favourite.end(), id), favourite.end());
    if (fav) favourite.push_back(id);
    return true;
}

bool Store::setDefault(const std::string& mode, const std::string& id) {
    if (id.empty()) { defaultFor.erase(mode); return true; }
    const Preset* p = find(id);
    if (!p || p->mode != mode) return false;
    defaultFor[mode] = id;
    return true;
}

void Store::touch(const std::string& id) {
    if (!find(id)) return;
    recent.erase(std::remove(recent.begin(), recent.end(), id), recent.end());
    recent.insert(recent.begin(), id);
    if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
}

J::Value storeToJson(const Store& s) {
    J::Value j = J::Value::object();
    copyExtra(s.extra, j);
    j.set("format", J::Value::string("onair-presets"));
    j.set("version", J::Value::number(kSchemaVersion));
    J::Value arr = J::Value::array();
    for (const auto& p : s.user) {
        J::Value e = toJson(p, ExportOptions{true});   // the user's own file keeps the location they chose to save
        e.set("id", J::Value::string(p.id));
        arr.push(e);
    }
    j.set("presets", arr);
    J::Value rec = J::Value::array();
    for (const auto& r : s.recent) rec.push(J::Value::string(r));
    j.set("recent", rec);
    J::Value fav = J::Value::array();
    for (const auto& r : s.favourite) fav.push(J::Value::string(r));
    j.set("favourites", fav);
    J::Value def = J::Value::object();
    for (const auto& [m, id] : s.defaultFor) def.set(m, J::Value::string(id));
    j.set("defaults", def);
    return j;
}

bool storeFromJson(const J::Value& j, Store& out, std::string& err) {
    err.clear();
    if (j.type != J::Value::Obj || j.str("format") != "onair-presets") { err = "not a presets file"; return false; }
    Store s;
    s.extra = J::Value::object();
    for (const auto& [k, v] : j.o) {
        if (k == "format" || k == "version" || k == "presets" || k == "recent" || k == "favourites" || k == "defaults") continue;
        if (!looksSecret(k)) s.extra.set(k, v);
    }
    if (const J::Value* a = j.get("presets"); a && a->type == J::Value::Arr) {
        for (const auto& e : a->a) {
            Preset p; std::string e2;
            if (!fromJson(e, p, e2)) { err += "skipped a preset: " + e2 + "; "; continue; }
            p.id = e.str("id");
            if (p.id.empty() || s.find(p.id)) p.id = "user:" + std::to_string(s.user.size() + 1000 + (int)s.user.size());
            s.user.push_back(std::move(p));
        }
    }
    auto ids = [&](const char* key, std::vector<std::string>& dst) {
        if (const J::Value* a = j.get(key); a && a->type == J::Value::Arr)
            for (const auto& e : a->a) if (e.type == J::Value::Str && s.find(e.s)) dst.push_back(e.s);
    };
    ids("recent", s.recent);
    if (s.recent.size() > Store::kMaxRecent) s.recent.resize(Store::kMaxRecent);
    ids("favourites", s.favourite);
    if (const J::Value* d = j.get("defaults"); d && d->type == J::Value::Obj)
        for (const auto& [m, v] : d->o) if (v.type == J::Value::Str) { const Preset* p = s.find(v.s); if (p && p->mode == m) s.defaultFor[m] = v.s; }
    out = std::move(s);
    return true;
}

std::string storeText(const Store& s) { return J::dump(storeToJson(s), true) + "\n"; }

bool loadStoreText(const std::string& text, Store& out, std::string& err) {
    J::Value j;
    if (!J::parse(text, j, err)) return false;
    return storeFromJson(j, out, err);
}

bool saveStoreFile(const std::string& path, const Store& s, std::string& err) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot write " + tmp; return false; }
        f << storeText(s);
        if (!f) { err = "write failed"; return false; }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(path.c_str());   // Windows: rename does not replace
        if (std::rename(tmp.c_str(), path.c_str()) != 0) { err = "cannot replace " + path; return false; }
    }
    return true;
}

bool loadStoreFile(const std::string& path, Store& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { out = Store{}; err.clear(); return true; }
    std::stringstream ss; ss << f.rdbuf();
    return loadStoreText(ss.str(), out, err);
}

std::string suggestedFileName(const Preset& p) {
    std::string n;
    for (char c : p.name) n += (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ') ? c : '_';
    if (n.empty()) n = "preset";
    return n + ".onairpreset";
}

// ---------------------------------------------------------------- built-ins

const std::vector<Preset>& builtinPresets() {
    static const std::vector<Preset> t = [] {
        std::vector<Preset> v;
        auto mk = [&](const char* id, const char* name, const char* mode, std::initializer_list<std::pair<const char*, Value>> vals) {
            Preset p; p.id = std::string("builtin:") + id; p.name = name; p.mode = mode; p.builtin = true;
            for (const auto& kv : vals) p.values[kv.first] = kv.second;
            v.push_back(std::move(p));
        };
        using V = Value;
        mk("fm-broadcast", "FM broadcast", "fm", {{"freqMhz", V::n(100.0)}, {"bandwidthMhz", V::n(0.2)}, {"sampleRate", V::n(2.4e6)}, {"autoGain", V::f(true)}, {"audio.lpHz", V::n(15000)}});
        mk("airband-am", "Airband AM", "airband", {{"freqMhz", V::n(121.5)}, {"bandwidthMhz", V::n(0.025)}, {"sampleRate", V::n(2.4e6)}, {"gainDb", V::n(35)}, {"squelchDb", V::n(-60)}, {"audio.hpHz", V::n(300)}, {"audio.lpHz", V::n(3500)}});
        mk("marine-fm", "Marine VHF channel 16", "marine", {{"freqMhz", V::n(156.8)}, {"bandwidthMhz", V::n(0.025)}, {"sampleRate", V::n(2.4e6)}, {"gainDb", V::n(35)}, {"squelchDb", V::n(-60)}});
        mk("adsb-1090", "ADS-B 1090 MHz", "adsb", {{"freqMhz", V::n(1090.0)}, {"sampleRate", V::n(2e6)}, {"gainDb", V::n(40)}});
        mk("ham-20m-usb", "Ham 20 m SSB (USB)", "general", {{"freqMhz", V::n(14.2)}, {"bandwidthMhz", V::n(0.0027)}, {"sampleRate", V::n(2e6)}, {"gainDb", V::n(30)}, {"audio.hpHz", V::n(300)}, {"audio.lpHz", V::n(2700)}, {"opt.sideband", V::n(1)}});
        mk("ham-40m-lsb", "Ham 40 m SSB (LSB)", "general", {{"freqMhz", V::n(7.1)}, {"bandwidthMhz", V::n(0.0027)}, {"sampleRate", V::n(2e6)}, {"gainDb", V::n(30)}, {"audio.hpHz", V::n(300)}, {"audio.lpHz", V::n(2700)}, {"opt.sideband", V::n(-1)}});
        mk("noaa-apt", "NOAA APT weather satellite", "meteor", {{"freqMhz", V::n(137.62)}, {"bandwidthMhz", V::n(0.04)}, {"sampleRate", V::n(2.4e6)}, {"gainDb", V::n(40)}});
        mk("dab-5c", "DAB 5C", "dab", {{"freqMhz", V::n(178.352)}, {"bandwidthMhz", V::n(1.536)}, {"sampleRate", V::n(2.048e6)}, {"autoGain", V::f(true)}});
        mk("dvbt-8mhz", "DVB-T 8 MHz UHF", "dvbt", {{"freqMhz", V::n(626.0)}, {"bandwidthMhz", V::n(8.0)}, {"sampleRate", V::n(9.142857e6)}, {"gainDb", V::n(30)}});
        return v;
    }();
    return t;
}

}  // namespace dect2::presets
