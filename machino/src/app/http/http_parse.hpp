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

// Insert Machino's two navigation entries into a relayed OpenIPC WebUI page,
// WITHOUT touching any file on the camera.
//
// The stock header.cgi is a haserl script with a HARDCODED menu list and no
// extension slot, so the only place to add a link and still leave /var/www
// byte-identical is here, in the response Machino already relays as the
// front door. The nav is server-side-included into every full page, so this
// works on the rendered HTML, not on a separately-fetched header partial.
//
// Anchored on the existing System -> Setup "network.cgi" item: the two links
// are inserted right after that <li>, so they land inside the same dropdown.
// Idempotent (a page that already carries "/machino/devices" is returned
// unchanged) and conservative: if the anchor is not present the input is
// returned verbatim and `changed` is false -- never a heuristic cut that could
// corrupt HTML from a future WebUI.
std::string inject_machino_nav(const std::string& html, bool& changed);

// True when a relayed head is a plain HTML page safe to buffer for injection:
// Content-Type text/html and NOT chunked (we do not parse chunk framing). Query
// only, never rewrites.
bool relay_head_is_html(const std::string& head);

// Rebuild a relayed head to declare exactly `body_len` bytes with the given
// connection disposition. Drops any existing Content-Length and hop-by-hop
// headers (Connection/Keep-Alive/Transfer-Encoding/Proxy-Connection). Used after
// the body was buffered and rewritten, where the CGI head carried no length.
std::string relay_head_with_length(const std::string& head, size_t body_len, bool keep_alive);

// Rebuild a relayed head for a body we will REWRITE while streaming: drop any
// Content-Length (it would be wrong after injection) and hop-by-hop headers,
// and force Connection: close so the end is framed by the socket close. Used for
// the menu-injection path, where the length is not known up front and the page
// is delivered close-framed exactly as the stock CGI already was.
std::string relay_head_stream_close(const std::string& head);

const char* status_text(int status);
std::string response(int status, const std::string& content_type, const std::string& body, bool keep_alive,
                     const std::string& extra_headers = "");
std::string sse_headers();
std::string sse_event(const std::string& type, const std::string& data);
std::string mjpeg_headers(const std::string& boundary);
std::string mjpeg_frame(const std::string& boundary, const uint8_t* data, size_t len);

}} // namespace machino::http
