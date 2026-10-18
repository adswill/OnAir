// Radios on the network that the user adds by address (issues #28, #54, #68): a PlutoSDR by IP, an rtl_tcp server, a SoapyRemote server,
// a SpyServer, an SDR++ server, a Hermes Lite 2, a Red Pitaya, a KiwiSDR (found by discovery, or by address).
// They are listed with the other radios (listRadios) once setNetworkRadios() has them.
#pragma once
#include "dect2/source/source.h"
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct NetRadio {
    enum Type { Pluto = 0, RtlTcp = 1, SoapyRemote = 2, SpyServer = 3, HermesLite2 = 4, RedPitaya = 5, SdrppServer = 6, NrspSt = 7, KiwiSdr = 8 } type = RtlTcp;
    std::string host;   // name or address ("ip:" in front of a Pluto's is accepted and dropped)
    int port = 0;       // 0 = the type's default (a Pluto: libiio's own, 30431)
    std::string remote; // KiwiSDR: the password ("" = none; stored in the settings, never shown or logged). SoapyRemote: which of the server's radios to ask for, "mirisdr" or "driver=mirisdr,serial=1" (empty: the ones it lists)
    bool operator==(const NetRadio& o) const { return type == o.type && host == o.host && port == o.port && remote == o.remote; }
};

// SoapyRemote: one radio a server offers (the keys of its enumeration, "remote:" prefix dropped)
struct RemoteSoapyDevice { std::string driver, serial, label; };
// The SoapySDR arguments of a SoapyRemote radio: "driver=remote,remote=tcp://host:port" plus every key=value of r.remote as "remote:key=value"
// ("mirisdr" alone means driver=mirisdr)
std::string soapyRemoteArgs(const NetRadio& r);
// What the enumeration of a server (one kwargs map per radio) offers, without sound-card drivers (audio) unless includeAudio
std::vector<RemoteSoapyDevice> soapyRemoteOffers(const std::vector<std::map<std::string, std::string>>& kwargs, bool includeAudio = false);
// The enumeration used for a SoapyRemote server's radios: given the SoapySDR arguments, one kwargs map per radio found. Tests replace it.
using SoapyEnumerateFn = std::vector<std::map<std::string, std::string>> (*)(const std::string& args);
void setSoapyEnumerator(SoapyEnumerateFn fn);   // nullptr: SoapySDR's own; also forgets what was remembered
// The radios the server of r offers (asks it, then remembers the answer for a minute). Blocks while it asks: not for the UI thread.
std::vector<RemoteSoapyDevice> soapyRemoteList(const NetRadio& r, bool includeAudio = false);
// Whether SoapySDR has SoapyRemote's client module (the "remote" driver); false without SoapySDR. Tests force it: 0 / 1, -1 = real.
bool soapyRemoteModuleAvailable();
const char* soapyRemoteModuleHelp();   // what is missing on THIS platform and what to do (Windows/macOS: a damaged install; Linux: apt install soapysdr-module-remote)
const char* soapyRemoteModuleHelpFor(int platform);   // the same for 0 Windows, 1 macOS, 2 Linux (the app translates these three texts)
void setSoapyRemoteModuleOverride(int v);
// The plain-language reason a SoapyRemote radio could not be opened: what the server offers and what to type in "Remote driver". "" for other args.
std::string soapyRemoteFailureHelp(const std::string& soapyArgs);

const char* netRadioTypeName(NetRadio::Type t);       // "PlutoSDR", "rtl_tcp", "SoapyRemote", "SpyServer", "Hermes Lite 2", "Red Pitaya", "SDR++ server"
int netRadioDefaultPort(NetRadio::Type t);            // 0 (Pluto), 1234, 55132, 5555, 1024, 5259 (SDR++ server)
const char* netRadioDefaultHost(NetRadio::Type t);    // "192.168.2.1" for a Pluto, "" otherwise
int netRadioPort(const NetRadio& r);                  // the port connected to (a Pluto without one: 30431, its iiod)

// The list as the settings keep it: "rtltcp,host,port;pluto,host,0;soapyremote,host,port;spyserver,host,port;hl2,host,port;redpitaya,host,port;sdrpp,host,port;kiwisdr,host,port,password" (and back; bad
// items are skipped)
std::string netRadiosToString(const std::vector<NetRadio>& v);
std::vector<NetRadio> netRadiosFromString(const std::string& s);

// The radios listRadios() adds after the ones it finds (thread-safe)
void setNetworkRadios(const std::vector<NetRadio>& v);
std::vector<NetRadio> networkRadios();
// The device entries of these radios: a Pluto as the native "ip:<host>" entry, rtl_tcp as the native "rtltcp" one, SoapyRemote as the
// SoapySDR device "driver=remote,remote=tcp://host:port" (left out when built without SoapySDR)
std::vector<DeviceInfo> netRadioDevices(const std::vector<NetRadio>& v);
// "host:port" of r as the radio's device entry keys it (nativeArgs of a native radio)
std::string netRadioHostPort(const NetRadio& r);
// The name the entry of r has in the radio list (a Hermes Lite 2 or Red Pitaya: its RX1; one entry per receiver is listed)
std::string netRadioDeviceName(const NetRadio& r);

// Whether the radio answers within timeoutMs: a TCP connection to it (an rtl_tcp server must also send its "RTL0" header, a
// SpyServer its DEVICE_INFO, an SDR++ server its menu; a Hermes Lite 2 must answer a discovery packet sent to it). Blocks: call it
// on a worker thread. false: err says why, in plain words.
bool probeNetRadio(const NetRadio& r, int timeoutMs, std::string& err);

// A radio that answered a discovery: its IPv4 address, what it said about itself, and whether it is streaming to a program already
struct FoundNetRadio {
    std::string host;
    std::string detail;   // "MAC 00:1C:C0:A2:12:34, gateware 73.2, 4 receivers"
    int receivers = 0;    // the receivers (slices) it has, 0 = it did not say
    bool busy = false;
};
// The Hermes Lite 2 radios on this computer's networks: an openHPSDR discovery packet broadcast on each of them (UDP port 1024), the
// answers that come within timeoutMs. Blocks for timeoutMs: call it on a worker thread.
std::vector<FoundNetRadio> discoverHermesLite2(int timeoutMs);
// The same for Red Pitaya boards running the HPSDR-compatible SDR transceiver: the discovery reply's board id is 1 (Hermes), so another
// Hermes-protocol radio with that id would be listed too (it cannot be told apart by the reply)
std::vector<FoundNetRadio> discoverRedPitaya(int timeoutMs);
// The rate limits of a radio as its own settings (TuneSettings::radio) make them: an rtl_tcp server set to (or found serving) another
// radio than an RTL-SDR goes up to 20 Msps without the RTL-SDR's rate gap; a SpyServer offers the decimation steps it was last
// seen with. Other radios are left as they are.
void applyRadioOptionCaps(DeviceInfo& d, const TuneSettings& t);

} // namespace dect2
