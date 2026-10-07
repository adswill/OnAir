// Operating-system services for the UI: file dialogs, saved settings, fonts.
#pragma once
#include <string>
#include <vector>

namespace plat {

// A message the user must see before the program exits (a message box on Windows, standard error elsewhere).
void showFatalError(const char* title, const char* text);

std::string openFileDialog();
std::string saveFileDialog(const char* defaultName);

// Saved settings. Keys are short names; values are stored per platform (NSUserDefaults on macOS, a text file elsewhere).
struct Channel { double freqMhz = 0, bwMhz = 8; std::string name, mode; float snrDb = 0; int nServices = 0; bool favourite = false; };

struct Prefs {
    virtual ~Prefs() {}
    virtual bool has(const char* key) = 0;
    virtual double getD(const char* key, double def) = 0;
    virtual long getI(const char* key, long def) = 0;
    virtual bool getB(const char* key, bool def) = 0;
    virtual std::string getS(const char* key, const std::string& def) = 0;
    virtual void setD(const char* key, double v) = 0;
    virtual void setI(const char* key, long v) = 0;
    virtual void setB(const char* key, bool v) = 0;
    virtual void setS(const char* key, const std::string& v) = 0;
    virtual std::vector<Channel> getChannels() = 0;
    virtual void setChannels(const std::vector<Channel>& c) = 0;
    virtual void flush() {}
};
Prefs& prefs();

// Pictures and position (the ADS-B map).
// Decode a PNG or JPEG file into RGBA8 (0xAABBGGRR, row by row). False when the file cannot be read or the system has no decoder.
bool decodeImage(const std::string& path, int& w, int& h, std::vector<uint32_t>& rgba);
// A folder for downloaded files (map tiles), created on demand.
std::string cacheDir();
// The position from the system's location service (on macOS this is Wi-Fi positioning and needs the user's permission).
// locateStart() begins a request; locateState() says how it went: 0 nothing asked, 1 working, 2 done (lat, lon, accuracy in metres), 3 failed (msg says why).
void locateStart();
int locateState(double& lat, double& lon, double& accuracyM, std::string& msg);

// Paths of fonts to try, best first (proportional UI font, monospaced font)
std::vector<std::string> uiFontCandidates();
std::vector<std::string> monoFontCandidates();

}
