#include "dect2/integrations/integrations.h"
#include "dect2/automation/json_mini.h"
#include "dect2/common/thread_name.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

namespace dect2::integrations {
namespace au = dect2::automation;
namespace js = dect2::automation::json;

au::FamilyMask defaultFamilies() {
    return au::kAllFamilies & ~(au::familyBit(au::FTime) | au::familyBit(au::FReceiver) | au::familyBit(au::FSatellite) | au::familyBit(au::FVoice) |
                                au::familyBit(au::FSession) | au::familyBit(au::FPlugin));
}

automation::FamilyMask archiveFamilies() { return defaultFamilies(); }
automation::FamilyMask Settings::hookFamilies() const {
    return (anySink() ? families : au::FamilyMask(0)) | (archive.enabled && !archive.folder.empty() ? archiveFamilies() : au::FamilyMask(0));
}

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// one topic level made safe
std::string level(const std::string& v) {
    std::string o;
    for (unsigned char c : v) {
        if (c == '/' || c == '+' || c == '#' || c < 0x20 || c == 0x7f) o += '_';
        else if (c == ' ') o += '_';
        else o += (char)c;
        if (o.size() >= 120) break;
    }
    return o.empty() ? "_" : o;
}

std::string familyPart(const au::Event& ev) { return ev.type.substr(0, ev.type.find('.')); }
std::string subPart(const au::Event& ev) {
    const size_t d = ev.type.find('.');
    return d == std::string::npos ? ev.type : ev.type.substr(d + 1);
}

std::string numText(double v) {
    char b[48];
    snprintf(b, sizeof b, "%.6f", v);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? "0" : s;
}

js::Value toJsonValue(const au::Value& v) {
    switch (v.t) {
    case au::Value::Num: return std::isfinite(v.n) ? js::Value::number(v.n) : js::Value();
    case au::Value::Str: return js::Value::string(v.s);
    case au::Value::Bool: return js::Value::boolean(v.b);
    default: return js::Value();
    }
}
}  // namespace

std::string checkTopicTemplate(const std::string& tpl) {
    if (trim(tpl).empty()) return "the topic is empty";
    if (tpl.size() > 300) return "the topic is too long";
    if (tpl[0] == '/') return "the topic must not start with /";
    // wildcards and control characters are never allowed in the fixed parts
    for (unsigned char c : tpl) if (c == '+' || c == '#' || c < 0x20) return "the topic has a character a topic cannot have (+ # or a control character)";
    // braces must be balanced and name something we know
    for (size_t i = 0; i < tpl.size(); i++) {
        if (tpl[i] != '{') { if (tpl[i] == '}') return "a } without a {"; continue; }
        const size_t e = tpl.find('}', i);
        if (e == std::string::npos) return "a { without a }";
        const std::string name = tpl.substr(i + 1, e - i - 1);
        static const char* known[] = {"family", "type", "sub", "mode", "ident", "freq_mhz"};
        bool ok = name.compare(0, 6, "field:") == 0 && name.size() > 6;
        for (const char* k : known) if (name == k) ok = true;
        if (!ok) return "unknown placeholder {" + name + "}";
        i = e;
    }
    return {};
}

std::string eventIdent(const au::Event& ev) {
    static const char* keys[] = {"source", "icao", "hex", "mmsi", "callsign", "call", "id", "serial", "address", "src", "mac", "name", "flight", "ric", "tail", "registration", "station"};
    for (const char* k : keys) {
        auto it = ev.f.find(k);
        if (it != ev.f.end() && !it->second.null()) {
            const std::string t = it->second.text();
            if (!t.empty()) return t;
        }
    }
    return {};
}

