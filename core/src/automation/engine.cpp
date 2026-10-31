#include "dect2/common/thread_name.h"
#include "dect2/automation/engine.h"
#include "dect2/automation/expr.h"
#include "dect2/automation/script.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace dect2::automation {

namespace fs = std::filesystem;

namespace {
constexpr size_t kQueueMax = 2048, kFiringsMax = 500, kKeyMax = 4000;
constexpr double kHour = 3600;

double systemNow() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}
fs::path u8path(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }

std::string brief(const Event& ev) {
    std::string s = ev.type;
    int n = 0;
    for (const auto& kv : ev.f) {
        if (kv.second.null()) continue;
        std::string v = kv.second.text();
        if (v.size() > 40) v.resize(40);
        s += (n ? ", " : " ") + kv.first + "=" + v;
        if (++n == 3) break;
    }
    if (s.size() > 120) s.resize(120);
    return s;
}

bool numField(const Event& ev, const char* k, double& v) {
    auto it = ev.f.find(k);
    if (it == ev.f.end() || it->second.t != Value::Num) return false;
    v = it->second.n;
    return true;
}

std::string describe(const std::string& kind, const std::map<std::string, std::string>& p) {
    std::string s = kind + ":";
    for (const auto& kv : p) {
        std::string v = kv.second;
        if (v.size() > 160) v = v.substr(0, 160) + "...";
        for (char& c : v) if (c == '\n' || c == '\r') c = ' ';
        s += " " + kv.first + "=" + v;
    }
    return s;
}
}  // namespace

int familyOf(const std::string& type) {
    const size_t d = type.find('.');
    const std::string f = type.substr(0, d);
    static const char* const names[kFamilies] = {"adsb", "acars", "vdl2", "hfdl", "pager", "ais", "aprs", "sonde", "gnss", "lora", "ble", "rid", "dsc", "signal", "receiver", "satellite", "time", "plugin", "voice", "session"};
    for (int i = 0; i < kFamilies; i++) if (f == names[i]) return i;
    return -1;
}

FamilyMask maskOfTrigger(const std::string& trigger) {
    if (trigger == "*") return kAllFamilies;
    return familyBit(familyOf(trigger));
}

struct Engine::Impl {
    Engine* self = nullptr;
    // settings and rules (mu)
    mutable std::mutex mu;
    std::shared_ptr<const std::vector<Rule>> rules = std::make_shared<std::vector<Rule>>();
    struct RuleRt {
        std::string whenSrc;
        std::shared_ptr<const Expr> expr;
        std::map<std::string, double> lastFire;
        std::deque<double> hourFires;
        std::map<std::string, bool> geoState;
        RuleStats st;
    };
    std::map<std::string, RuleRt> rt;
    struct ScriptRt;
    ActionSink* sink = nullptr;
    bool stationSet = false;
    double stLat = 0, stLon = 0;
    std::string logDir;
    bool allowRun = false;
    std::set<std::string> confirmed;
    std::function<double()> clock = systemNow;
    int tz = 0;
    int maxPerMin = 60;
    std::deque<double> actionTimes;
    std::deque<Firing> firings;
    std::function<void(const Firing&)> hook;
    // scripts (procMu for the list, mu for stats)
    std::mutex procMu;
    std::vector<std::shared_ptr<ScriptRt>> scripts;
    std::map<std::string, RuleStats> scriptStats;
    std::map<std::string, std::string> scriptErr;
    // queue and thread
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<Event> q;
    bool stopReq = false;
    std::thread th;
    // timed jobs (procMu)
    struct SondeSeen { double last = 0, lat = 0, lon = 0, alt = 0, maxAlt = 0, vs = 0; std::string type; bool sent = false; };
    std::map<std::string, SondeSeen> sondes;
    long long lastMinute = -1;

