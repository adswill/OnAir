// The headless server's settings file: plain "key=value" lines (# starts a comment), so it can be written by hand and kept in /etc or in a service's
// state directory. Unknown keys and comments are kept when the program saves what it changed (mode, frequency, gain ... after the browser changed them).
// The file holds the access token, so it is written readable by its owner only.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace onair_server {

class ConfigFile {
public:
    bool parse(const std::string& text);                 // false: never (bad lines are kept as they are and ignored)
    bool load(const std::string& path);                  // false: the file is not there or not readable (the settings stay empty)
    bool save(const std::string& path, std::string& err) const;   // atomic (written beside, then renamed), mode 0600 on POSIX
    std::string text() const;
    std::string get(const std::string& key, const std::string& def = "") const;   // the last line with this key
    std::vector<std::string> getAll(const std::string& key) const;                // every line with this key, in order
    double num(const std::string& key, double def) const;
    bool flag(const std::string& key, bool def) const;   // 1 / true / yes / on
    void set(const std::string& key, const std::string& value);   // replaces the key's lines by one (the first keeps its place)
    void setNum(const std::string& key, double v);
    bool has(const std::string& key) const;
private:
    struct Line { std::string raw, key, value; bool kv = false; };
    std::vector<Line> lines_;
};

// The default place of the file: $ONAIR_SERVER_CONFIG, else ~/.config/onair/server.conf (%APPDATA%\OnAir\server.conf on Windows)
std::string defaultConfigPath();

// Host-side file helpers for the web directory's temporary body and status files (the web sources themselves never touch the disk: web_safety).
bool writeHostFile(const std::string& path, const std::string& data, std::string& err, bool atomic);   // atomic: write path.tmp then rename over path
void removeHostFile(const std::string& path);

} // namespace onair_server
