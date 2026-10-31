// Antenna mount drivers behind one small interface: Hamlib rotctld (TCP, any OS) and ASCOM Alpaca (HTTP REST, any OS, no COM).
// GS-232 stays in rotator.cpp. ASCOM COM drivers on Windows are reached through the ASCOM Remote server, which speaks Alpaca.
// Goto mounts take Alt/Az positions; tracking mounts (Alpaca MoveAxis) also take a speed per axis, in degrees per second.
// All calls block (a worker thread owns the driver); positions are mechanical degrees, azimuth 0..360 from north.
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct RotatorConfig;

struct MountPos {
    double azDeg = 0, elDeg = 0;
    bool azValid = false, elValid = false;
    bool slewing = false, slewingValid = false;   // Alpaca reports it; rotctld does not
};

class MountDriver {
public:
    virtual ~MountDriver() = default;
    virtual const char* name() const = 0;
    virtual bool connect(std::string& err) = 0;
    virtual void close() = 0;
    virtual bool gotoAzEl(double azDeg, double elDeg, std::string& err) = 0;
    virtual bool stop(std::string& err) = 0;   // halts a slew and any axis motion
    virtual bool readPosition(MountPos& out, std::string& err) = 0;
    // Tracking mounts: move the axes at the given speeds (deg/s, signed; 0 stops that axis). False when unsupported.
    virtual bool supportsRates() const { return false; }
    virtual bool moveAxes(double azRateDegS, double elRateDegS, std::string& err) { (void)azRateDegS; (void)elRateDegS; err = "this mount cannot move at a set speed"; return false; }
};

std::unique_ptr<MountDriver> makeRotctldDriver(const std::string& host, int port);
std::unique_ptr<MountDriver> makeAlpacaDriver(const std::string& host, int port, int deviceNumber = 0);
// The driver for cfg.protocol; nullptr for GS-232 (handled by Rotator itself) or no link.
std::unique_ptr<MountDriver> makeMountDriver(const RotatorConfig& cfg);

// One-shot check for the Test button and scripts: connect, read a position, disconnect. Fills msg with a line for the user.
bool probeMount(const RotatorConfig& cfg, MountPos& pos, std::string& msg);

// Alpaca discovery (UDP broadcast "alpacadiscovery1"): hosts that answered, as "ip:port". targetAddr/udpPort are overridable for tests.
std::vector<std::string> alpacaDiscover(int timeoutMs = 1500, const std::string& targetAddr = "255.255.255.255", int udpPort = 32227);

// ---- pure helpers (unit tested) ----
std::string rotctldSetPosCmd(double azDeg, double elDeg);                 // "P 123.000 45.000\n"
bool parseRprt(const std::string& line, int& code);                       // "RPRT 0" -> code 0
bool parseRotctldPos(const std::string& l1, const std::string& l2, double& az, double& el);
bool alpacaParseResponse(const std::string& json, std::string& valueText, std::string& err);   // false on ErrorNumber != 0 or no JSON
// Speed for one axis of a tracking mount: the target's own rate plus a correction of the pointing error, clamped to +-maxRate.
// errDeg = wanted - actual (signed, azimuth already wrapped to +-180). Inside deadbandDeg only the target's rate is used.
double mountRateForError(double errDeg, double targetRateDegS, double maxRateDegS, double gain = 0.8, double deadbandDeg = 0.05);

} // namespace dect2
