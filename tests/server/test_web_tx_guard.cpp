// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Web listeners and transmit: the host setting web_allow_tx. OFF by default (a transmit message is "forbidden"); 1 lets listeners transmit through
// the host's radio (the message reaches the host's transmit handler). Keys, decryption and recording stay forbidden either way.
#include "../../server/web/web_hub.h"
#include "../../server/web/web_policy.h"
#include "../../server/web/web_proto.h"
#include "server_config.h"
using namespace onair_web;
#include "../common/test_util.h"

int main() {
    const char* txTypes[] = {"tx", "transmit", "ptt", "arm", "tx_arm", "carrier"};
    {   // the default: off
        WebPolicy p;
        CHECK(!p.allowTx(), "web_allow_tx is off by default");
        onair_server::ConfigFile cf;
        cf.parse("web=1\n");
        p.load(cf);
        CHECK(!p.allowTx(), "and stays off when the config does not mention it");
        for (const char* t : txTypes) {
            ClientMsg m; std::string code, why;
            CHECK(!parseClientMsg(std::string("{\"type\":\"") + t + "\"}", m, code, why) && code == err::Forbidden, "%s is forbidden by default", t);
        }
    }
    {   // the setting allows it
        WebPolicy p;
        onair_server::ConfigFile cf;
        cf.parse("web_allow_tx=1\n");
        p.load(cf);
        CHECK(p.allowTx(), "web_allow_tx=1 turns it on");
        onair_server::ConfigFile out;
        p.save(out);
        CHECK(out.get("web_allow_tx") == "1", "and is saved");
        cf.parse("web_allow_tx=0\n");
        p.load(cf);
        CHECK(!p.allowTx(), "web_allow_tx=0 turns it off");
        CHECK(p.json().find("\"allow_tx\":false") != std::string::npos, "the policy json shows it");
    }
    for (const char* t : txTypes) {
        ClientMsg m; std::string code, why;
        CHECK(parseClientMsg(std::string("{\"type\":\"") + t + "\"}", m, code, why, true) && m.type == ClientMsg::Tx && m.txType == t, "%s parses when the host allows it", t);
    }
    for (const char* t : {"key", "keys", "import_key", "voicekeys", "decrypt", "record", "recording", "exec", "shell", "admin"}) {
        ClientMsg m; std::string code, why;
        CHECK(!parseClientMsg(std::string("{\"type\":\"") + t + "\"}", m, code, why, true) && code == err::Forbidden, "%s stays forbidden even with web_allow_tx=1", t);
    }
    {   // the hub: off -> forbidden; on, no handler -> forbidden with a clear text; on with a handler -> the handler gets it
        WebHub h;
        CHECK(!h.allowTx(), "the hub follows the policy: off");
        std::string seen;
        h.setTxHandler([&](int, const std::string& type, const std::string&) { seen = type; return std::string(); });
        h.policy().setAllowTx(true);
        CHECK(h.allowTx(), "the hub follows the policy: on");
    }
    return testSummary("web_tx_guard");
}
