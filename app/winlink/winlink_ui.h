// The Winlink email panel (winlink_ui.cpp): a view of the APRS / Packet and ARDOP screens.
#pragma once
struct App;

// ctx 0: the APRS / Packet screen (packet 1200 / 9600 baud and Telnet gateways), 1: the ARDOP screen (ARDOP and Telnet gateways)
void winlinkPanel(App& a, int ctx);
// every frame while either mode is selected: the session, the receivers' frames, the transmit bursts
void winlinkTick(App& a);
