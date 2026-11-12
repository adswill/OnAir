// Map badges in the app (docs/MAP-BADGES-4.0.md): the Badges window (Server > Badges..., Settings > Network and updates > Map badges),
// the "Badge earned" toast, and the two hooks the map share (app/map/map_send.cpp) calls. The logic is in core/src/badges/badges_client.cpp.
#pragma once
#include <string>
struct App;

namespace badgesui {
bool& windowOpen();
// The Badges window and the toast; call once per frame.
void window(App& a);
// A menu item (menu = true) or a selectable that opens the window.
void openItem(bool menu);
// For the map post, on the UI thread: the header to add ("" = the post is sent exactly as before) and the station's locator square for range
// badges ("" = none). Both are "" unless the user is signed in and has Earn badges on; home also needs "Use my area for range".
void postExtras(App& a, std::string& authHeader, std::string& home);
// The /ingest answer of a post that carried the header (any thread): queues new badges for the toast, notes a refused sign-in.
void onIngestReply(const std::string& body);
}
