#include "app/http/http_parse.hpp"
#include <cstdlib>
#include <cstring>
#include <cctype>

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
    // Same reasoning as the header check below: the request line is written
    // back out verbatim by forward_request, so a control character in the
    // target would inject a line of its own upstream. A request-target is
    // VCHAR only.
    for (unsigned char ch : target) if (ch < 0x21 || ch == 0x7f) return Parse::Bad;
    size_t q = target.find('?'); r.path = target.substr(0, q); if (q != std::string::npos) r.query = target.substr(q + 1);
    // AP30: dot-segments in the PATH (the query may legitimately contain them).
    //
    // Machino's own routes are exact string matches, so `..` cannot reach one.
    // Everything else is relayed verbatim, and busybox httpd does reject `..`
    // and its percent-encoded form - measured: both come back 400. But that
    // makes the property the UPSTREAM's, not ours, and it would quietly stop
    // holding the day something else sits behind the relay or a native route
    // ever touches a path. Rejected here so it does not depend on that.
    if (r.path.find("..") != std::string::npos) return Parse::Bad;
    for (size_t i = 0; i + 2 < r.path.size(); ++i)
        if (r.path[i] == '%' && r.path[i + 1] == '2' && (r.path[i + 2] | 0x20) == 'e') return Parse::Bad;
    for (char c : r.method) if (c < 'A' || c > 'Z') return Parse::Bad;
    r.keep_alive = (version == "HTTP/1.1");
    p = nl + 2;
    while (p < end) {
        size_t e = buf.find("\r\n", p); if (e == std::string::npos || e > end) e = end;
        std::string h = buf.substr(p, e - p); p = e + 2;
        // AP30: a header line is split on CRLF, so a BARE LF inside it survives
        // as part of the value - and forward_request writes headers back out
        // verbatim. "X-Foo: a\nContent-Length: 99" therefore reached busybox as
        // two headers, one of which Machino never accounted for: header
        // smuggling into the CGI layer. Demonstrated before this check existed.
        //
        // No control character belongs in a field line at all (RFC 7230 3.2:
        // field-value is VCHAR / SP / HTAB), so the whole class goes at once
        // rather than just the LF that was found.
        for (unsigned char ch : h)
            if (ch < 0x20 && ch != '\t' && ch != '\r') return Parse::Bad;
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

namespace {
// Iterate header lines (CRLF or bare LF, mixed) between the status line and the
// terminator, calling `fn(lower_name, raw_line)` for each field line.
template <typename Fn>
void for_each_header_line(const std::string& head, size_t end, Fn fn) {
    size_t pos = 0;
    bool first = true;
    while (pos < end) {
        size_t eol = head.find('\n', pos);
        if (eol == std::string::npos || eol > end) eol = end;
        size_t line_end = eol;
        if (line_end > pos && head[line_end - 1] == '\r') --line_end;
        const std::string line = head.substr(pos, line_end - pos);
        pos = eol + 1;
        if (first) { first = false; continue; }
        if (line.empty()) continue;
        const size_t colon = line.find(':');
        std::string name = colon == std::string::npos ? line : line.substr(0, colon);
        for (char& ch : name) ch = (char)tolower((unsigned char)ch);
        fn(name, line);
    }
}
} // namespace

bool relay_head_is_html(const std::string& head) {
    size_t sep = 0;
    const size_t end = relay_head_end(head, sep);
    if (end == std::string::npos) return false;
    bool html = false, chunked = false;
    for_each_header_line(head, end, [&](const std::string& name, const std::string& line) {
        if (name == "transfer-encoding") chunked = true;
        if (name == "content-type") {
            std::string v = line.substr(line.find(':') + 1);
            for (char& ch : v) ch = (char)tolower((unsigned char)ch);
            if (v.find("text/html") != std::string::npos) html = true;
        }
    });
    return html && !chunked;
}

std::string relay_head_with_length(const std::string& head, size_t body_len, bool keep_alive) {
    size_t sep = 0;
    const size_t end = relay_head_end(head, sep);
    if (end == std::string::npos) return head;

    // Keep the status line verbatim.
    size_t sl = head.find('\n');
    if (sl == std::string::npos) return head;
    size_t sl_end = sl;
    if (sl_end > 0 && head[sl_end - 1] == '\r') --sl_end;
    std::string out = head.substr(0, sl_end) + "\r\n";

    for_each_header_line(head, end, [&](const std::string& name, const std::string& line) {
        if (name == "content-length" || name == "connection" || name == "keep-alive"
            || name == "proxy-connection" || name == "transfer-encoding")
            return;
        out += line;
        out += "\r\n";
    });
    out += "Content-Length: " + std::to_string(body_len) + "\r\n";
    out += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    return out;
}

std::string relay_head_stream_close(const std::string& head) {
    size_t sep = 0;
    const size_t end = relay_head_end(head, sep);
    if (end == std::string::npos) return head;
    size_t sl = head.find('\n');
    if (sl == std::string::npos) return head;
    size_t sl_end = sl;
    if (sl_end > 0 && head[sl_end - 1] == '\r') --sl_end;
    std::string out = head.substr(0, sl_end) + "\r\n";
    for_each_header_line(head, end, [&](const std::string& name, const std::string& line) {
        if (name == "content-length" || name == "connection" || name == "keep-alive"
            || name == "proxy-connection" || name == "transfer-encoding")
            return;
        out += line;
        out += "\r\n";
    });
    out += "Connection: close\r\n\r\n";
    return out;
}

std::string inject_machino_nav(const std::string& html, bool& changed) {
    changed = false;

    // Already there? Do not double it -- re-relaying the same page (or a proxy
    // in front) must not stack the entries.
    if (html.find("/machino/devices") != std::string::npos)
        return html;

    // The anchor is the stock Network item in System -> Setup. Match the href
    // only, so the label text (page_label renders "Network"/"Netzwerk"/...) does
    // not matter. `href="network.cgi"` and `href='network.cgi'` both occur in
    // the wild, so accept either quote.
    size_t at = html.find("href=\"network.cgi\"");
    if (at == std::string::npos)
        at = html.find("href='network.cgi'");
    if (at == std::string::npos)
        return html; // unknown layout: leave it exactly as it came

    // Insert AFTER the <li> that holds the anchor, so the new items are siblings
    // in the same dropdown list rather than nested in the Network entry. Find the
    // first </li> at or after the anchor.
    const size_t liEnd = html.find("</li>", at);
    if (liEnd == std::string::npos)
        return html; // anchor without a closing <li>: not the structure we know
    const size_t insertAt = liEnd + 5; // past "</li>"

    // Bootstrap dropdown-item markup, matching the surrounding entries. Literal
    // text (no haserl page_label here -- that is server-side and already run).
    //
    // ENGLISCH, weil die Stock-WebUI englisch ist: "Dashboard", "Live",
    // "Camera", "System", "Network", "Time", "Access". Ein deutscher Eintrag
    // mitten darin sieht nach Fremdkoerper aus -- und genau das war er.
    const std::string add =
        "\n\t\t\t\t\t\t\t<li><a class=\"dropdown-item\" href=\"/machino/devices\">Device Manager</a></li>"
        "\n\t\t\t\t\t\t\t<li><a class=\"dropdown-item\" href=\"/machino/net\">Network &amp; USB</a></li>";

    std::string out;
    out.reserve(html.size() + add.size());
    out.append(html, 0, insertAt);
    out.append(add);
    out.append(html, insertAt, std::string::npos);
    changed = true;
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

// See the header for why this is conservative. Anything it cannot frame
// exactly keeps the old close-the-connection behaviour.
size_t relay_head_end(const std::string& buf, size_t& sep_len) {
    // busybox answers static files with proper CRLF, but a CGI's own output is
    // passed through nearly verbatim and haserl emits BARE LF:
    //   HTTP/1.1 200 OK\r\nContent-type: text/html\nPragma: no-cache\n\n
    // So the head can end in either "\r\n\r\n" or "\n\n". Looking only for the
    // former made every CGI look like an endless header. Note "\r\n\r\n" does
    // not contain "\n\n", so the two never alias.
    const size_t crlf = buf.find("\r\n\r\n");
    const size_t lf   = buf.find("\n\n");
    if (crlf != std::string::npos && (lf == std::string::npos || crlf <= lf)) { sep_len = 4; return crlf; }
    if (lf != std::string::npos) { sep_len = 2; return lf; }
    sep_len = 0;
    return std::string::npos;
}

bool relay_head_keepalive(const std::string& head, std::string& out, size_t& body_len) {
    out = head;
    body_len = 0;

    // The head must be terminated; a partial head can never be judged.
    size_t sep = 0;
    const size_t end = relay_head_end(head, sep);
    if (end == std::string::npos) return false;

    // Status line: "HTTP/1.x NNN ..."
    const size_t sp = head.find(' ');
    if (sp == std::string::npos || sp + 4 > head.size()) return false;
    const int status = atoi(head.c_str() + sp + 1);
    if (status < 100) return false;

    bool have_len = false, chunked = false;
    std::string rebuilt;
    rebuilt.reserve(head.size() + 32);

    size_t pos = 0;
    bool first = true;
    while (pos < end) {
        // Lines may end in CRLF or bare LF, and a head may mix the two: busybox
        // writes the status line with CRLF and then hands the CGI's own LF
        // headers through untouched.
        size_t eol = head.find('\n', pos);
        if (eol == std::string::npos || eol > end) eol = end;
        size_t line_end = eol;
        if (line_end > pos && head[line_end - 1] == '\r') --line_end;
        const std::string line = head.substr(pos, line_end - pos);
        pos = eol + 1;
        if (first) { rebuilt += line; rebuilt += "\r\n"; first = false; continue; }
        if (line.empty()) continue;

        const size_t colon = line.find(':');
        std::string name = colon == std::string::npos ? line : line.substr(0, colon);
        for (char& ch : name) ch = (char)tolower((unsigned char)ch);

        if (name == "content-length") {
            have_len = true;
            body_len = (size_t)strtoul(line.c_str() + colon + 1, nullptr, 10);
            rebuilt += line; rebuilt += "\r\n";
            continue;
        }
        if (name == "transfer-encoding") { chunked = true; continue; }
        // Hop-by-hop: we decide these ourselves.
        if (name == "connection" || name == "keep-alive" || name == "proxy-connection") continue;
        rebuilt += line; rebuilt += "\r\n";
    }

    // Chunked upstream: we do not parse chunk framing, so we cannot know where
    // the body ends without the close.
    if (chunked) { out = head; body_len = 0; return false; }

    // 1xx, 204 and 304 carry no body by definition - a Content-Length is not
    // required for them and must not be invented.
    const bool bodyless = (status >= 100 && status < 200) || status == 204 || status == 304;
    if (bodyless) body_len = 0;
    else if (!have_len) { out = head; body_len = 0; return false; }

    rebuilt += "Connection: keep-alive\r\n\r\n";
    out = rebuilt;
    return true;
}

}} // namespace machino::http
