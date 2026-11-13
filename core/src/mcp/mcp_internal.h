// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared by mcp_server.cpp (protocol) and mcp_tools.cpp (the tools).
#pragma once
#include "dect2/mcp/mcp.h"
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace dect2 {
namespace mcp {

struct ToolError : std::runtime_error { using std::runtime_error::runtime_error; };

struct ToolCtx {
    RemoteApiHost& host;
    Engine* eng;
    Options opt;
};

struct Tool {
    std::string name, title, description;
    Json schema;                 // JSON Schema of the arguments
    bool readOnly = false;       // annotations (MCP 2025-03-26)
    bool destructive = false;
    bool idempotent = false;
    bool openWorld = false;
    bool needsTx = false;        // listed and callable only when Options::allowTx
    std::function<Json(ToolCtx&, const Json&)> fn;
};

const std::vector<Tool>& allTools();
Json hostJson(const std::string& text);          // parses a host's JSON answer; null on failure
std::string guideText();                          // onair://guide: how an assistant should use the tools
Json modesTable(ToolCtx& c);                      // [{number, id, name, sub, ...}] merged from the host and the mode tables
Json resourceModeInfo(ToolCtx& c, const std::string& idOrName, bool& found);
Json bandPlanFor(double hz);

} // namespace mcp
} // namespace dect2
