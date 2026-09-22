// Application: minimal HTTP/1.1 request parser and response builder (pure
// functions, host-testable). Bounded: header block, header count, body size.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace machino { namespace http {

struct Request {
    std::string method;
    std::string path;      // without query
    std::string query;
    std::vector<std::pair<std::string, std::string>> headers;   // names lower-cased
    std::string body;
    bool keep_alive = true;
    std::string header(const std::string& lower_name) const;
};

enum class Parse { Incomplete, Ok, Bad, TooLarge };

struct Limits { size_t max_head = 8192; size_t max_headers = 32; size_t max_body = 8192; };

// Parses one request from `buf`. On Ok, `consumed` bytes are used.
Parse parse_request(const std::string& buf, size_t& consumed, Request& out, const Limits& lim = Limits());

// Rebuild a parsed request as raw HTTP/1.0 bytes to forward to the internal
// OpenIPC WebUI (busybox httpd). Front-door mode: Machino serves the Majestic
// routes and relays everything else here. HTTP/1.0 + Connection: close makes
// the upstream response EOF-delimited (no chunked parsing). Hop-by-hop headers
// are dropped; Host is set to `upstream_host`; Authorization/Cookie pass
// through so the SAME OpenIPC login still applies.
std::string forward_request(const Request& req, const std::string& upstream_host);

// Decide whether a relayed upstream response head can be handed downstream on a
// KEPT-ALIVE connection, and rewrite it accordingly.
//
// The relay streams busybox's bytes through opaquely and learns that a response
// ended only when busybox closes the socket - EOF is the framing. That is why
// every relayed reply used to close the browser's connection too, which costs
// two TCP connections per asset: one measured browser page load burns ~515
// TIME_WAIT entries against this camera's budget of 512.
//
// A downstream connection may only stay open when the end of the body is
// knowable WITHOUT the close: an explicit Content-Length, or a status that
// carries no body at all. Chunked upstreams are refused because the relay does
// not parse chunk framing. Getting this wrong truncates or concatenates
// responses, so the default on any doubt is the old behaviour.
//
// Returns true when keep-alive is safe; `out` then holds the head with its
// hop-by-hop headers replaced by `Connection: keep-alive`, and `body_len` the
// exact number of body bytes to forward. Returns false otherwise, leaving
// `out` == `head` unchanged.
bool relay_head_keepalive(const std::string& head, std::string& out, size_t& body_len);

// Offset of the header terminator in a relayed upstream response, or npos.
// `sep_len` receives 4 for "\r\n\r\n" and 2 for a bare "\n\n" - busybox uses
// CRLF for static files but passes a CGI's own bare-LF headers through
// untouched, and looking only for CRLF makes every CGI look like an endless
// header (that shipped once, as a 502 on every WebUI page).
size_t relay_head_end(const std::string& buf, size_t& sep_len);

const char* status_text(int status);
std::string response(int status, const std::string& content_type, const std::string& body, bool keep_alive,
                     const std::string& extra_headers = "");
std::string sse_headers();
std::string sse_event(const std::string& type, const std::string& data);
std::string mjpeg_headers(const std::string& boundary);
std::string mjpeg_frame(const std::string& boundary, const uint8_t* data, size_t len);

}} // namespace machino::http
