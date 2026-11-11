// The Store tab of View > Plugins (docs/PLUGIN-MARKETPLACE-3.9.md): browse a signed plugin registry, install, update and remove. Nothing is fetched
// until the user presses Refresh / Check for updates (or Install, which downloads one file).
#pragma once
#include <functional>
#include <string>
namespace dect2::plugin { class Manager; }
struct App;
// Draws the tab. stopIfActive(id) must stop the plugin mode if `id` is the one running (called on the UI thread before a plugin is replaced, removed or switched off).
// Returns true when the set of installed plugins changed (the caller refreshes the mode list).
bool pluginStoreTab(App& a, dect2::plugin::Manager& mgr, const std::function<void(const std::string&)>& stopIfActive);
// Joins the background task; call before the plugin manager is destroyed.
void pluginStoreShutdown();
