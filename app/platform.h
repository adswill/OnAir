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

// Paths of fonts to try, best first (proportional UI font, monospaced font)
std::vector<std::string> uiFontCandidates();
std::vector<std::string> monoFontCandidates();

}
