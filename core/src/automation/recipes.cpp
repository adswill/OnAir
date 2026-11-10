#include "dect2/automation/recipes.h"

namespace dect2::automation {

namespace {
ActionSpec act(const char* type, std::map<std::string, std::string> p = {}) { ActionSpec a; a.type = type; a.p = std::move(p); return a; }

Recipe rule(const char* name, const char* note, const char* trigger, const char* when, std::vector<ActionSpec> actions, const char* key = "", double cooldown = 60) {
    Recipe r;
    r.kind = Recipe::KindRule;
    r.rule.name = name;
    r.rule.id = makeId(name);
    r.rule.note = note;
    r.rule.enabled = false;
    r.rule.trigger = trigger;
    r.rule.when = when;
    r.rule.cooldownKey = key;
    r.rule.cooldownSec = cooldown;
    r.rule.actions = std::move(actions);
    return r;
}

Recipe script(const char* name, const char* note, std::vector<std::string> events, const char* code) {
    Recipe r;
    r.kind = Recipe::KindScript;
    r.script.name = name;
    r.script.id = makeId(name);
    r.script.note = note;
    r.script.enabled = false;
    r.script.events = std::move(events);
    r.script.code = code;
    return r;
}
}  // namespace

std::vector<Recipe> builtinRecipes() {
    std::vector<Recipe> v;
    v.push_back(rule("Emergency squawk 7500 7600 7700", "Hijack, radio failure or general emergency: tells you, beeps and records 60 s of the radio's signal.",
        "adsb.aircraft", "squawk in [7500, 7600, 7700]",
        {act("notify", {{"title", "Squawk {{squawk}}"}, {"body", "{{callsign}} ({{hex}}) at {{alt_ft}} ft"}}), act("sound"), act("record_iq", {{"seconds", "60"}})}, "hex", 900));
    v.push_back(rule("Military aircraft by address", "Aircraft whose ICAO address is in a military block (US AE and AF, UK 43C). Edit the list to taste.",
        "adsb.new", "hex startswith 'AE' or hex startswith 'AF' or hex startswith '43C'",
        {act("notify", {{"title", "Military aircraft"}, {"body", "{{hex}} {{callsign}}"}}), act("log", {{"file", "military"}})}, "hex", 3600));
    {
        Recipe r = rule("Aircraft within 20 km of home", "Tells you once when an aircraft comes within 20 km of the receiver position (set in the ADS-B window).",
            "adsb.aircraft", "has('lat')", {act("notify", {{"title", "Aircraft overhead"}, {"body", "{{callsign}} {{hex}} at {{alt_ft}} ft, {{distance_km}} km"}})}, "hex", 600);
        r.rule.geo = Rule::GeoCircle; r.rule.geoHome = true; r.rule.geoRadiusKm = 20; r.rule.geoEnterOnly = true;
        v.push_back(r);
    }
    v.push_back(rule("Pager keyword alert", "A pager message that mentions one of the words: edit the words in the condition.",
        "pager.message", "text contains 'fire' or text contains 'alarm' or text contains 'ambulance'",
        {act("notify", {{"title", "Pager"}, {"body", "{{address}}: {{text}}"}}), act("log", {{"file", "pager-alerts"}})}, "", 30));
    v.push_back(rule("New signal in the 2 m band", "The sweep scanner finds a new signal between 144 and 146 MHz.",
        "signal.new", "freq_mhz >= 144 and freq_mhz <= 146",
        {act("notify", {{"title", "New signal"}, {"body", "{{freq_mhz}} MHz, {{snr_db}} dB over the noise"}}), act("log", {{"file", "new-signals"}})}, "", 30));
    v.push_back(rule("Receiver dropped or overloaded", "The radio stopped sending samples, or the computer cannot keep up.",
        "receiver.state", "state in ['dropped', 'overload']",
        {act("notify", {{"title", "Receiver {{state}}"}, {"body", "{{detail}}"}}), act("sound")}, "", 300));
    v.push_back(rule("ISS pass recording", "Records the radio signal while the tracked ISS pass lasts (track the ISS in the Passes window).",
        "satellite.aos", "name contains 'ISS'",
        {act("notify", {{"title", "{{name}} is up"}, {"body", "Highest point {{max_el_deg}} degrees"}}), act("record_iq", {{"seconds", "600"}})}, "", 3600));
    v.push_back(rule("NOAA / Meteor pass recording", "Records the weather satellite's pass (track it in the Passes window).",
        "satellite.aos", "name startswith 'NOAA' or name startswith 'METEOR'",
        {act("notify", {{"title", "{{name}} pass starts"}, {"body", "Highest point {{max_el_deg}} degrees"}}), act("record_iq", {{"seconds", "900"}})}, "", 600));
    {
        Recipe r = rule("Vessel enters a harbour area", "Tells you when a vessel comes into the polygon (the example is around Klaipeda): replace the points by your own.",
            "ais.vessel", "has('lat')", {act("notify", {{"title", "{{name}} entered the area"}, {"body", "MMSI {{mmsi}}, {{sog_kt}} kt"}})}, "mmsi", 600);
        r.rule.geo = Rule::GeoPolygon; r.rule.geoPolygon = "55.75,21.05; 55.75,21.15; 55.65,21.15; 55.65,21.05"; r.rule.geoEnterOnly = true;
        v.push_back(r);
    }
    v.push_back(rule("Radiosonde landed near me", "A radiosonde has come down within 50 km of the receiver position: go and find it.",
        "sonde.landed", "distance_km <= 50",
        {act("notify", {{"title", "Sonde {{serial}} landed"}, {"body", "{{distance_km}} km away at {{lat}}, {{lon}}"}}), act("log", {{"file", "sondes-landed"}})}, "serial", 3600));
    v.push_back(rule("APRS message to me", "An APRS message addressed to your callsign (set it in APRS, Messages): tells you and beeps.",
        "aprs.message", "to_me",
        {act("notify", {{"title", "APRS message from {{from}}"}, {"body", "{{text}}"}}), act("sound")}, "from", 30));
    v.push_back(rule("Freezer too warm", "A sensor you named with \"freezer\" in the name reads above -12 °C. Name your sensors in Sensors (click the name).",
        "sensor.reading", "named and name contains 'freezer' and temperature_C > -12",
        {act("notify", {{"title", "{{name}} is at {{temperature_C}} °C"}, {"body", "Above -12 °C"}}), act("sound")}, "key", 1800));
    v.push_back(rule("Frost", "One of your named sensors reads below 0 °C.",
        "sensor.reading", "named and temperature_C < 0",
        {act("notify", {{"title", "Frost: {{name}} {{temperature_C}} °C"}, {"body", "Below 0 °C"}})}, "key", 6 * 3600));
    v.push_back(rule("Water leak", "One of your named leak sensors reports water.",
        "sensor.reading", "named and (detect_wet == 1 or water == 1 or leak == 1)",
        {act("notify", {{"title", "Water: {{name}}"}, {"body", "The leak sensor is wet"}}), act("sound")}, "key", 300));
    v.push_back(rule("Door or window opened", "One of your named door or window sensors reports open.",
        "sensor.reading", "named and (contact_open == 1 or opened == 1)",
        {act("notify", {{"title", "Open: {{name}}"}, {"body", "{{model}} {{id}}"}})}, "key", 60));
    v.push_back(rule("Sensor battery low", "One of your named sensors reports a low battery. Once a day per sensor.",
        "sensor.reading", "named and battery_low",
        {act("notify", {{"title", "Battery low: {{name}}"}, {"body", "{{model}} {{id}}"}})}, "key", 86400));
    v.push_back(rule("Favourite artist on the radio", "A station you listen to (FM or DAB) plays an artist you name: tells you and records 4 minutes of the sound. Change the name.",
        "broadcast.now_playing", "artist contains 'change me'",
        {act("notify", {{"title", "{{artist}} on {{station}}"}, {"body", "{{title}}"}}), act("record_audio", {{"seconds", "240"}})}, "station", 600));
    v.push_back(rule("Word in the radio text", "The radio text or DAB label of the station mentions a word (traffic, a place, a name). Change the word.",
        "broadcast.text", "text contains 'traffic'",
        {act("notify", {{"title", "{{station}}"}, {"body", "{{text}}"}})}, "station", 300));
    v.push_back(rule("Traffic announcement", "The FM station starts a traffic announcement (TA): tells you and records 2 minutes.",
        "broadcast.text", "ta_start",
        {act("notify", {{"title", "Traffic on {{station}}"}, {"body", "{{text}}"}}), act("record_audio", {{"seconds", "120"}})}, "station", 120));
    v.push_back(rule("Emergency squawk to my phone (ntfy)", "Sends the emergency squawk to the ntfy app on your phone. Change the topic to something only you know.",
        "adsb.aircraft", "squawk in [7500, 7600, 7700]",
        {act("webhook", {{"preset", "ntfy"}, {"topic", "onair-change-this-topic"}, {"title", "Squawk {{squawk}}"}, {"text", "{{callsign}} {{hex}} at {{alt_ft}} ft"}})}, "hex", 900));
    v.push_back(script("Busy sky counter", "Lua example: counts the aircraft heard in the last 5 minutes within 50 km and tells you when there are more than 15.", {"adsb.aircraft"},
        "-- on_event(e) is called for every event the script asked for\n"
        "local seen = {}\n"
        "local told = 0\n"
        "function on_event(e)\n"
        "  if not e.lat then return end\n"
        "  local d = distance_km(e.lat, e.lon)\n"
        "  if d == nil or d > 50 then return end\n"
        "  seen[e.hex] = e.time\n"
        "  local n = 0\n"
        "  for hex, t in pairs(seen) do\n"
        "    if e.time - t > 300 then seen[hex] = nil else n = n + 1 end\n"
        "  end\n"
        "  if n > 15 and e.time - told > 1800 then\n"
        "    told = e.time\n"
        "    notify('Busy sky', n .. ' aircraft within 50 km')\n"
        "  end\n"
        "end\n"));
    v.push_back(script("Low flyer logger", "Lua example: writes aircraft below 1500 ft that are not on the ground to the CSV file low-flyers.", {"adsb.aircraft"},
        "function on_event(e)\n"
        "  if e.alt_ft and e.alt_ft < 1500 and not e.ground and e.lat then\n"
        "    log_row('low-flyers', e.time, e.hex, e.callsign, e.alt_ft, e.lat, e.lon)\n"
        "  end\n"
        "end\n"));
    return v;
}

}  // namespace dect2::automation
