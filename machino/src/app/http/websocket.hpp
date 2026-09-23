// Application: minimal RFC 6455 WebSocket support for the majestic /ws/video
// route - pure functions, host-testable. Server side only: outgoing frames
// are unmasked (as the RFC requires of servers), incoming client frames are
// masked and unmasked here. Fragmented client messages are rejected: the
// stock webui sends only tiny single-frame JSON ({"request":"idr"}).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace machino { namespace ws {

// Sec-WebSocket-Accept for a client key (SHA-1 + base64 over the RFC GUID).
std::string accept_key(const std::string& sec_websocket_key);

// The complete 101 Switching Protocols response.
std::string handshake_response(const std::string& sec_websocket_key);

// One complete server frame (FIN set, unmasked). text=false -> binary.
std::string frame(bool text, const void* payload, size_t len);

// Control frames.
//
// AP33: there is no close_frame() here on purpose. It existed, was never
// called, and was removed - this server ends a WebSocket by closing the TCP
// connection (close_after_flush), which every consumer in the contract handles
// through onclose. If a proper RFC 6455 close handshake is ever wanted it is
// three lines, but it would be a behaviour change and not a cleanup.
std::string pong_frame(const std::string& ping_payload);

enum class Parse { Incomplete, Ok, Bad };
// Parses ONE client frame from `in`; on Ok, `consumed` bytes were used,
// `opcode` is the RFC opcode (1 text, 2 binary, 8 close, 9 ping, 10 pong)
// and `payload` the unmasked payload. Unmasked client frames and fragmented
// messages are Bad (the RFC requires clients to mask).
Parse parse_frame(const std::string& in, size_t& consumed, int& opcode, std::string& payload,
                  size_t max_payload = 4096);

// Exposed for tests.
void sha1(const uint8_t* data, size_t len, uint8_t out[20]);
std::string base64(const uint8_t* data, size_t len);

}} // namespace machino::ws