std::string expandTopic(const std::string& tpl, const au::Event& ev) {
    std::string out;
    for (size_t i = 0; i < tpl.size(); i++) {
        if (tpl[i] != '{') { out += tpl[i]; continue; }
        const size_t e = tpl.find('}', i);
        if (e == std::string::npos) { out += tpl.substr(i); break; }
        const std::string name = tpl.substr(i + 1, e - i - 1);
        std::string v;
        if (name == "family") v = familyPart(ev);
        else if (name == "type") v = ev.type;
        else if (name == "sub") v = subPart(ev);
        else if (name == "mode") v = ev.mode;
        else if (name == "ident") v = eventIdent(ev);
        else if (name == "freq_mhz") v = ev.freqHz > 0 ? numText(ev.freqHz / 1e6) : std::string();
        else if (name.compare(0, 6, "field:") == 0) { auto it = ev.f.find(name.substr(6)); if (it != ev.f.end()) v = it->second.text(); }
        // a dot in {type} stays a dot; slashes and wildcards never get through
        out += level(v);
        i = e;
    }
    return out;
}

std::string eventJson(const au::Event& ev) {
    js::Value o = js::Value::object();
    o.set("source", js::Value::string("OnAir"));
    o.set("type", js::Value::string(ev.type));
    o.set("mode", js::Value::string(ev.mode));
    o.set("freq_hz", js::Value::number(ev.freqHz));
    o.set("time", js::Value::number(ev.time));
    o.set("ident", js::Value::string(eventIdent(ev)));
    js::Value f = js::Value::object();
    for (const auto& kv : ev.f) { js::Value v = toJsonValue(kv.second); if (v.type != js::Value::Null) f.set(kv.first, v); }
    o.set("fields", f);
    return js::dump(o);
}

std::string httpUrlProblem(const std::string& u0) {
    const std::string u = trim(u0);
    if (u.size() < 12 || u.size() > 500) return "the address is not a web address";
    if (u.compare(0, 8, "https://") != 0 && u.compare(0, 7, "http://") != 0) return "the address must start with http:// or https://";
    for (unsigned char c : u) if (c <= 0x20 || c == 0x7f) return "the address has spaces or odd characters";
    return {};
}

std::vector<HaSensor> haSensors(const MqttSettings& m) {
    struct Def { const char* key; const char* name; const char* family; const char* icon; };
    static const Def defs[] = {
        {"adsb", "OnAir last aircraft", "adsb", "mdi:airplane"},
        {"ais", "OnAir last vessel", "ais", "mdi:ferry"},
        {"aprs", "OnAir last APRS station", "aprs", "mdi:radio-tower"},
        {"pager", "OnAir last pager message", "pager", "mdi:message-text"},
        {"sonde", "OnAir last radiosonde", "sonde", "mdi:balloon"},
    };
    std::vector<HaSensor> v;
    for (const Def& d : defs) {
        HaSensor s;
        s.key = d.key; s.name = d.name; s.family = d.family; s.icon = d.icon;
        s.stateTopic = m.haBase + "/ha/" + d.key;
        s.configTopic = m.haPrefix + "/sensor/onair_" + d.key + "/config";
        js::Value c = js::Value::object();
        c.set("name", js::Value::string(d.name));
        c.set("unique_id", js::Value::string(std::string("onair_") + d.key));
        c.set("state_topic", js::Value::string(s.stateTopic));
        c.set("value_template", js::Value::string("{{ value_json.ident if value_json.ident else value_json.type }}"));
        c.set("json_attributes_topic", js::Value::string(s.stateTopic));
        c.set("availability_topic", js::Value::string(m.haBase + "/status"));
        c.set("icon", js::Value::string(d.icon));
        js::Value dev = js::Value::object();
        js::Value ids = js::Value::array();
        ids.push(js::Value::string("onair"));
        dev.set("identifiers", ids);
        dev.set("name", js::Value::string("OnAir"));
        dev.set("manufacturer", js::Value::string("OnAir"));
        c.set("device", dev);
        s.configJson = js::dump(c);
        v.push_back(s);
    }
    return v;
}