    // ---- helpers ----
    Env envFor(const Event& ev, double now) {
        Env e;
        e.ev = &ev; e.now = now; e.tzOffsetSec = tz;
        e.stationSet = stationSet; e.stationLat = stLat; e.stationLon = stLon;
        return e;
    }
    int localHour(double now) const {
        const long long t = (long long)std::floor(now) + tz;
        const long long day = (long long)std::floor((double)t / 86400.0);
        return (int)((t - day * 86400) / 3600);
    }
    bool allowAction(double now) {   // the cap on all actions together; mu held
        while (!actionTimes.empty() && now - actionTimes.front() > 60) actionTimes.pop_front();
        if (maxPerMin > 0 && (int)actionTimes.size() >= maxPerMin) return false;
        actionTimes.push_back(now);
        return true;
    }
    void addFiring(Firing f) {
        std::function<void(const Firing&)> h;
        {
            std::lock_guard<std::mutex> lk(mu);
            firings.push_back(f);
            while (firings.size() > kFiringsMax) firings.pop_front();
            h = hook;
        }
        if (h) h(f);
    }
    bool doLog(const std::map<std::string, std::string>& raw, const Event& ev, const std::string& ruleName, bool execute, std::string& desc);
    void runActions(const Rule& r, const Event& ev, bool execute, std::vector<std::string>& out, bool& ok);
    void processLocked(const Event& ev);
    void tickLocked(double now);
    void runScripts(const Event& ev, double now);
    void setMask();
};

// ---- script runtime ----
struct Engine::Impl::ScriptRt final : ScriptApi {
    Impl* e;
    Script def;
    std::unique_ptr<ScriptHost> host;
    std::vector<std::string> acts;
    int count = 0;
    double nowOverride = 0;
    ScriptRt(Impl* i, const Script& s) : e(i), def(s) {}
    bool act(const std::string& kind, const std::map<std::string, std::string>& p, std::string& detail) override {
        if (kind == "run") { detail = "scripts cannot run programs"; return false; }
        if (++count > 20) { detail = "too many actions in one call (20 at most)"; return false; }
        const double now = e->clock();
        {
            std::lock_guard<std::mutex> lk(e->mu);
            if (!e->allowAction(now)) { detail = "throttled: too many actions per minute"; acts.push_back(kind + ": throttled"); return false; }
        }
        if (kind == "webhook") {
            const std::string url = p.count("url") ? p.at("url") : "";
            if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) { detail = "the webhook address must start with http:// or https://"; return false; }
        }
        if (kind == "log") {   // log_row(file, ...): the engine writes it
            std::string d;
            Event none;
            std::map<std::string, std::string> raw = p;
            const bool ok = e->doLog(raw, none, def.name, true, d);
            acts.push_back(d);
            detail = d;
            return ok;
        }
        std::string d;
        ActionSink* s;
        { std::lock_guard<std::mutex> lk(e->mu); s = e->sink; }
        bool ok = false;
        if (!s) d = "no action sink";
        else ok = s->perform(kind, p, d);
        acts.push_back(describe(kind, p) + (ok ? "  -> ok" : "  -> failed: " + d));
        detail = d;
        return ok;
    }
    void note(const std::string& t) override { if (acts.size() < 40) acts.push_back("log: " + t.substr(0, 300)); }
    bool station(double& lat, double& lon) override { std::lock_guard<std::mutex> lk(e->mu); lat = e->stLat; lon = e->stLon; return e->stationSet; }
    double now() override { return e->clock(); }
    int tzOffsetSec() override { std::lock_guard<std::mutex> lk(e->mu); return e->tz; }
};

