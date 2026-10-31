// Server > OnAir Web: the two-button setup. "Share my radio" starts onair-server --web and a Cloudflare quick tunnel (no account) and shows the
// public https address; "List on the public directory" shows exactly what the directory will receive, and after one confirm turns the listing on.
// Everything runs as child programs with logs under <data>/webshare/, so closing OnAir ends the sharing. Docs: docs/ONAIR-WEB.md.
#pragma once
#include <string>

struct App;

namespace webshare {

// Pure helpers (tests/webshare/test_webshare.cpp)
std::string parseTunnelUrl(const std::string& log);    // the https://*.trycloudflare.com address in cloudflared's output, or ""
std::string parseAckCode(const std::string& log);      // the code after "web_directory_ack=" in the server's output, or ""
std::string parsePreview(const std::string& log);      // the server's printed preview text (what the directory would receive), or ""
bool cleanLine(const std::string& s, size_t maxLen);   // single-line printable text, at most maxLen bytes (name, location, radio)
std::string tunnelAdvice(const std::string& log);      // what to tell the owner when the tunnel program did not come up, from its log ("" if nothing specific)
int freeLocalPort(int start);                          // first port from `start` that can be listened on (127.0.0.1), 0 if none (webshare_net.cpp)
std::string lanAddress();                              // this computer's address on the local network, "" if none

// The Server menu / popup items (menu = inside a menu, else inside the Server popup): one entry that opens the "Share on web" window, with a one-line status
// (the window itself is webshare.cpp; the choices and the step 2 state machine are webshare_core.h)
void drawItems(App& a, bool menu);
// Modal windows (download consent, listing preview); call once per frame beside the other Server popups
void drawPopups(App& a);
// Ends the child programs (app shutdown, Stop button)
void stopAll();

} // namespace webshare
