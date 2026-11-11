// Sub-GHz sensor names, move hints and the sensor.reading event: see sensors_names.h.
#include "dect2/sensors/sensors_names.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2 {

namespace {
std::string cleanName(const std::string& s) {
    std::string o;
    for (unsigned char c : s) o += (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    size_t a = 0, b = o.size();
    while (a < b && o[a] == ' ') a++;
    while (b > a && o[b - 1] == ' ') b--;
    o = o.substr(a, b - a);
    if (o.size() > 60) {
        size_t n = 60;
        while (n > 0 && ((unsigned char)o[n] & 0xC0) == 0x80) n--;   // not in the middle of a UTF-8 character
        o.resize(n);
    }
    return o;
}
const SensorDevice* findDev(const SensorsTelemetry& t, const std::string& key) {
    for (const auto& d : t.devices) if (d.key == key) return &d;
    return nullptr;
}
}  // namespace

bool SensorNames::splitKey(const std::string& key, std::string& model, std::string& id, std::string& channel) {
    const size_t a = key.find('|');
    if (a == std::string::npos) return false;
    const size_t b = key.find('|', a + 1);
    if (b == std::string::npos) return false;
    model = key.substr(0, a);
    id = key.substr(a + 1, b - a - 1);
    channel = key.substr(b + 1);
    return true;
}

void SensorNames::set(const std::string& key, const std::string& name, int64_t now) {
    const std::string n = cleanName(name);
    if (n.empty() || key.empty()) { byKey_.erase(key); return; }
    SensorName& e = byKey_[key];
    e.name = n;
    std::string id;
    splitKey(key, e.model, id, e.channel);
    if (now > e.lastSeen) e.lastSeen = now;
}

std::string SensorNames::get(const std::string& key) const {
    auto it = byKey_.find(key);
    return it == byKey_.end() ? std::string() : it->second.name;
}

bool SensorNames::move(const std::string& fromKey, const std::string& toKey, int64_t now) {
    auto it = byKey_.find(fromKey);
    if (it == byKey_.end() || fromKey == toKey) return false;
    const std::string n = it->second.name;
    byKey_.erase(it);
    set(toKey, n, now);
    return true;
}

bool SensorNames::touch(const SensorsTelemetry& t) {
    bool worth = false;
    for (auto& kv : byKey_) {
        const SensorDevice* d = findDev(t, kv.first);
        if (!d || d->lastSeen <= kv.second.lastSeen) continue;
        if (d->lastSeen - kv.second.lastSeen >= 600) worth = true;
        kv.second.lastSeen = d->lastSeen;
    }
    return worth;
}

std::vector<SensorMoveHint> SensorNames::hints(const SensorsTelemetry& t, int64_t now) const {
    struct Cand { std::string from, to, name; int64_t gap; };
    std::vector<Cand> c;
    for (const auto& kv : byKey_) {
        const SensorName& n = kv.second;
        if (n.lastSeen <= 0) continue;
        const SensorDevice* old = findDev(t, kv.first);
        // still sending: no hint (silent means not heard for 3 of its intervals, or 10 min when the interval is not known)
        const double quiet = old && old->health.intervalSec > 0 ? std::max(180.0, 3 * old->health.intervalSec) : 600.0;
        if (old && (double)(now - old->lastSeen) < quiet) continue;
        for (const auto& d : t.devices) {
            if (d.key == kv.first || byKey_.count(d.key)) continue;
            std::string m, id, ch;
            if (!splitKey(d.key, m, id, ch) || m != n.model || ch != n.channel) continue;
            if (d.firstSeen < n.lastSeen || d.firstSeen - n.lastSeen > 6 * 3600) continue;
            c.push_back({kv.first, d.key, n.name, d.firstSeen - n.lastSeen});
        }
    }
    std::sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) { return a.gap != b.gap ? a.gap < b.gap : a.from < b.from; });
    std::vector<SensorMoveHint> out;
    for (const Cand& x : c) {
        bool used = false;
        for (const auto& h : out) if (h.toKey == x.to || h.fromKey == x.from) used = true;
        if (!used) out.push_back({x.from, x.to, x.name});
    }
    return out;
}

std::map<std::string, std::string> SensorNames::plain() const {
    std::map<std::string, std::string> m;
    for (const auto& kv : byKey_) m[kv.first] = kv.second.name;
    return m;
}

std::string SensorNames::serialize() const {
    std::string o;
    for (const auto& kv : byKey_) o += kv.first + "\t" + kv.second.name + "\t" + std::to_string((long long)kv.second.lastSeen) + "\n";
    return o;
}

void SensorNames::parse(const std::string& text) {
    byKey_.clear();
    std::stringstream ss(text);
    std::string l;
    while (std::getline(ss, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        const size_t a = l.find('\t');
        if (a == std::string::npos || a == 0) continue;
        const size_t b = l.find('\t', a + 1);
        const std::string key = l.substr(0, a), name = l.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
        const int64_t seen = b == std::string::npos ? 0 : (int64_t)std::atoll(l.c_str() + b + 1);
        std::string m, id, ch;
        if (!splitKey(key, m, id, ch)) continue;
        set(key, name, seen);
    }
}

std::string sensorLabel(const SensorDevice& d, const SensorNames& names) {
    const std::string n = names.get(d.key);
    if (!n.empty()) return n;
    return d.id.empty() ? d.model : d.model + " " + d.id;
}

automation::Event sensorReadingEvent(const SensorDevice& d, const std::string& userName, double timeSec, double freqHz) {
    automation::Event ev("sensor.reading");
    ev.at(timeSec, "Sensors", freqHz);
    for (const auto& f : d.latest) {
        if (f.key.empty() || f.key == "model" || f.key == "id" || f.key == "channel" || f.key == "time" || f.key == "mic") continue;
        if (f.numeric) ev.num(f.key, f.num); else ev.str(f.key, f.text);
    }
    ev.str("key", d.key).str("model", d.model).str("id", d.id).str("channel", d.channel).str("kind", d.kind);
    ev.str("name", userName.empty() ? (d.id.empty() ? d.model : d.model + " " + d.id) : userName).flag("named", !userName.empty());
    ev.num("rssi_db", d.rssiDb).num("snr_db", d.snrDb).flag("battery_low", d.batteryOk == 0);
    return ev;
}

automation::Rule sensorAlertRule(const std::string& key, const std::string& label, const std::string& field, bool above, double value, double cooldownSec) {
    automation::Rule r;
    char v[48];
    std::snprintf(v, sizeof v, "%.6g", value);
    r.name = label + ": " + field + (above ? " above " : " below ") + v;
    r.id = automation::makeId(r.name);
    r.note = "Made in Sensors, Alert. Fires while the value holds, at most once per cooldown.";
    r.enabled = true;
    r.trigger = "sensor.reading";
    r.conds = {{"key", "==", key}, {field, above ? ">" : "<", v}};
    r.cooldownKey = "key";
    r.cooldownSec = cooldownSec;
    automation::ActionSpec n;
    n.type = "notify";
    n.p = {{"title", "{{name}}: " + field + " {{" + field + "}}"}, {"body", std::string(above ? "Above " : "Below ") + v}};
    automation::ActionSpec snd;
    snd.type = "sound";
    r.actions = {n, snd};
    return r;
}

}  // namespace dect2