// ---- log action ----
bool Engine::Impl::doLog(const std::map<std::string, std::string>& raw, const Event& ev, const std::string& ruleName, bool execute, std::string& desc) {
    auto get = [&](const char* k) { auto it = raw.find(k); return it == raw.end() ? std::string() : it->second; };
    std::string name = get("file");
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".csv") == 0) name.resize(name.size() - 4);
    bool good = !name.empty() && name.size() <= 64 && name[0] != '.';
    for (unsigned char c : name) if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') good = false;
    if (name.find("..") != std::string::npos) good = false;
    if (!good) { desc = "log: bad file name (letters, digits, - _ . only)"; return false; }
    std::vector<std::string> cols;
    const std::string rowT = get("row");
    std::string line;
    const bool fromScript = ev.type.empty();
    if (fromScript) {   // log_row(): the columns are the values given, '\n' apart
        size_t i = 0;
        const std::string cells = get("cells");
        while (i <= cells.size()) { size_t e2 = cells.find('\n', i); if (e2 == std::string::npos) e2 = cells.size(); cols.push_back(cells.substr(i, e2 - i)); i = e2 + 1; }
        for (size_t k = 0; k < cols.size() && k < 30; k++) line += (k ? "," : "") + csvField(cols[k]);
    } else if (!rowT.empty()) {
        size_t i = 0;
        int n = 0;
        while (i <= rowT.size() && n < 30) {
            size_t e2 = rowT.find(',', i);
            if (e2 == std::string::npos) e2 = rowT.size();
            line += (n ? "," : "") + csvField(renderTemplate(rowT.substr(i, e2 - i), ev, ruleName));
            i = e2 + 1; n++;
        }
    } else {
        std::string fields;
        for (const auto& kv : ev.f) fields += (fields.empty() ? "" : "; ") + kv.first + "=" + kv.second.text();
        line = csvField(renderTemplate("{{time_iso}}", ev, ruleName)) + "," + csvField(ruleName) + "," + csvField(ev.type) + "," + csvField(ev.mode) + "," + csvField(renderTemplate("{{freq_mhz}}", ev, ruleName)) + "," + csvField(fields);
    }
    desc = "log: " + name + ".csv <- " + line.substr(0, 160);
    if (!execute) return true;
    std::string dir;
    { std::lock_guard<std::mutex> lk(mu); dir = logDir; }
    if (dir.empty()) { desc = "log: no folder set"; return false; }
    std::error_code ec;
    fs::create_directories(u8path(dir), ec);
    const fs::path p = u8path(dir) / u8path(name + ".csv");
    const bool fresh = !fs::exists(p, ec);
    if (!fresh && fs::file_size(p, ec) > (5u << 20)) { desc = "log: " + name + ".csv is full (5 MB)"; return false; }
    std::ofstream f(p, std::ios::app | std::ios::binary);
    if (!f) { desc = "log: cannot write " + name + ".csv"; return false; }
    if (fresh && !fromScript && rowT.empty()) f << "time,rule,type,mode,freq_mhz,fields\n";
    f << line << "\n";
    return true;
}

void Engine::Impl::runActions(const Rule& r, const Event& ev, bool execute, std::vector<std::string>& out, bool& allOk) {
    allOk = true;
    ActionSink* s;
    bool runOk;
    {
        std::lock_guard<std::mutex> lk(mu);
        s = sink;
        runOk = allowRun && confirmed.count(runFingerprint(r));
    }
    for (const ActionSpec& a : r.actions) {
        std::map<std::string, std::string> p;
        std::string desc;
        bool ok = true;
        if (a.type == "log") {
            ok = doLog(a.p, ev, r.name, execute, desc);
            out.push_back(desc + (execute ? (ok ? "  -> ok" : "  -> failed") : ""));
            allOk = allOk && ok;
            continue;
        }
        if (a.type == "webhook") {
            std::string url, body, err;
            if (!buildWebhook(a, ev, r.name, url, body, err)) { out.push_back("webhook: " + err); allOk = false; continue; }
            p["url"] = url; p["body"] = body;
        } else if (a.type == "run") {
            p["program"] = a.p.count("program") ? a.p.at("program") : "";   // the program is never filled in from the event
            const std::string rawArgs = a.p.count("args") ? a.p.at("args") : "";
            std::string args;
            size_t i = 0;
            while (i <= rawArgs.size()) {
                size_t e2 = rawArgs.find('\n', i);
                if (e2 == std::string::npos) e2 = rawArgs.size();
                std::string one = renderTemplate(rawArgs.substr(i, e2 - i), ev, r.name);
                for (char& c : one) if (c == '\n' || c == '\r') c = ' ';   // one value stays one argument
                args += (i ? "\n" : "") + one;
                i = e2 + 1;
            }
            p["args"] = args;
            if (execute && !runOk) { out.push_back("run: not done. Needs 'Allow running programs' in the Automations window and your confirmation of this rule"); allOk = false; continue; }
        } else {
            for (const auto& kv : a.p) p[kv.first] = renderTemplate(kv.second, ev, r.name);
        }
        if (!execute) { out.push_back(describe(a.type, p)); continue; }
        if (!s) { out.push_back(a.type + ": no action sink"); allOk = false; continue; }
        ok = s->perform(a.type, p, desc);
        out.push_back(describe(a.type, p) + (ok ? "  -> ok" : "  -> failed: " + desc));
        allOk = allOk && ok;
    }
}

