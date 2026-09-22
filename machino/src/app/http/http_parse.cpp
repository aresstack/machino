#include "app/http/http_parse.hpp"
#include <cstdlib>
#include <cstring>

namespace machino { namespace http {

std::string Request::header(const std::string& n) const {
    for (const auto& h : headers) if (h.first == n) return h.second;
    return "";
}

static std::string lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }
static void trim(std::string& s) { size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r"); s = (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1); }

Parse parse_request(const std::string& buf, size_t& consumed, Request& out, const Limits& lim) {
    size_t end = buf.find("\r\n\r\n");
    if (end == std::string::npos) return buf.size() > lim.max_head ? Parse::TooLarge : Parse::Incomplete;
    if (end > lim.max_head) return Parse::TooLarge;
    Request r;
    size_t p = 0, nl = buf.find("\r\n");
    std::string line = buf.substr(0, nl);
    size_t s1 = line.find(' '), s2 = line.rfind(' ');
    if (s1 == std::string::npos || s2 == s1 || line.size() > 2048) return Parse::Bad;
    r.method = line.substr(0, s1);
    std::string target = line.substr(s1 + 1, s2 - s1 - 1);
    std::string version = line.substr(s2 + 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") return Parse::Bad;
    if (target.empty() || target[0] != '/') return Parse::Bad;
    size_t q = target.find('?'); r.path = target.substr(0, q); if (q != std::string::npos) r.query = target.substr(q + 1);
    for (char c : r.method) if (c < 'A' || c > 'Z') return Parse::Bad;
    r.keep_alive = (version == "HTTP/1.1");
    p = nl + 2;
    while (p < end) {
        size_t e = buf.find("\r\n", p); if (e == std::string::npos || e > end) e = end;
        std::string h = buf.substr(p, e - p); p = e + 2;
        size_t c = h.find(':'); if (c == std::string::npos) return Parse::Bad;
        std::string name = lower(h.substr(0, c)), val = h.substr(c + 1); trim(name); trim(val);
        if (name.empty() || r.headers.size() >= lim.max_headers) return Parse::Bad;
        r.headers.emplace_back(name, val);
    }
    std::string conn = lower(r.header("connection"));
    if (conn == "close") r.keep_alive = false; else if (conn == "keep-alive") r.keep_alive = true;
    size_t body_len = 0;
    // More than one Content-Length is ambiguous, and disagreeing about which
    // one counts is the whole of request smuggling. RFC 7230 3.3.3 says reject.
    // The relay already recomputes the header from the body it actually parsed
    // and opens a fresh upstream connection per request, so nothing downstream
    // could have been desynchronised - but an ambiguous request should not be
    // answered at all.
    {
        size_t n = 0;
        for (const auto& h : r.headers) if (h.first == "content-length") ++n;
        if (n > 1) return Parse::Bad;
    }
    std::string cl = r.header("content-length");
    if (!cl.empty()) {
        // strtoull accepts a leading sign and leading space; neither is a
        // valid field-value here, and "-1" would otherwise wrap to a huge
        // number that merely trips the size check instead of being refused.
        for (char c : cl) if (c < '0' || c > '9') return Parse::Bad;
        char* ep = nullptr; unsigned long long v = strtoull(cl.c_str(), &ep, 10);
        if (ep == cl.c_str() || *ep) return Parse::Bad;
        if (v > lim.max_body) return Parse::TooLarge;
        body_len = (size_t)v;
    }
    if (!r.header("transfer-encoding").empty()) return Parse::Bad;     // chunked not supported (bounded API)
    size_t total = end + 4 + body_len;
    if (buf.size() < total) return Parse::Incomplete;
    r.body = buf.substr(end + 4, body_len);
    consumed = total;
    out = r;
    return Parse::Ok;
}

std::string forward_request(const Request& req, const std::string& upstream_host) {
    std::string target = req.path;
    if (!req.query.empty()) { target += '?'; target += req.query; }
    std::string out = req.method + " " + target + " HTTP/1.0\r\n";
    for (const auto& h : req.headers) {
        // Drop hop-by-hop and length/host headers we set ourselves; keep the
        // rest verbatim (Authorization, Cookie, Content-Type, If-Match, Accept,
        // User-Agent, X-Requested-With, ...) so haserl/CGI see the real request.
        const std::string& n = h.first; // already lower-cased
        if (n == "host" || n == "connection" || n == "keep-alive" || n == "proxy-connection" ||
            n == "transfer-encoding" || n == "upgrade" || n == "content-length" || n == "te")
            continue;
        out += h.first; out += ": "; out += h.second; out += "\r\n";
    }
    out += "Host: " + upstream_host + "\r\n";
    out += "Connection: close\r\n";
    if (!req.body.empty())
        out += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
    out += "\r\n";
    out += req.body;
    return out;
}

const char* status_text(int s) {
    switch (s) {
        case 200: return "OK"; case 204: return "No Content"; case 302: return "Found";
        case 400: return "Bad Request"; case 401: return "Unauthorized"; case 403: return "Forbidden"; case 404: return "Not Found";
        case 405: return "Method Not Allowed"; case 409: return "Conflict"; case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity"; case 500: return "Internal Server Error";
        case 501: return "Not Implemented"; case 502: return "Bad Gateway";
        case 503: return "Service Unavailable"; case 504: return "Gateway Timeout";
    }
    return "Unknown";
}

std::string response(int status, const std::string& ct, const std::string& body, bool keep_alive, const std::string& extra) {
    std::string r = "HTTP/1.1 " + std::to_string(status) + " " + status_text(status) + "\r\n";
    r += "Content-Type: " + ct + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    r += "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n";
    r += extra;
    r += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    r += "\r\n"; r += body;
    return r;
}

std::string sse_headers() {
    return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
           "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n: connected\n\n";
}

std::string sse_event(const std::string& type, const std::string& data) {
    return "event: " + type + "\ndata: " + data + "\n\n";
}

std::string mjpeg_headers(const std::string& boundary) {
    return "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=" + boundary +
           "\r\nCache-Control: no-store\r\nPragma: no-cache\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
}

std::string mjpeg_frame(const std::string& boundary, const uint8_t* data, size_t len) {
    std::string f = "--" + boundary + "\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(len) + "\r\n\r\n";
    f.append(reinterpret_cast<const char*>(data), len);
    f += "\r\n";
    return f;
}

}} // namespace machino::http
