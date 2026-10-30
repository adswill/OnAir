// Integrations: decoded events (ADS-B, AIS, APRS, pagers, ACARS/VDL2, sondes, voice call records ...) published as MQTT messages, posted to a webhook
// or appended to a JSON-lines file. Everything is off until the user switches it on and types the broker / address; there are no default servers.
// The password and tokens are the user's: they live in the settings file (owner-only) and are never put into a status text, a log line or an event.
#pragma once
#include "dect2/automation/event.h"
#include "dect2/automation/engine.h"
#include "dect2/integrations/mqtt.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::integrations {

struct MqttSettings {
    bool enabled = false;
    std::string host;                         // typed by the user; nothing is contacted while it is empty
    int port = 1883;                          // 8883 is usual with TLS
    bool tls = false;
    std::string user, password;
    std::string clientId;                     // empty: "onair-" + a random suffix chosen at start
    std::string topicTemplate = "onair/{family}/{sub}/{ident}";
    bool retain = false;
    int qos = 0;                              // 0 or 1
    bool haDiscovery = false;                 // Home Assistant MQTT discovery for a few sensors
    std::string haPrefix = "homeassistant";
    std::string haBase = "onair";             // availability and sensor state topics: <haBase>/status, <haBase>/ha/<sensor>
};

struct WebhookSettings {
    bool enabled = false;
    std::string url;                          // http:// or https://
    std::string token;                        // optional, sent as "Authorization: Bearer"
};

struct JsonlSettings {
    bool enabled = false;
    std::string path;                         // one JSON object per line, appended
    int maxMegabytes = 100;                   // over this the file is renamed to <path>.1 and a new one started; 0 = no limit
};

struct Settings {
    automation::FamilyMask families = 0;      // which families go out (bit = automation::Family); none until the user ticks
    int maxPerSecond = 100;                   // events over this are dropped and counted; 0 = no limit
    MqttSettings mqtt;
    WebhookSettings webhook;
    JsonlSettings jsonl;
    bool anyEnabled() const { return (mqtt.enabled && !mqtt.host.empty()) || (webhook.enabled && !webhook.url.empty()) || (jsonl.enabled && !jsonl.path.empty()); }
};

// The families that go out when the user switches Integrations on and has not chosen: the decoded-message ones, not voice calls, plug-ins, the session store,
// receiver, satellite or time events.
automation::FamilyMask defaultFamilies();

// ---- the parts, public for the tests ----
// "onair/{family}/{sub}/{ident}": {family} adsb, {type} adsb.aircraft, {sub} aircraft, {mode}, {ident} (icao, mmsi, call sign, id ...), {freq_mhz}, {field:NAME}.
// Every substituted value is made safe for a topic level (no / + # or control characters, spaces to _, empty to "_").
std::string expandTopic(const std::string& tpl, const automation::Event& ev);
// Empty string when fine, else why the template cannot be used (empty, wildcards, leading /, NUL, too long).
std::string checkTopicTemplate(const std::string& tpl);
// The best single identifier of an event (icao/hex, mmsi, callsign, id, serial, address, src, mac, name ...), or empty.
std::string eventIdent(const automation::Event& ev);
// {"source":"OnAir","type":..,"mode":..,"freq_hz":..,"time":..,"ident":..,"fields":{..}}
std::string eventJson(const automation::Event& ev);
// Home Assistant discovery: the sensors, each with its config topic, its state topic and the retained config payload.
struct HaSensor { std::string key, name, family, icon, configTopic, stateTopic, configJson; };
std::vector<HaSensor> haSensors(const MqttSettings& m);
// Text form of the settings (JSON) for the file in the data folder.
std::string toJson(const Settings& s);
bool fromJson(const std::string& text, Settings& s, std::string& err);
std::string httpUrlProblem(const std::string& url);   // empty when it is a usable http(s) address

struct Stats {
    uint64_t mqttSent = 0, mqttFailed = 0, webhookSent = 0, webhookFailed = 0, jsonlLines = 0, dropped = 0, rateDropped = 0;
    std::string mqttStatus;                   // "connected", "off", "waiting to retry: ..."; never holds the password
    std::string webhookStatus, jsonlStatus;
};

// The web post of the webhook sink is the app's (it has the HTTP client); a function that returns true when the server accepted it.
using PostFn = std::function<bool(const std::string& url, const std::string& body, const std::vector<std::string>& headers)>;

class Hub {
public:
    Hub();
    ~Hub();
    void setSettings(const Settings& s);      // takes effect at once; switching MQTT off disconnects
    Settings settings() const;
    void setPost(PostFn f);
    void setClock(std::function<double()> now);   // tests
    // For the decoder's thread: filters by family, applies the rate limit, queues. Cheap, never blocks, never touches the network.
    void submit(const automation::Event& ev);
    void start();
    void stop();
    Stats stats() const;
    // Sends a small test message through every sink that is switched on; the result goes to the status texts.
    void sendTest();
    // For the tests: wait until the queue is empty and the worker is idle (or timeout in ms).
    bool flush(int timeoutMs);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

Hub& global();

}  // namespace dect2::integrations
