// A small XML reader for the signaling fragments of ATSC 3.0 (SLT, S-TSID, USBD, MPD): elements, attributes, text, comments, CDATA,
// entities. Namespaces are kept as written; lookups compare the local name (the part after any prefix).
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct XmlNode {
    std::string name;                           // as written, with prefix
    std::map<std::string, std::string> attrs;
    std::string text;                           // character data directly inside, concatenated
    std::vector<std::shared_ptr<XmlNode>> children;

    std::string local() const { auto p = name.find(':'); return p == std::string::npos ? name : name.substr(p + 1); }
    std::string attr(const std::string& key, const std::string& def = std::string()) const;   // attribute by local name
    bool has(const std::string& key) const;
    std::vector<std::shared_ptr<XmlNode>> all(const std::string& localName) const;           // direct children with this local name
    std::shared_ptr<XmlNode> first(const std::string& localName) const;
};

// Parses a document; returns the root element or null on malformed input.
std::shared_ptr<XmlNode> parseXml(const std::string& text);

} // namespace dect2