void Engine::Impl::setMask() {
    FamilyMask m = 0;
    std::lock_guard<std::mutex> lk(mu);
    auto add = [&](const std::string& t) { m |= maskOfTrigger(t); };
    for (const Rule& r : *rules) if (r.enabled) add(r.trigger);
    for (const auto& s : scripts) if (s->def.enabled) { if (s->def.events.empty()) add("*"); for (const auto& t : s->def.events) add(t); }
    self->mask_.store(m);
}

Engine::Engine() : p_(new Impl) { p_->self = this; }
Engine::~Engine() { stop(); }

void Engine::setSink(ActionSink* s) { std::lock_guard<std::mutex> lk(p_->mu); p_->sink = s; }
void Engine::setStation(bool set, double lat, double lon) { std::lock_guard<std::mutex> lk(p_->mu); p_->stationSet = set; p_->stLat = lat; p_->stLon = lon; }
void Engine::setLogDir(const std::string& d) { std::lock_guard<std::mutex> lk(p_->mu); p_->logDir = d; }
void Engine::setAllowRun(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->allowRun = on; }
void Engine::setConfirmedRuns(const std::set<std::string>& f) { std::lock_guard<std::mutex> lk(p_->mu); p_->confirmed = f; }
void Engine::setClock(std::function<double()> c) { std::lock_guard<std::mutex> lk(p_->mu); p_->clock = c ? c : systemNow; }
void Engine::setTzOffset(int s) { std::lock_guard<std::mutex> lk(p_->mu); p_->tz = s; }
void Engine::setMaxActionsPerMinute(int n) { std::lock_guard<std::mutex> lk(p_->mu); p_->maxPerMin = n; }
void Engine::setFiringHook(std::function<void(const Firing&)> f) { std::lock_guard<std::mutex> lk(p_->mu); p_->hook = std::move(f); }

void Engine::setRules(const std::vector<Rule>& r) {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->rules = std::make_shared<std::vector<Rule>>(r);
        std::set<std::string> ids;
        for (const Rule& x : r) ids.insert(x.id);
        for (auto it = p_->rt.begin(); it != p_->rt.end();) it = ids.count(it->first) ? std::next(it) : p_->rt.erase(it);
    }
    p_->setMask();
}
std::vector<Rule> Engine::rules() const { std::lock_guard<std::mutex> lk(p_->mu); return *p_->rules; }

