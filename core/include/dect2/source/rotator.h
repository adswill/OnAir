// Antenna rotator control (GS-232 / Yaesu-compatible ASCII). One rotator at a time: serial or TCP (e.g. PstRotator).
// Off until connected. A worker thread sends commands and polls position; the UI reads mutex-protected status.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace dect2 {

enum class RotatorLink { None = 0, Serial, Tcp };
// What the TCP link speaks: GS-232 ASCII (PstRotator, Arduino rotators), Hamlib rotctld, or ASCOM Alpaca HTTP (see mount_driver.h).
enum class RotatorProtocol { Gs232 = 0, Rotctld, Alpaca };

struct RotatorConfig {
    RotatorLink link = RotatorLink::None;
    std::string serialDevice;   // e.g. /dev/tty.usbserial-1410 or COM3
    int baud = 9600;
    std::string host = "127.0.0.1";
    int tcpPort = 4533;         // rotctld default 4533; Alpaca default 11111
    RotatorProtocol protocol = RotatorProtocol::Gs232;   // Rotctld and Alpaca need link = Tcp
    int alpacaDevice = 0;       // Alpaca telescope device number
};

// North correction and optional travel limits (mechanical degrees). Park and user commands use corrected (true) az/el.
struct RotatorCalibration {
    double azOffsetDeg = 0;     // corrected_az = norm(mechanical_az + azOffsetDeg)
    double elOffsetDeg = 0;     // corrected_el = mechanical_el + elOffsetDeg
    bool limitAz = false;
    double azMin = 0, azMax = 360;
    bool limitEl = false;
    double elMin = 0, elMax = 90;
    double parkAz = 0, parkEl = 0;   // corrected park position
    bool shortestPathAz = true;
};

// Offsets so mechanical readings match a known aim: corrected = mechanical + offset.
double rotatorAzOffsetForTrueBearing(double trueAzDeg, double mechanicalAzDeg);
double rotatorElOffsetForTrueElevation(double trueElDeg, double mechanicalElDeg);

struct RotatorStatus {
    bool connected = false;
    bool moving = false;
    double azDeg = 0;      // mechanical azimuth, 0 .. 360
    double elDeg = 0;      // mechanical elevation
    double azCorrectedDeg = 0, elCorrectedDeg = 0;
    bool azValid = false, elValid = false;
    double targetAz = 0, targetEl = 0;   // mechanical targets last commanded
    bool hasTarget = false;
    std::string lastError;
};

class Rotator {
public:
    static Rotator& get();
    ~Rotator();
    // Connects or disconnects as cfg.link says. False when the port cannot be opened (see status().lastError).
    bool configure(const RotatorConfig& cfg);
    RotatorConfig config() const;
    void setCalibration(const RotatorCalibration& cal);
    RotatorCalibration calibration() const;
    void disconnect();
    void stop();                        // send stop (C)
    void requestPoll();                 // read position now (; / V)
    // Corrected (true/user) azimuth and elevation; applies offset, limits and shortest-path before W.
    void gotoAzEl(double correctedAzDeg, double correctedElDeg);
    void gotoMechanicalAzEl(double azDeg, double elDeg);   // calibration sweeps: no offset / limits applied
    void park();
    // Follow a moving target (corrected az/el, plus its speed in deg/s). Mounts that take a speed (Alpaca MoveAxis) are driven by a
    // rate loop; others get a goto. Speeds expire after 3 s unless refreshed, so a stalled app never leaves the mount running away.
    void trackAzEl(double correctedAzDeg, double correctedElDeg, double azRateDegS, double elRateDegS);
    bool supportsRates() const;
    RotatorStatus status() const;
    bool connected() const { return connected_.load(std::memory_order_relaxed); }
    // JSON for GET /api/v1/rotator (mechanical + corrected, moving, link).
    std::string apiStatusJson() const;
private:
    Rotator();
    struct Impl;
    std::unique_ptr<Impl> p_;
    mutable std::atomic<bool> connected_{false};
};

} // namespace dect2
