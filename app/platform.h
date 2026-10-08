// Operating-system services for the UI: file dialogs, saved settings, fonts.
#pragma once
#include <cstdint>
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
// Decode a picture file into RGBA8 (0xAABBGGRR, row by row): PNG everywhere, on macOS also what the system reads. False when it cannot be read.
bool decodeImage(const std::string& path, int& w, int& h, std::vector<uint32_t>& rgba);
// A folder for downloaded files (map tiles), created on demand. UTF-8.
std::string cacheDir();
// Downloads url into file (UTF-8 path) with the curl program, which Windows 10 and later, macOS and nearly every Linux have (no TLS library
// is linked). On Windows it runs without a console window. Gives up after about 15 s. False when the download did not work.
bool fetchUrl(const std::string& url, const std::string& file, const std::string& userAgent);
// The position from the system's location service (on macOS this is Wi-Fi positioning and needs the user's permission).
// locateStart() begins a request; locateState() says how it went: 0 nothing asked, 1 working, 2 done (lat, lon, accuracy in metres), 3 failed (msg says why).
void locateStart();
int locateState(double& lat, double& lon, double& accuracyM, std::string& msg);

// Paths of fonts to try, best first (proportional UI font, monospaced font)
std::vector<std::string> uiFontCandidates();
std::vector<std::string> monoFontCandidates();

}