// ---- settings file ----
std::string toJson(const Settings& s) {
    js::Value root = js::Value::object();
    root.set("version", js::Value::number(1));
    root.set("families", js::Value::number((double)(s.families & ((au::FamilyMask(1) << 52) - 1))));   // doubles hold 53 bits exactly
    root.set("max_per_second", js::Value::number(s.maxPerSecond));
    js::Value m = js::Value::object();
    const MqttSettings& q = s.mqtt;
    m.set("enabled", js::Value::boolean(q.enabled));
    m.set("host", js::Value::string(q.host));
    m.set("port", js::Value::number(q.port));
    m.set("tls", js::Value::boolean(q.tls));
    m.set("user", js::Value::string(q.user));
    m.set("password", js::Value::string(q.password));
    m.set("client_id", js::Value::string(q.clientId));
    m.set("topic", js::Value::string(q.topicTemplate));
    m.set("retain", js::Value::boolean(q.retain));
    m.set("qos", js::Value::number(q.qos));
    m.set("ha", js::Value::boolean(q.haDiscovery));
    m.set("ha_prefix", js::Value::string(q.haPrefix));
    m.set("ha_base", js::Value::string(q.haBase));
    root.set("mqtt", m);
    js::Value w = js::Value::object();
    w.set("enabled", js::Value::boolean(s.webhook.enabled));
    w.set("url", js::Value::string(s.webhook.url));
    w.set("token", js::Value::string(s.webhook.token));
    root.set("webhook", w);
    js::Value j = js::Value::object();
    j.set("enabled", js::Value::boolean(s.jsonl.enabled));
    j.set("path", js::Value::string(s.jsonl.path));
    j.set("max_mb", js::Value::number(s.jsonl.maxMegabytes));
    root.set("jsonl", j);
    js::Value a = js::Value::object();
    a.set("enabled", js::Value::boolean(s.archive.enabled));
    a.set("folder", js::Value::string(s.archive.folder));
    a.set("format", js::Value::number((int)s.archive.format));
    root.set("archive", a);
    return js::dump(root, true);
}

bool fromJson(const std::string& text, Settings& s, std::string& err) {
    js::Value root;
    if (!js::parse(text, root, err)) return false;
    if (root.type != js::Value::Obj) { err = "not an integrations file"; return false; }
    Settings n;
    n.families = root.get("families") ? (au::FamilyMask)root.num("families", 0) & au::kAllFamilies : defaultFamilies();
    n.maxPerSecond = (int)std::max(0.0, std::min(100000.0, root.num("max_per_second", 100)));
    if (const js::Value* m = root.get("mqtt")) {
        MqttSettings& q = n.mqtt;
        q.enabled = m->flag("enabled");
        q.host = trim(m->str("host"));
        q.port = (int)std::max(1.0, std::min(65535.0, m->num("port", 1883)));
        q.tls = m->flag("tls");
        q.user = m->str("user");
        q.password = m->str("password");
        q.clientId = m->str("client_id");
        q.topicTemplate = m->str("topic", q.topicTemplate);
        q.retain = m->flag("retain");
        q.qos = m->num("qos", 0) >= 1 ? 1 : 0;
        q.haDiscovery = m->flag("ha");
        q.haPrefix = m->str("ha_prefix", "homeassistant");
        q.haBase = m->str("ha_base", "onair");
    }
    if (const js::Value* w = root.get("webhook")) { n.webhook.enabled = w->flag("enabled"); n.webhook.url = trim(w->str("url")); n.webhook.token = w->str("token"); }
    if (const js::Value* j = root.get("jsonl")) { n.jsonl.enabled = j->flag("enabled"); n.jsonl.path = j->str("path"); n.jsonl.maxMegabytes = (int)std::max(0.0, std::min(100000.0, j->num("max_mb", 100))); }
    if (const js::Value* a = root.get("archive")) {
        n.archive.enabled = a->flag("enabled"); n.archive.folder = a->str("folder");
        const int f = (int)a->num("format", 0);
        n.archive.format = f == 1 ? ArchiveFormat::Json : f == 2 ? ArchiveFormat::Csv : ArchiveFormat::JsonLines;
    }
    s = n;
    return true;
}

