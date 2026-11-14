// Other payloads of a transport stream, recognised from their packets (signalling and labelling only, nothing is descrambled): the application information table
// (AIT, EN 102 809: HbbTV and MHP applications with their names, control codes and where they load from), SCTE-35 splice information (ad insertion cues), EBU data in
// PES (teletext, subtitles, VPS/WSS data identified by their data_identifier), DSM-CC and MPE PIDs, and a label for every private PID that is not known otherwise.
#pragma once
#include "dvb_text.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct AitApplication {
    uint32_t orgId = 0;
    int appId = 0, controlCode = 0;       // 1 autostart, 2 present, 3 destroy, 4 kill, 5 prefetch, 7 remote, 8 unbound
    std::string controlName, name, lang;
    int visibility = -1, priority = -1;
    std::vector<int> profiles;
    std::vector<std::string> transports;  // "object carousel (component 0x0A)", "HTTP https://x/", "IP multicast"
    std::string baseUrl, initialPath;
};

struct AitTable { int pid = 0, appType = 0, version = 0; std::string typeName; std::vector<AitApplication> apps; };

struct Scte35Cue { int pid = 0; int command = 0; std::string commandName; double ptsSec = 0; bool hasPts = false; uint32_t eventId = 0; bool outOfNetwork = false; double durationSec = 0; uint64_t count = 0; };

struct TsDataPid {
    int pid = 0;
    std::string kind;             // "AIT (application signalling)", "SCTE-35", "DSM-CC carousel", "MPE (IP data)", "EBU teletext", ...
    uint64_t packets = 0;
};

struct TsDataSnapshot {
    std::vector<TsDataPid> pids;
    std::vector<AitTable> ait;
    std::vector<Scte35Cue> scte;
    bool empty() const { return pids.empty(); }
};

const char* aitAppTypeName(int t);
const char* aitControlName(int c);

class TsDataScan {
public:
    void packet(const uint8_t* p188);
    TsDataSnapshot snapshot() const;
    void reset() { *this = TsDataScan(); }
    void setTextContext(const dvbsi::TextContext& context) { textContext_ = context; }
private:
    dvbsi::TextContext textContext_;
    struct Pid { std::vector<uint8_t> partial; bool active = false; uint64_t packets = 0; std::string kind; bool pes = false; int tableSeen = -1; };
    void section(int pid, const uint8_t* s, int len);
    void ait(int pid, const uint8_t* s, int len);
    void scte(int pid, const uint8_t* s, int len);
    std::map<int, Pid> pids_;
    std::map<std::pair<int, int>, AitTable> ait_;     // by (pid, application type)
    std::map<std::pair<int, int>, Scte35Cue> scte_;   // by (pid, command)
};

} // namespace dect2
