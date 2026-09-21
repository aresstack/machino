// STUN for ICE-lite (RFC 5389/8445, responder only): the camera never sends
// checks, it answers the browser's Binding Requests on the media socket with
// XOR-MAPPED-ADDRESS + MESSAGE-INTEGRITY (short-term credential = our
// ice-pwd) + FINGERPRINT. Pure byte work over caller-supplied buffers.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace machino { namespace webrtc {

// True when the datagram is STUN (first two bits 0, magic cookie present).
bool is_stun(const uint8_t* p, size_t n);

struct StunRequest {
    bool ok = false;
    uint8_t  tid[12] = {};       // transaction id
    std::string username;        // USERNAME attribute verbatim ("ourUfrag:theirUfrag")
    bool use_candidate = false;
    bool integrity_ok = false;   // MESSAGE-INTEGRITY verified against `pwd`
};

// Parses a Binding Request and verifies its MESSAGE-INTEGRITY with our
// ice-pwd. Only type 0x0001 yields ok.
StunRequest parse_binding_request(const uint8_t* p, size_t n, const std::string& pwd);

// Success response: XOR-MAPPED-ADDRESS(peer ip4/port) + MESSAGE-INTEGRITY(pwd)
// + FINGERPRINT.
std::vector<uint8_t> binding_response(const uint8_t tid[12], uint32_t peer_ip4,
                                      uint16_t peer_port, const std::string& pwd);

// Shared primitives (also the SRTP auth tag later).
void hmac_sha1(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len, uint8_t out[20]);
uint32_t crc32(const uint8_t* p, size_t n);

}} // namespace machino::webrtc
