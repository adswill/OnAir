// The community marketplace inside the Store tab of View > Plugins (docs/PLUGIN-MARKETPLACE-4.0.md): browse, search, sort, tags, likes,
// install with the "unreviewed" confirmation, report, and Upload plugin (GitHub sign-in, file, title, description, link, tags, preview,
// Publish). Nothing is fetched until the user opens the panel and presses Search, or acts on a plugin.
#pragma once
#include <functional>
#include <string>
namespace dect2::plugin { class Manager; }
struct App;
// Draws the panel; true when the set of installed plugins changed.
bool pluginMarketPanel(App& a, dect2::plugin::Manager& mgr, const std::function<void(const std::string&)>& stopIfActive);
// Opens the Upload plugin window the next time the panel is drawn (for a menu entry).
void pluginMarketOpenUpload();
// Joins the background task; call before the plugin manager is destroyed.
void pluginMarketShutdown();
