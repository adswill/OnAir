// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Watchlists and alerts: lists of aircraft, ships, APRS call signs, frequencies and words; a decoded event that matches an entry raises an alert
// (desktop notification, sound, and a push through ntfy, a Telegram bot or a generic webhook the user sets up). Alerts are rate limited and can be
// muted in quiet hours. Tokens are the user's; they are never put into the history, the status text or any log line.
#pragma once
#include "dect2/automation/event.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2::watchlist {

enum class Kind { Aircraft, Ship, Aprs, Frequency, Words };
const char* kindName(Kind k);                       // "aircraft", "ship", "aprs", "frequency", "words"
bool kindFromName(const std::string& s, Kind& k);

struct List {
    std::string id;
    std::string name;
    Kind kind = Kind::Aircraft;
    bool enabled = true;
    std::vector<std::string> values;   // aircraft: hex, registration, call sign; ship: MMSI or name; aprs: call sign (without SSID: any SSID);
                                       // frequency: "162.025" (MHz), "162025 kHz", "144-146" (MHz range); words: a word or phrase. A trailing * is a prefix.
    double tolHz = 5000;               // frequency entries: how far from the frequency still counts
};

struct Channels {
    bool notify = true;                // desktop notification and the banner in the window
    bool sound = false;                // the user's choice; follows Mute
    std::string soundFile;             // empty: three beeps
    bool ntfy = false;
    std::string ntfyServer = "https://ntfy.sh";
    std::string ntfyTopic;
    std::string ntfyToken;             // optional access token
    bool telegram = false;
    std::string telegramToken;         // the bot token from BotFather
    std::string telegramChat;          // chat id or @channel
    bool webhook = false;
    std::string webhookUrl;
    std::string webhookToken;          // optional, sent as "Authorization: Bearer"
};

struct Settings {
    bool enabled = true;
    Channels ch;
    int maxPerHour = 30;               // all alerts together, 0 = no limit
    int cooldownSec = 300;             // the same entry on the same station again: at most this often
    bool quietOn = false;
    int quietStartMin = 22 * 60;       // minutes after midnight, local time
    int quietEndMin = 7 * 60;          // may be before the start (over midnight)
    int tzOffsetSec = 0;               // local time = UTC + this
};

struct Hit {
    double time = 0;
    std::string list;                  // name of the list
    std::string kind;
    std::string matched;               // the entry that matched
    std::string type;                  // event type
    std::string mode;
    std::string ident;                 // who: hex, MMSI, call sign ...
    std::string summary;
    bool delivered = false;
    std::string why;                   // not delivered: "quiet hours", "rate limit", "repeat"
    std::string result;                // what the channels did ("notify, sound, ntfy: sent in the background"); never holds a token or an address
};

// A request for the web service of a push channel (the sink sends it; the headers hold the token).
struct Request {
    std::string label;                 // "ntfy", "telegram", "webhook"
    std::string url;
    std::string contentType;
    std::string body;
    std::vector<std::string> headers;  // "Name: value"
};

class Sink {
public:
    virtual ~Sink() = default;
    virtual void notify(const std::string& title, const std::string& body) = 0;
    virtual void sound(const std::string& file) = 0;
    virtual void post(const Request& r) = 0;     // must not block for long: queue it
};

// ---- the parts, public so the tests can reach them ----
// Does the list match the event? matched = the entry, ident = who (for the repeat check).
bool matchList(const List& l, const automation::Event& ev, std::string& matched, std::string& ident);
// Call signs, hex addresses and names compared without case, spaces and dashes; a trailing * on the entry is a prefix.
std::string normId(const std::string& s);
bool inQuietHours(const Settings& s, double unixTime);
// Push requests; false and err when the channel is not set up properly (bad address, odd characters in a token ...).
bool buildNtfy(const Channels& c, const std::string& title, const std::string& body, Request& out, std::string& err);
bool buildTelegram(const Channels& c, const std::string& title, const std::string& body, Request& out, std::string& err);
bool buildWebhook(const Channels& c, const Hit& h, const automation::Event& ev, Request& out, std::string& err);
// Text form of the lists and settings (JSON), for the file in the data folder.
std::string toJson(const std::vector<List>& lists, const Settings& s);
bool fromJson(const std::string& text, std::vector<List>& lists, Settings& s, std::string& err);
// Entries from text: one per line or separated by commas/semicolons; blanks removed, duplicates dropped, at most 500.
std::vector<std::string> splitValues(const std::string& text);
std::string joinValues(const std::vector<std::string>& v);

class Watcher {
public:
    Watcher();
    ~Watcher();
    void setLists(const std::vector<List>& l);
    std::vector<List> lists() const;
    void setSettings(const Settings& s);
    Settings settings() const;
    void setSink(Sink* s);
    void setClock(std::function<double()> now);    // tests

    // For the decoder's thread: queues the event (cheap, never blocks); a worker thread matches and delivers. Events nobody watches for are dropped at once.
    void submit(const automation::Event& ev);
    void start();
    void stop();
    // Matches and delivers on the caller's thread (tests, and the worker's own work).
    int process(const automation::Event& ev);
    // A test alert through every channel that is switched on (ignores quiet hours and the rate limit).
    void sendTest();

    std::vector<Hit> history() const;              // oldest first, the last 300
    void clearHistory();
    uint64_t alertsSent() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

Watcher& global();

}  // namespace dect2::watchlist