// ---- the hub ----
struct Hub::Impl {
    mutable std::mutex mu;                 // settings, stats
    Settings set;
    Stats st;
    PostFn post;
    std::function<double()> clock = [] { return au::wallNow(); };
    std::atomic<bool> active{false};
    std::atomic<au::FamilyMask> mask{0};
    // rate limit (decoder thread; qmu)
    double winStart = 0;
    int winCount = 0;
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<au::Event> q;
    bool busy = false, stopReq = false, running = false, testReq = false, cfgChanged = false;
    std::thread th;
    // worker-only state
    mqtt::Client client;
    bool haSent = false;
    double nextTry = 0;
    double backoff = 2;
    std::string clientIdUsed, cfgKey;

    static constexpr size_t kQueueMax = 2000;

    void refresh() {   // mu held
        active.store(set.anyEnabled());
        mask.store(set.hookFamilies());
    }

    std::string clientIdFor(const MqttSettings& m) {
        if (!m.clientId.empty()) return m.clientId;
        if (clientIdUsed.empty()) {
            std::random_device rd;
            char b[24];
            snprintf(b, sizeof b, "onair-%06x", (unsigned)(rd() & 0xffffff));
            clientIdUsed = b;
        }
        return clientIdUsed;
    }

    void setMqttStatus(const std::string& s) { std::lock_guard<std::mutex> lk(mu); st.mqttStatus = s; }

    // connects when needed; false with the status set when it is not (yet) possible
    bool ensureMqtt(const Settings& s) {
        const MqttSettings& m = s.mqtt;
        if (client.connected()) return true;
        const double now = clock();
        if (now < nextTry) return false;
        if (!checkTopicTemplate(m.topicTemplate).empty()) { setMqttStatus("the topic template is not valid: " + checkTopicTemplate(m.topicTemplate)); nextTry = now + 30; return false; }
        mqtt::ClientConfig c;
        c.host = m.host; c.port = m.port; c.tls = m.tls;
        c.connect.clientId = clientIdFor(m);
        c.connect.user = m.user; c.connect.password = m.password;
        c.connect.keepAlive = 60;
        if (m.haDiscovery) { c.connect.willTopic = m.haBase + "/status"; c.connect.willPayload = "offline"; c.connect.willRetain = true; }
        client.configure(c);
        std::string err;
        if (!client.connect(err)) {
            nextTry = now + backoff;
            backoff = std::min(60.0, backoff * 2);
            setMqttStatus("waiting to retry: " + err);
            return false;
        }
        backoff = 2;
        setMqttStatus("connected");
        if (m.haDiscovery) {
            std::string e;
            bool ok = client.publish(m.haBase + "/status", "online", 1, true, e);
            for (const HaSensor& hs : haSensors(m)) ok = ok && client.publish(hs.configTopic, hs.configJson, 1, true, e);
            if (!ok) { setMqttStatus("waiting to retry: " + e); nextTry = now + backoff; return false; }
        }
        return true;
    }

    void mqttSend(const Settings& s, const au::Event& ev, const std::string& json) {
        const MqttSettings& m = s.mqtt;
        std::vector<std::pair<std::string, bool>> topics;   // topic, retain
        topics.push_back({expandTopic(m.topicTemplate, ev), m.retain});
        if (m.haDiscovery) {
            const std::string fam = familyPart(ev);
            for (const HaSensor& hs : haSensors(m)) if (hs.family == fam) topics.push_back({hs.stateTopic, m.retain});
        }
        for (int attempt = 0; attempt < 2; attempt++) {
            if (!ensureMqtt(s)) { std::lock_guard<std::mutex> lk(mu); st.mqttFailed++; return; }
            bool ok = true;
            std::string err;
            for (const auto& t : topics) {
                if (!mqtt::topicNameValid(t.first)) { ok = true; continue; }
                if (!client.publish(t.first, json, m.qos, t.second, err)) { ok = false; break; }
                std::lock_guard<std::mutex> lk(mu);
                st.mqttSent++;
            }
            if (ok) return;
            setMqttStatus("waiting to retry: " + err);
            nextTry = 0;   // one retry at once after a dropped connection (a failed connect backs off as usual)
            backoff = 2;
        }
        std::lock_guard<std::mutex> lk(mu);
        st.mqttFailed++;
    }

