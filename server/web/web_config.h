// OnAir Web settings: the web_* keys of the server's settings file (docs/ONAIR-WEB.md lists them) to the hub's limits, the policy and the
// network server's config, and back (the admin API's policy and invite changes).
#pragma once
#include "web_directory.h"
#include "web_hub.h"
#include "web_relay.h"
#include "web_server.h"
#include <string>
#include <vector>

namespace onair_server { class ConfigFile; }

namespace onair_web {

struct WebSettings {
    bool on = false;
    WebServerConfig server;
    HubLimits limits;
    bool logIp = false;
    std::string name = "OnAir Web", location, antenna;
    DirectoryConfig directory;
    RelayConfig relay;               // web_relay=1: an outbound connection to a relay service gives this server a public address with no port forwarding
    std::vector<std::string> warnings;   // printed at start (plain HTTP on the network, missing admin token, ...)
};

// Reads the web_* keys. `madeAdminToken` is set when no admin token was configured and one was made (the caller saves the file).
WebSettings loadWebSettings(onair_server::ConfigFile& c, bool& madeAdminToken);
// Writes what the admin API can change: the policy and the invites
void saveWebState(onair_server::ConfigFile& c, const WebPolicy& p, const std::vector<InviteToken>& invites);

} // namespace onair_web
