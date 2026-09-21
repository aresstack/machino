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

const char* status_text(int status);
std::string response(int status, const std::string& content_type, const std::string& body, bool keep_alive,
                     const std::string& extra_headers = "");
std::string sse_headers();
std::string sse_event(const std::string& type, const std::string& data);
std::string mjpeg_headers(const std::string& boundary);
std::string mjpeg_frame(const std::string& boundary, const uint8_t* data, size_t len);

}} // namespace machino::http