    void jsonlWrite(const JsonlSettings& j, const std::string& line) {
        std::error_code ec;
        if (j.maxMegabytes > 0) {
            std::ifstream probe(j.path, std::ios::binary | std::ios::ate);
            if (probe && (long long)probe.tellg() > (long long)j.maxMegabytes * 1024 * 1024) {
                probe.close();
                std::remove((j.path + ".1").c_str());
                std::rename(j.path.c_str(), (j.path + ".1").c_str());
            }
        }
        std::ofstream o(j.path, std::ios::binary | std::ios::app);
        std::lock_guard<std::mutex> lk(mu);
        if (!o) { st.jsonlStatus = "cannot write the file"; return; }
        o << line << '\n';
        st.jsonlLines++;
        st.jsonlStatus = "writing";
    }

    void webhookSend(const WebhookSettings& w, const std::string& json) {
        PostFn p;
        { std::lock_guard<std::mutex> lk(mu); p = post; }
        const std::string bad = httpUrlProblem(w.url);
        if (!bad.empty() || !p) { std::lock_guard<std::mutex> lk(mu); st.webhookFailed++; st.webhookStatus = bad.empty() ? "no web client" : bad; return; }
        std::vector<std::string> hd;
        if (!w.token.empty()) {
            bool ok = true;
            for (unsigned char c : w.token) if (c <= 0x20 || c == 0x7f) ok = false;
            if (!ok) { std::lock_guard<std::mutex> lk(mu); st.webhookFailed++; st.webhookStatus = "the token has odd characters"; return; }
            hd.push_back("Authorization: Bearer " + w.token);
        }
        const bool ok = p(trim(w.url), json, hd);
        std::lock_guard<std::mutex> lk(mu);
        if (ok) { st.webhookSent++; st.webhookStatus = "sent"; } else { st.webhookFailed++; st.webhookStatus = "failed"; }
    }

    void archiveSave(const ArchiveSettings& a, const au::Event& ev) {
        const std::string err = archiveWrite(a, ev);
        std::lock_guard<std::mutex> lk(mu);
        if (err.empty()) { st.archiveRecords++; st.archiveStatus = "saving to " + a.folder; }
        else { st.archiveFailed++; st.archiveStatus = err; }
    }

    void deliver(const Settings& s, const au::Event& ev, bool isTest = false) {
        const int fam = au::familyOf(ev.type);
        const bool sinkFam = isTest || (fam >= 0 && (s.families >> fam & 1));
        if (sinkFam && s.anySink()) {
            const std::string json = eventJson(ev);
            if (s.mqtt.enabled && !s.mqtt.host.empty()) mqttSend(s, ev, json);
            if (s.jsonl.enabled && !s.jsonl.path.empty()) jsonlWrite(s.jsonl, json);
            if (s.webhook.enabled && !s.webhook.url.empty()) webhookSend(s.webhook, json);
        }
        if (!isTest && s.archive.enabled && !s.archive.folder.empty() && fam >= 0 && (archiveFamilies() >> fam & 1)) archiveSave(s.archive, ev);
    }

    void run() {
        for (;;) {
            au::Event ev;
            bool have = false, test = false;
            Settings s;
            {
                std::unique_lock<std::mutex> lk(qmu);
                busy = false;
                qcv.notify_all();
                qcv.wait_for(lk, std::chrono::seconds(20), [&] { return stopReq || !q.empty() || testReq || cfgChanged; });
                if (stopReq) break;
                if (!q.empty()) { ev = std::move(q.front()); q.pop_front(); have = true; }
                test = testReq; testReq = false;
                cfgChanged = false;
                busy = true;
            }
            { std::lock_guard<std::mutex> lk(mu); s = set; }
            {   // another broker, login or discovery setting: drop the old connection and try at once
                const MqttSettings& m = s.mqtt;
                const std::string key = m.host + "|" + std::to_string(m.port) + "|" + (m.tls ? "t" : "p") + "|" + m.user + "|" + m.password + "|" + m.clientId + "|" + (m.haDiscovery ? m.haBase : "-");
                if (key != cfgKey) { cfgKey = key; if (client.connected()) client.disconnect(); nextTry = 0; backoff = 2; }
            }
            if (!s.mqtt.enabled || s.mqtt.host.empty()) {
                if (client.connected()) client.disconnect();
                haSent = false;
                std::lock_guard<std::mutex> lk(mu);
                st.mqttStatus = "off";
            } else if (!have && !test && client.connected() && client.idleSeconds() > 30) {
                std::string e;
                if (!client.ping(e)) { setMqttStatus("waiting to retry: " + e); nextTry = clock() + backoff; }
            }
            if (!s.anyEnabled()) { std::lock_guard<std::mutex> lk(qmu); q.clear(); continue; }
            if (test) {
                au::Event t("signal.test");
                t.at(clock(), "OnAir", 0).str("text", "Test message from OnAir Integrations");
                deliver(s, t, true);
            }
            if (have) deliver(s, ev);
        }
        if (client.connected()) client.disconnect();
    }
};

