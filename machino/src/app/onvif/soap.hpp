// Minimal SOAP/XML support for the ONVIF surface.
//
// This is NOT an XML parser and does not try to be one. It is a bounded,
// non-recursive scanner for the handful of things an ONVIF request actually
// has to yield: which operation was asked for, and the WS-Security token.
// Everything it does not understand it ignores, and everything dangerous it
// refuses outright.
//
// Why not a real parser: the input is unauthenticated and arrives before any
// credential has been checked, so the parser IS the attack surface. A scanner
// with no entity handling, no recursion and a hard input bound has a far
// smaller one than a general parser, and ONVIF's request shape is rigid enough
// that nothing is lost.
//
// Refused on sight:
//   - <!DOCTYPE and <!ENTITY   (entity expansion / billion laughs)
//   - anything over MAX_REQUEST bytes
#pragma once
#include <cstddef>
#include <string>

namespace machino { namespace onvif {

// An ONVIF request is a few hundred bytes. 64 KiB is already absurdly
// generous and keeps a hostile body from ever being buffered.
static const size_t MAX_REQUEST = 64 * 1024;

// True when the document is safe to look at: within bounds and free of the
// constructs this scanner deliberately cannot handle.
bool soap_acceptable(const std::string& xml);

// The operation, taken from the first element inside <...:Body>. Namespace
// prefixes are stripped, so "tds:GetDeviceInformation" yields
// "GetDeviceInformation". False when there is no Body or it is empty.
bool soap_action(const std::string& xml, std::string& action);

// Text of the first element with this LOCAL name, prefix-insensitive, with
// XML entities for the five predefined characters decoded. Returns false when
// the element is absent; an empty element yields true with an empty string.
bool element_text(const std::string& xml, const std::string& local_name, std::string& out);

// The attribute value of `attr` on the first element whose local name matches.
bool element_attr(const std::string& xml, const std::string& local_name,
                  const std::string& attr, std::string& out);

// WS-Security UsernameToken, as ONVIF clients send it.
struct WsseToken {
    bool        present = false;
    std::string username;
    std::string password;     // the <Password> text, base64 digest or cleartext
    std::string nonce_b64;
    std::string created;      // xsd:dateTime, UTC
    // PasswordDigest vs PasswordText, from the Type attribute. Absent Type
    // means PasswordText per the spec.
    bool        digest = false;
};

bool wsse_token(const std::string& xml, WsseToken& out);

// Base64 with a real failure mode: returns false on any byte outside the
// alphabet, so malformed input never becomes an accepted credential.
bool b64_decode(const std::string& in, std::string& out);

std::string xml_escape(const std::string& s);

// A complete SOAP envelope around `body_xml`, with the namespace declarations
// ONVIF clients expect.
std::string envelope(const std::string& body_xml);

// A SOAP 1.2 fault. `subcode` is an ONVIF code such as
// "ter:NotAuthorized"; `reason` is human-readable.
std::string fault(const std::string& code, const std::string& subcode,
                  const std::string& reason);

}} // namespace machino::onvif
