#include "dect2/automation/catalog.h"
#include <cstdlib>

namespace dect2::automation {

namespace {
const char* kAc = "ADS-B";
std::vector<FieldInfo> adsbFields() {
    return {
        {"hex", 's', "ICAO address, six hex digits, upper case", "4840D6"},
        {"callsign", 's', "flight id, empty until sent", "KLM1023"},
        {"squawk", 'n', "transponder code as written (7700)", "1000"},
        {"alt_ft", 'n', "barometric altitude", "36000"},
        {"speed_kt", 'n', "speed in knots", "450"},
        {"heading_deg", 'n', "track or heading", "182"},
        {"vrate_fpm", 'n', "climb rate in feet per minute", "0"},
        {"lat", 'n', "latitude of a decoded position", "54.69"},
        {"lon", 'n', "longitude of a decoded position", "25.28"},
        {"distance_km", 'n', "from the receiver position (when set)", "35"},
        {"ground", 'b', "on the ground", "false"},
        {"emergency", 'n', "emergency state 0 to 7; 7500, 7600 and 7700 squawks give 1, 4 and 3 or more", "0"},
        {"category", 's', "emitter category such as A3", "A3"},
        {"messages", 'n', "messages received from this aircraft", "120"},
    };
}
std::vector<EventInfo> build() {
    std::vector<EventInfo> v;
    v.push_back({"adsb.new", "Aircraft heard for the first time", kAc, "ADS-B", 1090e6, adsbFields()});
    v.push_back({"adsb.aircraft", "Aircraft update (new squawk, or every 2 s per aircraft)", kAc, "ADS-B", 1090e6, adsbFields()});
    v.push_back({"acars.message", "ACARS message", "ACARS", "ACARS", 131.725e6, {
        {"tail", 's', "registration or address", "A6-EDA"}, {"flight", 's', "flight id of a downlink", "EK0201"}, {"label", 's', "two-character label", "H1"},
        {"text", 's', "message text", "REQUEST PUSHBACK"}, {"downlink", 'b', "aircraft to ground", "true"}, {"msg_num", 's', "message number", "M12A"},
        {"lat", 'n', "position when the message has one", "54.7"}, {"lon", 'n', "position when the message has one", "25.3"}}});
    v.push_back({"vdl2.message", "VDL mode 2 frame", "VDL2", "VDL2", 136.975e6, {
        {"src", 's', "source address, hex", "4840D6"}, {"dst", 's', "destination address, hex", "100A12"}, {"from_air", 'b', "sent by an aircraft", "true"},
        {"proto", 's', "ACARS, X.25, CLNP, XID ...", "ACARS"}, {"summary", 's', "one-line reading", "ACARS label H1"}, {"frame", 's', "frame type", "I"}}});
    v.push_back({"hfdl.message", "HFDL message", "HFDL", "HFDL", 8.927e6, {
        {"kind", 's', "message kind", "Unnumbered data"}, {"summary", 's', "one-line reading", "Logon request"}, {"icao", 's', "aircraft address, hex, when known", "4840D6"},
        {"uplink", 'b', "ground to air", "false"}, {"snr_db", 'n', "signal to noise ratio", "18"}}});
    v.push_back({"pager.message", "Pager message (POCSAG, FLEX)", "Pagers", "POCSAG", 152.0e6, {
        {"protocol", 's', "POCSAG or FLEX", "POCSAG"}, {"speed", 's', "POCSAG 1200 ...", "POCSAG 1200"}, {"address", 'n', "RIC / capcode", "1234567"},
        {"function", 'n', "POCSAG function bits, -1 for FLEX", "3"}, {"type", 's', "tone, numeric, alpha, secure, binary", "alpha"},
        {"text", 's', "the message", "FIRE ALARM MAIN ST 12"}, {"damaged", 'b', "a code word could not be repaired", "false"}}});
    v.push_back({"ais.vessel", "Vessel heard (position report)", "AIS", "AIS", 162.0e6, {
        {"mmsi", 'n', "MMSI number", "219012345"}, {"name", 's', "ship name", "NORDIC STAR"}, {"callsign", 's', "radio call sign", "OZ1234"},
        {"ship_type", 'n', "ship type code 0 to 99", "70"}, {"lat", 'n', "latitude", "55.1"}, {"lon", 'n', "longitude", "21.2"},
        {"sog_kt", 'n', "speed over ground", "12"}, {"cog_deg", 'n', "course over ground", "270"}, {"nav_status", 'n', "navigation status 0 to 15", "0"},
        {"class", 's', "A, B, base, aid ...", "A"}, {"destination", 's', "destination text", "KLAIPEDA"}}});
    v.push_back({"ais.cpa", "Vessel on a close-approach course (CPA alert)", "AIS", "AIS", 162.0e6, {
        {"mmsi", 'n', "MMSI number", "219012345"}, {"name", 's', "ship name", "NORDIC STAR"}, {"ship_type", 'n', "ship type code 0 to 99", "70"},
        {"class", 's', "A, B, base, aid ...", "A"}, {"lat", 'n', "latitude", "55.1"}, {"lon", 'n', "longitude", "21.2"},
        {"sog_kt", 'n', "speed over ground", "12"}, {"cog_deg", 'n', "course over ground", "270"}, {"distance_nm", 'n', "distance now, nautical miles", "3.2"},
        {"cpa_nm", 'n', "distance at the closest approach", "0.3"}, {"tcpa_min", 'n', "minutes to the closest approach", "9"}}});
    v.push_back({"ais.geofence", "Vessel entered or left a geofence", "AIS", "AIS", 162.0e6, {
        {"mmsi", 'n', "MMSI number", "219012345"}, {"name", 's', "ship name", "NORDIC STAR"}, {"ship_type", 'n', "ship type code 0 to 99", "70"},
        {"class", 's', "A, B, base, aid ...", "A"}, {"lat", 'n', "latitude", "55.1"}, {"lon", 'n', "longitude", "21.2"},
        {"sog_kt", 'n', "speed over ground", "12"}, {"cog_deg", 'n', "course over ground", "270"}, {"distance_nm", 'n', "distance from the receiver position (0 when not set)", "3.2"},
        {"fence", 's', "name of the geofence", "Harbour"}, {"transition", 's', "enter or exit", "enter"}}});
    v.push_back({"aprs.packet", "AX.25 / APRS packet", "APRS", "APRS", 144.8e6, {
        {"from", 's', "source call sign", "LY1ABC-9"}, {"to", 's', "destination", "APRS"}, {"path", 's', "digipeater path", "WIDE1-1"},
        {"info", 's', "information field", "!5441.40N/02517.00E>test"}, {"type", 's', "packet type", "position"}, {"summary", 's', "one-line reading", "position"},
        {"lat", 'n', "latitude when the packet has a position", "54.69"}, {"lon", 'n', "longitude when the packet has a position", "25.28"}}});
    v.push_back({"sonde.update", "Radiosonde frame (at most one per 5 s per sonde)", "Radiosondes", "Sonde", 403.0e6, {
        {"serial", 's', "serial number", "V1234567"}, {"type", 's', "RS41, DFM, M10 ...", "RS41"}, {"lat", 'n', "latitude", "54.5"}, {"lon", 'n', "longitude", "25.0"},
        {"alt_m", 'n', "altitude in metres", "12000"}, {"vspeed_ms", 'n', "climb rate m/s (negative: falling)", "5"}, {"hspeed_ms", 'n', "ground speed m/s", "20"},
        {"temp_c", 'n', "air temperature", "-40"}, {"batt_v", 'n', "battery volts", "2.7"}, {"sats", 'n', "GPS satellites", "9"}}});
    v.push_back({"sonde.landed", "Radiosonde has landed (falling, then silent for 90 s)", "Radiosondes", "Sonde", 403.0e6, {
        {"serial", 's', "serial number", "V1234567"}, {"type", 's', "RS41, DFM, M10 ...", "RS41"}, {"lat", 'n', "last latitude", "54.5"}, {"lon", 'n', "last longitude", "25.0"},
        {"alt_m", 'n', "last altitude", "800"}, {"max_alt_m", 'n', "highest altitude of the flight", "28000"}, {"distance_km", 'n', "from the receiver position (when set)", "12"}}});
    v.push_back({"gnss.fix", "GNSS position fix", "GNSS", "GNSS", 1575.42e6, {
        {"lat", 'n', "latitude", "54.69"}, {"lon", 'n', "longitude", "25.28"}, {"alt_m", 'n', "altitude", "110"}, {"sats", 'n', "satellites used", "9"}}});
    v.push_back({"lora.packet", "LoRa / Meshtastic / LoRaWAN packet", "LoRa, mesh", "LoRa", 868.1e6, {
        {"kind", 's', "lora, meshtastic, meshcore, lorawan ...", "meshtastic"}, {"hex", 's', "payload, hex", "0A0B0C"}, {"snr_db", 'n', "signal to noise ratio", "6"}, {"sf", 'n', "spreading factor", "11"}}});
    v.push_back({"ble.device", "Bluetooth LE advertiser", "Bluetooth LE", "BLE", 2402e6, {
        {"mac", 's', "address", "AA:BB:CC:11:22:33"}, {"name", 's', "device name", "Tag"}, {"rssi_db", 'n', "signal level", "-70"}}});
    v.push_back({"rid.drone", "Drone Remote ID broadcast (at most one per 5 s per drone)", "Remote ID", "Remote ID", 2437e6, {
        {"id", 's', "serial number or session id", "1596F123"}, {"mac", 's', "transmitter address", "AA:BB:CC:11:22:33"}, {"lat", 'n', "latitude", "54.7"}, {"lon", 'n', "longitude", "25.3"},
        {"alt_m", 'n', "altitude (geodetic) in metres", "80"}, {"speed_ms", 'n', "ground speed m/s", "9"}, {"op_lat", 'n', "operator latitude", "54.69"}, {"op_lon", 'n', "operator longitude", "25.28"}}});
    v.push_back({"dsc.call", "Marine DSC call", "Marine", "DSC", 156.525e6, {
        {"category", 's', "routine, safety, urgency, distress", "distress"}, {"from_mmsi", 's', "calling station", "219000123"}, {"to_mmsi", 's', "called station or all ships", "all"},
        {"summary", 's', "one-line reading", "Distress: fire"}, {"kind", 's', "Distress alert, Distress alert acknowledgement, Test call ...", "Distress alert"}, {"nature", 's', "nature of distress", "Fire, explosion"},
        {"distress_mmsi", 's', "station in distress (acknowledgements and relays)", "219000123"}, {"from_country", 's', "country of the calling station's MID", "Denmark"}, {"text", 's', "the whole reading", ""},
        {"lat", 'n', "latitude of the position in the call", "55.1"}, {"lon", 'n', "longitude of the position in the call", "11.2"}}});
    v.push_back({"signal.new", "New signal found by the sweep scanner", "Sweep scanner", "Sweep", 433.92e6, {
        {"bw_hz", 'n', "width", "12500"}, {"peak_db", 'n', "peak level dBFS", "-60"}, {"snr_db", 'n', "signal to noise ratio", "18"}}});
    v.push_back({"receiver.state", "Receiver state changed", "all", "", 0, {
        {"state", 's', "started, stopped, dropped, reconnected, overload", "dropped"}, {"detail", 's', "a few words", "radio stopped sending samples"}}});
    v.push_back({"satellite.aos", "Tracked satellite pass starts", "Passes window", "", 137.1e6, {
        {"name", 's', "satellite", "NOAA 19"}, {"max_el_deg", 'n', "highest elevation of the pass", "62"}}});
    v.push_back({"satellite.los", "Tracked satellite pass ends", "Passes window", "", 137.1e6, {{"name", 's', "satellite", "NOAA 19"}}});
    // the end-of-session summaries of the mode experience framework (core/include/dect2/session): ids, counts and the title; never an alias, note or message text
    {
        static const char* kSes = "Calls, threads and tracks";
        auto sessionFields = [] {
            return std::vector<FieldInfo>{
                {"session_id", 's', "the session's id (a number written as text: it is 63 bits wide)", "1766010347528192001"},
                {"key", 's', "what the mode calls it: talkgroup and slot, capcode, ICAO address (a short tag for personal ids)", "TG 91/slot1"},
                {"title", 's', "the title shown in the Items tab", "DMR TG 91 Worldwide, 12 s"},
                {"kind", 's', "call, thread or track", "call"},
                {"verdict", 's', "clear, encrypted, unreadable or test (encrypted ones are metadata only)", "clear"},
                {"mode_num", 'n', "the mode's number (docs/MODE-NUMBERS.md)", "9"},
                {"receiver", 'n', "receiver instance 0, 1 ...", "0"},
                {"duration_s", 'n', "length in seconds", "12"},
                {"events", 'n', "frames, messages or points in it", "40"},
                {"emergency", 'b', "marked as an emergency", "false"},
            };
        };
        v.push_back({"session.call", "A call ended", kSes, "DMR", 438.5e6, sessionFields()});
        v.push_back({"session.thread", "A message thread ended", kSes, "ACARS", 131.725e6, sessionFields()});
        v.push_back({"session.track", "A track ended", kSes, "ADS-B", 1090e6, sessionFields()});
    }
    v.push_back({"time.minute", "Every minute (time-of-day triggers)", "clock", "", 0, {
        {"hour", 'n', "local hour 0 to 23", "7"}, {"minute", 'n', "minute 0 to 59", "30"}, {"weekday", 'n', "0 Sunday to 6 Saturday", "1"}}});
    return v;
}
}  // namespace

const std::vector<EventInfo>& eventCatalog() {
    static const std::vector<EventInfo> v = build();
    return v;
}

const EventInfo* findEventInfo(const std::string& type) {
    for (const EventInfo& e : eventCatalog()) if (type == e.type) return &e;
    return nullptr;
}

Event sampleEvent(const std::string& type, double now) {
    Event ev(type);
    ev.time = now;
    if (const EventInfo* ei = findEventInfo(type)) {
        ev.mode = ei->mode;
        ev.freqHz = ei->sampleFreqHz;
        for (const FieldInfo& f : ei->fields) {
            if (f.kind == 'n') ev.num(f.name, std::atof(f.sample));
            else if (f.kind == 'b') ev.flag(f.name, std::string(f.sample) == "true");
            else ev.str(f.name, f.sample);
        }
    }
    return ev;
}

bool triggerMatches(const std::string& trigger, const std::string& type) {
    if (trigger.empty() || type.empty()) return false;
    if (trigger == "*") return true;
    if (trigger.size() >= 2 && trigger.compare(trigger.size() - 2, 2, ".*") == 0) {
        const std::string pre = trigger.substr(0, trigger.size() - 1);   // "adsb."
        return type.compare(0, pre.size(), pre) == 0;
    }
    return trigger == type;
}

}  // namespace dect2::automation
