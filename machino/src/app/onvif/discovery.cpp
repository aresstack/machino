#include "app/onvif/discovery.hpp"
#include "app/onvif/soap.hpp"
#include "app/http/websocket.hpp"     // ws::sha1

#include <cstdio>
#include <cstring>

namespace machino { namespace onvif {

namespace {

// RFC 4122 §4.3-ish: a name-based UUID, but with SHA-1 truncated rather than
// MD5. The point here is stability and distinctness across cameras, not
// interoperability with another namespace's generator, so the version and
// variant nibbles are set and nothing else is claimed.
std::string uuid_from_digest(const uint8_t d[20]) {
    uint8_t b[16];
    memcpy(b, d, 16);
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x50);   // version 5, name-based SHA-1
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);   // RFC 4122 variant
    char out[40];
    snprintf(out, sizeof out,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return out;
}

const char* const NS =
    " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
    " xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
    " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"";

const char* const TO_DISCOVERY =
    "urn:schemas-xmlsoap-org:ws:2005:04:discovery";

std::string header(const char* action, const std::string& msg_id,
                   const std::string& relates_to, bool to_discovery) {
    std::string h = "<s:Header>";
    h += "<a:MessageID>" + xml_escape(msg_id) + "</a:MessageID>";
    if (to_discovery) h += std::string("<a:To>") + TO_DISCOVERY + "</a:To>";
    h += std::string("<a:Action>") + action + "</a:Action>";
    if (!relates_to.empty()) h += "<a:RelatesTo>" + xml_escape(relates_to) + "</a:RelatesTo>";
    h += "</s:Header>";
    return h;
}

// The <d:ProbeMatch>/<d:Hello> payload, which is the same shape in all three
// messages.
std::string match_body(const Announcement& a) {
    char mv[32];
    snprintf(mv, sizeof mv, "%u", a.metadata_version);
    return "<a:EndpointReference><a:Address>" + xml_escape(a.uuid) + "</a:Address></a:EndpointReference>"
           "<d:Types>dn:" + std::string(NVT_TYPE) + "</d:Types>"
           "<d:Scopes>" + xml_escape(a.scopes) + "</d:Scopes>"
           "<d:XAddrs>" + xml_escape(a.xaddr) + "</d:XAddrs>"
           "<d:MetadataVersion>" + mv + "</d:MetadataVersion>";
}

std::string envelope_with(const std::string& hdr, const std::string& body) {
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?><s:Envelope") + NS + ">" +
           hdr + "<s:Body>" + body + "</s:Body></s:Envelope>";
}

} // namespace

bool parse_probe(const std::string& datagram, Probe& out) {
    out = Probe();
    if (!soap_acceptable(datagram)) return false;
    std::string action;
    if (!soap_action(datagram, action) || action != "Probe") return false;
    // A Probe with no MessageID cannot be answered: the reply has to relate to
    // it, and a client that gets an unrelated ProbeMatches discards it.
    if (!element_text(datagram, "MessageID", out.message_id) || out.message_id.empty())
        return false;
    element_text(datagram, "Types", out.types);
    element_text(datagram, "Scopes", out.scopes);
    out.valid = true;
    return true;
}

bool probe_matches_us(const Probe& p) {
    if (!p.valid) return false;
    // Absent or empty Types is "anything", which is what most discovery tools
    // send first.
    std::string t = p.types;
    size_t a = t.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return true;
    // Prefix-insensitive: the client picks its own prefix for the ONVIF
    // network namespace, so only the local name can be compared.
    return t.find(NVT_TYPE) != std::string::npos;
}

std::string device_uuid(const std::string& seed) {
    // A fixed salt so the value is not simply the hash of a serial number that
    // may also be printed elsewhere.
    const std::string material = "machino-onvif-endpoint:" + seed;
    uint8_t d[20];
    ws::sha1((const uint8_t*)material.data(), material.size(), d);
    return "urn:uuid:" + uuid_from_digest(d);
}

std::string message_id(uint64_t counter, int64_t unix_s) {
    char seed[64];
    snprintf(seed, sizeof seed, "%llu:%lld", (unsigned long long)counter, (long long)unix_s);
    uint8_t d[20];
    ws::sha1((const uint8_t*)seed, strlen(seed), d);
    return "urn:uuid:" + uuid_from_digest(d);
}

std::string probe_matches(const Announcement& a, const std::string& relates_to,
                          const std::string& msg_id) {
    return envelope_with(
        header("http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches",
               msg_id, relates_to, /*to_discovery=*/false),
        "<d:ProbeMatches><d:ProbeMatch>" + match_body(a) + "</d:ProbeMatch></d:ProbeMatches>");
}

std::string hello(const Announcement& a, const std::string& msg_id) {
    return envelope_with(
        header("http://schemas.xmlsoap.org/ws/2005/04/discovery/Hello", msg_id, "", true),
        "<d:Hello>" + match_body(a) + "</d:Hello>");
}

std::string bye(const Announcement& a, const std::string& msg_id) {
    return envelope_with(
        header("http://schemas.xmlsoap.org/ws/2005/04/discovery/Bye", msg_id, "", true),
        "<d:Bye>" + match_body(a) + "</d:Bye>");
}

}} // namespace machino::onvif
