// Radios on the network that the user adds by address (issues #28, #54, #68): a PlutoSDR by IP, an rtl_tcp server, a SoapyRemote server,
// a SpyServer, a Hermes Lite 2 (found by discovery, or by address).
// They are listed with the other radios (listRadios) once setNetworkRadios() has them.
#pragma once
#include "source.h"
#include <string>
#include <vector>

namespace dect2 {

struct NetRadio {
    enum Type { Pluto = 0, RtlTcp = 1, SoapyRemote = 2, SpyServer = 3, HermesLite2 = 4 } type = RtlTcp;
    std::string host;   // name or address ("ip:" in front of a Pluto's is accepted and dropped)
    int port = 0;       // 0 = the type's default (a Pluto: libiio's own, 30431)
    bool operator==(const NetRadio& o) const { return type == o.type && host == o.host && port == o.port; }
};

const char* netRadioTypeName(NetRadio::Type t);       // "PlutoSDR", "rtl_tcp", "SoapyRemote", "SpyServer", "Hermes Lite 2"
int netRadioDefaultPort(NetRadio::Type t);            // 0 (Pluto), 1234, 55132, 5555, 1024
const char* netRadioDefaultHost(NetRadio::Type t);    // "192.168.2.1" for a Pluto, "" otherwise
int netRadioPort(const NetRadio& r);                  // the port connected to (a Pluto without one: 30431, its iiod)

// The list as the settings keep it: "rtltcp,host,port;pluto,host,0;soapyremote,host,port;spyserver,host,port;hl2,host,port" (and back; bad
// items are skipped)
std::string netRadiosToString(const std::vector<NetRadio>& v);
std::vector<NetRadio> netRadiosFromString(const std::string& s);

// The radios listRadios() adds after the ones it finds (thread-safe)
void setNetworkRadios(const std::vector<NetRadio>& v);
std::vector<NetRadio> networkRadios();
// The device entries of these radios: a Pluto as the native "ip:<host>" entry, rtl_tcp as the native "rtltcp" one, SoapyRemote as the
// SoapySDR device "driver=remote,remote=tcp://host:port" (left out when built without SoapySDR)
std::vector<DeviceInfo> netRadioDevices(const std::vector<NetRadio>& v);
// The name the entry of r has in the radio list
std::string netRadioDeviceName(const NetRadio& r);

// Whether the radio answers within timeoutMs: a TCP connection to it (an rtl_tcp server must also send its "RTL0" header, a
// SpyServer its DEVICE_INFO; a Hermes Lite 2 must answer a discovery packet sent to it). Blocks: call it
// on a worker thread. false: err says why, in plain words.
bool probeNetRadio(const NetRadio& r, int timeoutMs, std::string& err);

// A radio that answered a discovery: its IPv4 address, what it said about itself, and whether it is streaming to a program already
struct FoundNetRadio {
    std::string host;
    std::string detail;   // "MAC 00:1C:C0:A2:12:34, gateware 73.2"
    bool busy = false;
};
// The Hermes Lite 2 radios on this computer's networks: an openHPSDR discovery packet broadcast on each of them (UDP port 1024), the
// answers that come within timeoutMs. Blocks for timeoutMs: call it on a worker thread.
std::vector<FoundNetRadio> discoverHermesLite2(int timeoutMs);
// The rate limits of a radio as its own settings (TuneSettings::radio) make them: an rtl_tcp server set to (or found serving) another
// radio than an RTL-SDR goes up to 20 Msps without the RTL-SDR's rate gap; a SpyServer offers the decimation steps it was last
// seen with. Other radios are left as they are.
void applyRadioOptionCaps(DeviceInfo& d, const TuneSettings& t);

} // namespace dect2