void Engine::setScripts(const std::vector<Script>& s) {
    std::lock_guard<std::mutex> pl(p_->procMu);
    std::vector<std::shared_ptr<Impl::ScriptRt>> next;
    for (const Script& x : s) {
        std::shared_ptr<Impl::ScriptRt> keep;
        for (auto& o : p_->scripts) if (o->def.id == x.id && o->def.code == x.code) { keep = o; break; }   // same code: keep its state
        if (!keep) {
            keep = std::make_shared<Impl::ScriptRt>(p_.get(), x);
            keep->host.reset(new ScriptHost(x, keep.get()));
            std::lock_guard<std::mutex> lk(p_->mu);
            p_->scriptErr[x.id] = keep->host->loaded() ? "" : keep->host->error();
        }
        keep->def = x;
        next.push_back(keep);
    }
    p_->scripts = std::move(next);
    p_->setMask();
}
std::vector<Script> Engine::scripts() const {
    std::lock_guard<std::mutex> pl(p_->procMu);
    std::vector<Script> o;
    for (auto& s : p_->scripts) o.push_back(s->def);
    return o;
}
RuleStats Engine::stats(const std::string& id) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->rt.find(id);
    if (it != p_->rt.end()) return it->second.st;
    auto s = p_->scriptStats.find(id);
    return s != p_->scriptStats.end() ? s->second : RuleStats();
}
std::string Engine::scriptError(const std::string& id) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->scriptErr.find(id);
    return it == p_->scriptErr.end() ? std::string() : it->second;
}
std::vector<Firing> Engine::firings() const { std::lock_guard<std::mutex> lk(p_->mu); return std::vector<Firing>(p_->firings.begin(), p_->firings.end()); }
void Engine::clearFirings() { std::lock_guard<std::mutex> lk(p_->mu); p_->firings.clear(); }

void Engine::setTap(std::function<void(const Event&)> f, FamilyMask familyMask) {
    std::lock_guard<std::mutex> lk(tapMu_);
    tap_ = std::move(f);
    tapMask_.store(tap_ ? familyMask : 0);
}

void Engine::setObserver(std::function<void(const Event&)> f, FamilyMask familyMask) {
    std::lock_guard<std::mutex> lk(obsMu_);
    obs_ = std::move(f);
    observeMask_.store(obs_ ? familyMask : 0);
}

void Engine::setWatcher(std::function<void(const Event&)> f, FamilyMask familyMask) {
    std::lock_guard<std::mutex> lk(watchMu_);
    watch_ = std::move(f);
    watchMask_.store(watch_ ? familyMask : 0);
}

void Engine::setStreamer(std::function<void(const Event&)> f, FamilyMask familyMask) {
    std::lock_guard<std::mutex> lk(streamMu_);
    stream_ = std::move(f);
    streamMask_.store(stream_ ? familyMask : 0);
}

void Engine::setPublisher(std::function<void(const Event&)> f, FamilyMask familyMask) {
    std::lock_guard<std::mutex> lk(publishMu_);
    publish_ = std::move(f);
    publishMask_.store(publish_ ? familyMask : 0);
}

void Engine::submit(Event&& ev) {
    const int fam = familyOf(ev.type);
    if (fam >= 0 && (tapMask_.load(std::memory_order_relaxed) >> fam & 1)) {
        std::function<void(const Event&)> t;
        { std::lock_guard<std::mutex> lk(tapMu_); t = tap_; }
        if (t) t(ev);
    }
    if (fam >= 0 && (observeMask_.load(std::memory_order_relaxed) >> fam & 1)) {
        std::function<void(const Event&)> t;
        { std::lock_guard<std::mutex> lk(obsMu_); t = obs_; }
        if (t) t(ev);
    }
    if (fam >= 0 && (watchMask_.load(std::memory_order_relaxed) >> fam & 1)) {
        std::function<void(const Event&)> t;
        { std::lock_guard<std::mutex> lk(watchMu_); t = watch_; }
        if (t) t(ev);
    }
    if (fam >= 0 && (streamMask_.load(std::memory_order_relaxed) >> fam & 1)) {
        std::function<void(const Event&)> t;
        { std::lock_guard<std::mutex> lk(streamMu_); t = stream_; }
        if (t) t(ev);
    }
    if (fam >= 0 && (publishMask_.load(std::memory_order_relaxed) >> fam & 1)) {
        std::function<void(const Event&)> t;
        { std::lock_guard<std::mutex> lk(publishMu_); t = publish_; }
        if (t) t(ev);
    }
    if (!(fam >= 0 && (mask_.load(std::memory_order_relaxed) >> fam & 1))) return;
    {
        std::lock_guard<std::mutex> lk(p_->qmu);
        if (p_->q.size() >= kQueueMax) { dropped_++; return; }
        p_->q.push_back(std::move(ev));
    }
    p_->qcv.notify_one();
}

