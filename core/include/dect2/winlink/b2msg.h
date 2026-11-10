// Winlink B2 message format: the header lines (Mid first, then the others), a blank line, the body, then each attached file, every part
// ended by CR LF. Also the message ID (MID: 12 characters of base32 of an MD5), the date form "YYYY/MM/DD HH:MM" (UTC), the "secure
// login" answer to a gateway's ;PQ challenge, and RFC 2047 "Q" word encoding of non-ASCII titles.
//
// Follows the public B2F description (winlink.org "B2F" protocol notes) and wl2k-go's fbb package (Copyright 2015-2016 Martin Hebnes
// Pedersen LA5NTA, MIT licence), which is credited in THIRD_PARTY.md. The code is new C++.
#pragma once
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {
namespace winlink {

struct B2File {
    std::string name;
    std::vector<uint8_t> data;
};

struct B2Message {
    // header fields in the order read; names as sent ("Mid", "Date", "Type", "From", "To", "Cc", "Subject", "Mbo", "Body", "File", ...)
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;
    std::vector<B2File> files;

    std::string get(const std::string& key) const;              // first value of a field (case-insensitive name), "" when absent
    std::vector<std::string> getAll(const std::string& key) const;
    void set(const std::string& key, const std::string& value); // replaces every value of the field
    void add(const std::string& key, const std::string& value);

    std::string mid() const { return get("Mid"); }
    std::string subject() const;                                // decoded
    std::string from() const { return get("From"); }
    std::vector<std::string> to() const { return getAll("To"); }
    std::string date() const { return get("Date"); }
    std::string bodyText() const { return std::string(body.begin(), body.end()); }
};

// A new private message with Mid, Date, Type, From, Mbo, To, Subject, Body (Content-Type text/plain ISO-8859-1, 8bit) and the files.
// Lines of the body are sent with CR LF; a body given with LF only is converted.
B2Message makeMessage(const std::string& from, const std::vector<std::string>& to, const std::vector<std::string>& cc, const std::string& subject,
                      const std::string& body, const std::vector<B2File>& files, std::time_t when);

// The bytes of a message: Mid first, the other fields sorted by name (as wl2k-go writes them), CR LF, body, then the files.
// Body and File fields are set from the actual sizes.
std::vector<uint8_t> encodeMessage(const B2Message& m);
// Parses message bytes. False with err when the header or a section length is wrong.
bool parseMessage(const uint8_t* d, size_t n, B2Message& m, std::string* err = nullptr);

// MID: base32 (RFC 4648) of MD5("<time>-<callsign>"), first 12 characters
std::string generateMid(const std::string& callsign, std::time_t when, uint32_t salt = 0);
// "YYYY/MM/DD HH:MM" in UTC
std::string b2Date(std::time_t when);

// The answer to a gateway's secure login challenge (";PQ: <challenge>"): eight digits from the MD5 of challenge + password + the
// published Winlink salt
std::string secureLoginResponse(const std::string& challenge, const std::string& password);

void md5(const uint8_t* d, size_t n, uint8_t out[16]);

// RFC 2047 Q encoding (UTF-8) of a title when it has bytes outside printable ASCII; plain titles are returned unchanged
std::string qEncodeTitle(const std::string& title);
std::string qDecodeTitle(const std::string& title);

} // namespace winlink
} // namespace dect2
