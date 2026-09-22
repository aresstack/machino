// WS-Discovery (the ONVIF profile of it): the reason a client can find this
// camera at all instead of having to be told its address.
//
// Message layer only - no socket. Everything that can be wrong lives here and
// is host-tested: whether a Probe is for us, what a ProbeMatches must relate
// to, and the endpoint identity. The socket is in discovery_server.
//
// The scanner from soap.hpp is reused deliberately: a Probe arrives over UDP
// from anyone on the segment, unauthenticated and unsolicited, so it is if
// anything a worse place for a real XML parser than the SOAP endpoint is.
#pragma once
#include <cstdint>
#include <string>

namespace machino { namespace onvif {

// The ONVIF device type a client probes for. A Probe with no Types at all
// matches everything and is answered; one that names a type we are not is
// ignored, because answering it would put this camera in a list it does not
// belong in.
static const char* const NVT_TYPE = "NetworkVideoTransmitter";

struct Probe {
    bool        valid = false;
    std::string message_id;   // what the reply must RelatesTo
    std::string types;        // raw <Types> text, "" when absent
    std::string scopes;       // raw <Scopes> text, "" when absent
};

// Parse an incoming datagram. Refuses anything the SOAP scanner refuses, and
// anything whose Body is not a Probe.
bool parse_probe(const std::string& datagram, Probe& out);

// Does this Probe ask for something we are? An empty or absent Types matches;
// otherwise the ONVIF NVT type (with any namespace prefix) must appear.
bool probe_matches_us(const Probe& p);

// A stable urn:uuid for this device, derived from a seed rather than stored:
// the same camera must present the same endpoint reference across restarts, and
// a state file would be one more thing that can disagree with reality. The seed
// should be something durable - serial number, MAC, board id.
std::string device_uuid(const std::string& seed);

struct Announcement {
    std::string uuid;         // "urn:uuid:..."
    std::string xaddr;        // "http://<ip>:<port>/onvif/device_service"
    std::string scopes;       // space-separated onvif:// scope items
    unsigned    metadata_version = 1;
};

// The reply to a Probe. `relates_to` is the Probe's MessageID.
std::string probe_matches(const Announcement& a, const std::string& relates_to,
                          const std::string& message_id);

// Multicast Hello (sent on start) and Bye (on clean shutdown), so clients
// learn about the camera without waiting for their next probe.
std::string hello(const Announcement& a, const std::string& message_id);
std::string bye(const Announcement& a, const std::string& message_id);

// A fresh urn:uuid for one message. Not a credential and not a nonce - it only
// has to be distinct, so it is derived from a counter and the clock rather
// than from entropy this process may not have at start-up.
std::string message_id(uint64_t counter, int64_t unix_s);

}} // namespace machino::onvif