void Engine::start() {
    if (p_->th.joinable()) return;
    p_->stopReq = false;
    p_->th = dect2::namedThread("onair-auto", [this] {
        for (;;) {
            std::deque<Event> batch;
            {
                std::unique_lock<std::mutex> lk(p_->qmu);
                p_->qcv.wait_for(lk, std::chrono::milliseconds(500), [&] { return p_->stopReq || !p_->q.empty(); });
                if (p_->stopReq) return;
                batch.swap(p_->q);
            }
            for (const Event& e : batch) process(e);
            double now;
            { std::lock_guard<std::mutex> lk(p_->mu); now = p_->clock(); }
            tick(now);
        }
    });
}
void Engine::stop() {
    {
        std::lock_guard<std::mutex> lk(p_->qmu);
        p_->stopReq = true;
    }
    p_->qcv.notify_all();
    if (p_->th.joinable()) p_->th.join();
}
void Engine::drain() {
    std::deque<Event> batch;
    { std::lock_guard<std::mutex> lk(p_->qmu); batch.swap(p_->q); }
    for (const Event& e : batch) process(e);
}

void Engine::process(const Event& ev) {
    std::lock_guard<std::mutex> pl(p_->procMu);
    p_->processLocked(ev);
}
void Engine::tick(double now) {
    std::lock_guard<std::mutex> pl(p_->procMu);
    p_->tickLocked(now);
}

void Engine::Impl::tickLocked(double now) {
    // time of day
    if (self->wants(FTime)) {
        const long long t = (long long)std::floor(now) + tz;
        const long long minute = t / 60 - (t < 0 ? 1 : 0);
        if (minute != lastMinute) {
            lastMinute = minute;
            const long long day = (long long)std::floor((double)t / 86400.0);
            const long long sod = t - day * 86400;
            Event e("time.minute");
            e.time = now;
            e.num("hour", (double)(sod / 3600)).num("minute", (double)((sod / 60) % 60)).num("weekday", (double)(((day % 7) + 11) % 7));
            processLocked(e);
        }
    }
    // a radiosonde that fell and went quiet has landed
    for (auto it = sondes.begin(); it != sondes.end();) {
        SondeSeen& s = it->second;
        if (!s.sent && now - s.last > 90 && s.vs < -1 && s.alt < 6000 && s.maxAlt > 3000) {
            s.sent = true;
            Event e("sonde.landed");
            e.at(now, "Sonde", 0);
            e.str("serial", it->first).str("type", s.type).num("lat", s.lat).num("lon", s.lon).num("alt_m", s.alt).num("max_alt_m", s.maxAlt);
            if (stationSet) e.num("distance_km", geo::distanceKm(stLat, stLon, s.lat, s.lon));
            processLocked(e);
        }
        if (now - s.last > 6 * kHour) it = sondes.erase(it); else ++it;
    }
}