Hub::Hub() : p_(new Impl) {}
Hub::~Hub() { stop(); }

void Hub::setSettings(const Settings& s) {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->set = s;
        p_->refresh();
    }
    { std::lock_guard<std::mutex> lk(p_->qmu); p_->cfgChanged = true; if (!s.anyEnabled()) p_->q.clear(); }
    p_->qcv.notify_all();
}

Settings Hub::settings() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->set; }
void Hub::setPost(PostFn f) { std::lock_guard<std::mutex> lk(p_->mu); p_->post = std::move(f); }
void Hub::setClock(std::function<double()> now) { p_->clock = std::move(now); }

void Hub::submit(const au::Event& ev) {
    if (!p_->active.load(std::memory_order_relaxed)) return;
    const int fam = au::familyOf(ev.type);
    if (fam < 0 || !(p_->mask.load(std::memory_order_relaxed) >> fam & 1)) return;
    int maxPerSec;
    bool onlyArchive;   // saved messages are never rate-limited: the limit protects a broker, not a file
    { std::lock_guard<std::mutex> lk(p_->mu); maxPerSec = p_->set.maxPerSecond; onlyArchive = !(p_->set.anySink() && (p_->set.families >> fam & 1)); }
    if (onlyArchive) maxPerSec = 0;
    {
        std::lock_guard<std::mutex> lk(p_->qmu);
        if (maxPerSec > 0) {
            const double now = p_->clock();
            if (now - p_->winStart >= 1.0 || now < p_->winStart) { p_->winStart = now; p_->winCount = 0; }
            if (++p_->winCount > maxPerSec) { std::lock_guard<std::mutex> l2(p_->mu); p_->st.rateDropped++; return; }
        }
        if (p_->q.size() >= Impl::kQueueMax) { p_->q.pop_front(); std::lock_guard<std::mutex> l2(p_->mu); p_->st.dropped++; }
        p_->q.push_back(ev);
    }
    p_->qcv.notify_one();
}

void Hub::start() {
    std::lock_guard<std::mutex> lk(p_->qmu);
    if (p_->running) return;
    p_->running = true;
    p_->stopReq = false;
    p_->th = dect2::namedThread("onair-integr", [this] { p_->run(); });
}

void Hub::stop() {
    {
        std::lock_guard<std::mutex> lk(p_->qmu);
        if (!p_->running) return;
        p_->stopReq = true;
    }
    p_->qcv.notify_all();
    if (p_->th.joinable()) p_->th.join();
    std::lock_guard<std::mutex> lk(p_->qmu);
    p_->running = false;
}

Stats Hub::stats() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->st; }

void Hub::sendTest() {
    { std::lock_guard<std::mutex> lk(p_->qmu); p_->testReq = true; }
    p_->qcv.notify_all();
}

bool Hub::flush(int timeoutMs) {
    std::unique_lock<std::mutex> lk(p_->qmu);
    return p_->qcv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return p_->q.empty() && !p_->busy && !p_->testReq; });
}

Hub& global() {
    static Hub h;
    return h;
}

}  // namespace dect2::integrations