void Engine::Impl::processLocked(const Event& ev0) {
    // an event with a position gets distance_km from the receiver, so conditions and {{distance_km}} in notifications work for every mode (issue #403)
    Event withDist;
    const Event* pe = &ev0;
    if (stationSet && ev0.f.find("distance_km") == ev0.f.end()) {
        double la, lo;
        if (numField(ev0, "lat", la) && numField(ev0, "lon", lo)) { withDist = ev0; withDist.num("distance_km", geo::distanceKm(stLat, stLon, la, lo)); pe = &withDist; }
    }
    const Event& ev = *pe;
    double now;
    std::shared_ptr<const std::vector<Rule>> rs;
    {
        std::lock_guard<std::mutex> lk(mu);
        now = clock();
        rs = rules;
    }
    if (ev.type == "sonde.update") {
        auto it = ev.f.find("serial");
        if (it != ev.f.end() && sondes.size() < 500) {
            SondeSeen& s = sondes[it->second.text()];
            s.last = now; s.sent = false;
            double v;
            if (numField(ev, "lat", v)) s.lat = v;
            if (numField(ev, "lon", v)) s.lon = v;
            if (numField(ev, "alt_m", v)) { s.alt = v; s.maxAlt = std::max(s.maxAlt, v); }
            if (numField(ev, "vspeed_ms", v)) s.vs = v;
            auto ty = ev.f.find("type");
            if (ty != ev.f.end()) s.type = ty->second.text();
        }
    }
    for (const Rule& r : *rs) {
        if (!r.enabled || !triggerMatches(r.trigger, ev.type)) continue;
        // ---- conditions, in the order of the editor: time window, expression, geofence ----
        if (r.windowFromHour >= 0) {
            const int h = localHour(now);
            const bool in = r.windowFromHour == r.windowToHour ? true : r.windowFromHour < r.windowToHour ? (h >= r.windowFromHour && h < r.windowToHour) : (h >= r.windowFromHour || h < r.windowToHour);
            if (!in) continue;
        }
        std::shared_ptr<const Expr> ex;
        Env env;
        {
            std::lock_guard<std::mutex> lk(mu);
            RuleRt& rr = rt[r.id];
            const std::string src = conditionExpr(r);
            if (!rr.expr || rr.whenSrc != src) {
                ExprError ee;
                rr.expr = Expr::parse(src, ee);
                rr.whenSrc = src;
                if (!rr.expr) { rr.st.errors++; rr.st.lastError = "condition: " + ee.text(); }
            }
            ex = rr.expr;
            env = envFor(ev, now);
        }
        if (!ex) continue;
        std::string err;
        if (!ex->test(env, &err)) {
            if (!err.empty()) { std::lock_guard<std::mutex> lk(mu); rt[r.id].st.errors++; rt[r.id].st.lastError = err; }
            continue;
        }
        const std::string key = r.cooldownKey.empty() ? std::string() : [&] { auto it = ev.f.find(r.cooldownKey); return it == ev.f.end() ? std::string() : it->second.text().substr(0, 200); }();
        if (r.geo != Rule::GeoOff) {
            double la, lo;
            if (!numField(ev, "lat", la) || !numField(ev, "lon", lo)) continue;
            bool inside;
            if (r.geo == Rule::GeoCircle) {
                double clat = r.geoLat, clon = r.geoLon;
                if (r.geoHome) { if (!stationSet) continue; clat = stLat; clon = stLon; }
                inside = geo::distanceKm(la, lo, clat, clon) <= r.geoRadiusKm;
            } else {
                std::vector<std::pair<double, double>> pts;
                if (!geo::parsePolygon(r.geoPolygon, pts)) continue;
                inside = geo::pointInPolygon(la, lo, pts);
            }
            const bool ok = inside == r.geoInside;
            if (r.geoEnterOnly) {
                std::lock_guard<std::mutex> lk(mu);
                RuleRt& rr = rt[r.id];
                if (rr.geoState.size() > kKeyMax) rr.geoState.clear();
                auto it = rr.geoState.find(key);
                const bool was = it != rr.geoState.end() && it->second;
                rr.geoState[key] = ok;
                if (!ok || was) continue;
            } else if (!ok) continue;
        }
        // ---- limits ----
        bool go = true;
        std::string why;
        {
            std::lock_guard<std::mutex> lk(mu);
            RuleRt& rr = rt[r.id];
            rr.st.matched++;
            auto lf = rr.lastFire.find(key);
            if (r.cooldownSec > 0 && lf != rr.lastFire.end() && now - lf->second < r.cooldownSec) { go = false; why = "cooldown"; }
            while (!rr.hourFires.empty() && now - rr.hourFires.front() > kHour) rr.hourFires.pop_front();
            if (go && r.maxPerHour > 0 && (int)rr.hourFires.size() >= r.maxPerHour) { go = false; why = "limit per hour"; }
            if (go && !allowAction(now)) { go = false; why = "throttled (too many actions per minute)"; }
            if (!go) { rr.st.suppressed++; continue; }
            if (rr.lastFire.size() > kKeyMax) rr.lastFire.clear();
            rr.lastFire[key] = now;
            rr.hourFires.push_back(now);
            rr.st.fired++;
            rr.st.lastFired = now;
        }
        Firing fi;
        fi.time = now; fi.rule = r.name; fi.eventType = ev.type; fi.summary = brief(ev);
        bool ok;
        runActions(r, ev, true, fi.actions, ok);
        fi.ok = ok;
        if (!ok) { fi.error = "an action failed"; std::lock_guard<std::mutex> lk(mu); rt[r.id].st.errors++; rt[r.id].st.lastError = "an action failed"; }
        addFiring(fi);
    }
    runScripts(ev, now);
}

void Engine::Impl::runScripts(const Event& ev, double now) {
    for (auto& sr : scripts) {
        if (!sr->def.enabled) continue;
        bool want = sr->def.events.empty();
        for (const auto& t : sr->def.events) if (triggerMatches(t, ev.type)) want = true;
        if (!want || !sr->host || !sr->host->loaded()) continue;
        sr->acts.clear(); sr->count = 0;
        const bool ok = sr->host->onEvent(ev);
        {
            std::lock_guard<std::mutex> lk(mu);
            RuleStats& st = scriptStats[sr->def.id];
            st.matched++;
            if (!sr->acts.empty()) { st.fired++; st.lastFired = now; }
            if (!ok) { st.errors++; st.lastError = sr->host->error(); scriptErr[sr->def.id] = sr->host->error(); }
        }
        if (!sr->acts.empty() || !ok) {
            Firing fi;
            fi.time = now; fi.rule = sr->def.name; fi.eventType = ev.type; fi.summary = brief(ev);
            fi.actions = sr->acts; fi.ok = ok;
            if (!ok) fi.error = sr->host->error();
            addFiring(fi);
        }
    }
}

FireReport Engine::test(const Rule& r, const Event& ev, bool execute) {
    FireReport rep;
    std::lock_guard<std::mutex> pl(p_->procMu);
    double now;
    Env env;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        now = p_->clock();
        env = p_->envFor(ev, now);
    }
    if (!triggerMatches(r.trigger, ev.type)) { rep.why = "the sample event is of another type than the trigger"; return rep; }
    ExprError ee;
    auto ex = Expr::parse(conditionExpr(r), ee);
    if (!ex) { rep.why = "condition: " + ee.text(); return rep; }
    std::string err;
    if (!ex->test(env, &err)) { rep.why = err.empty() ? "the condition is false for this event" : "condition: " + err; return rep; }
    if (r.geo != Rule::GeoOff) {
        double la, lo;
        if (!numField(ev, "lat", la) || !numField(ev, "lon", lo)) { rep.why = "the geofence needs the event to have lat and lon"; return rep; }
        bool inside = false;
        if (r.geo == Rule::GeoCircle) {
            double clat = r.geoLat, clon = r.geoLon;
            if (r.geoHome) { if (!env.stationSet) { rep.why = "the geofence is around the receiver position, which is not set"; return rep; } clat = env.stationLat; clon = env.stationLon; }
            inside = geo::distanceKm(la, lo, clat, clon) <= r.geoRadiusKm;
        } else {
            std::vector<std::pair<double, double>> pts;
            if (!geo::parsePolygon(r.geoPolygon, pts)) { rep.why = "bad polygon"; return rep; }
            inside = geo::pointInPolygon(la, lo, pts);
        }
        if (inside != r.geoInside) { rep.why = inside ? "the event is inside the geofence, the rule wants outside" : "the event is outside the geofence"; return rep; }
    }
    rep.matched = true;
    bool ok;
    p_->runActions(r, ev, execute, rep.actions, ok);
    if (execute) {
        Firing fi;
        fi.time = now; fi.rule = r.name; fi.eventType = ev.type; fi.summary = brief(ev); fi.actions = rep.actions; fi.ok = ok; fi.test = true;
        p_->addFiring(fi);
    }
    return rep;
}

Engine& global() {
    static Engine* e = new Engine();   // lives to the end of the program: decoders may emit while it shuts down
    return *e;
}

}  // namespace dect2::automation
