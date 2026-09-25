// HTTP parser boundary tests (pure functions).
#include "app/http/http_parse.hpp"
#include <cstdio>
#include <string>

using namespace machino::http;
extern int g_fail_ext, g_pass_ext;
#define HCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

void run_http_parse_tests() {
    Request r; size_t used = 0;
    std::string one = "GET /api/v1/state?x=1 HTTP/1.1\r\nHost: cam\r\nAccept: */*\r\n\r\n";
    HCHECK(parse_request(one, used, r) == Parse::Ok);
    HCHECK(r.method == "GET" && r.path == "/api/v1/state" && r.query == "x=1" && r.header("host") == "cam" && r.keep_alive && used == one.size());
    // POST/PATCH with body, exact consumption, pipelined remainder untouched
    std::string two = "PATCH /api/v1/config HTTP/1.1\r\nContent-Length: 9\r\nIf-Match: 3\r\n\r\n{\"a\":123}GET / HTTP/1.1\r\n\r\n";
    HCHECK(parse_request(two, used, r) == Parse::Ok && r.method == "PATCH" && r.body == "{\"a\":123}" && r.header("if-match") == "3");
    HCHECK(two.substr(used) == "GET / HTTP/1.1\r\n\r\n");
    // incomplete: head not finished / body missing
    HCHECK(parse_request("GET /x HTTP/1.1\r\nHost: a\r\n", used, r) == Parse::Incomplete);
    HCHECK(parse_request("PATCH /x HTTP/1.1\r\nContent-Length: 5\r\n\r\n{\"a", used, r) == Parse::Incomplete);
    // empty body PATCH is a complete request with empty body
    HCHECK(parse_request("PATCH /x HTTP/1.1\r\nContent-Length: 0\r\n\r\n", used, r) == Parse::Ok && r.body.empty());
    // oversized body / head
    HCHECK(parse_request("PATCH /x HTTP/1.1\r\nContent-Length: 999999\r\n\r\n", used, r) == Parse::TooLarge);
    HCHECK(parse_request(std::string("GET /x HTTP/1.1\r\nX: ") + std::string(9000, 'y'), used, r) == Parse::TooLarge);
    // malformed
    HCHECK(parse_request("GARBAGE\r\n\r\n", used, r) == Parse::Bad);
    HCHECK(parse_request("GET x HTTP/1.1\r\n\r\n", used, r) == Parse::Bad);
    HCHECK(parse_request("GET /x HTTP/2.0\r\n\r\n", used, r) == Parse::Bad);
    HCHECK(parse_request("GET /x HTTP/1.1\r\nNoColon\r\n\r\n", used, r) == Parse::Bad);
    HCHECK(parse_request("PATCH /x HTTP/1.1\r\nContent-Length: abc\r\n\r\n", used, r) == Parse::Bad);
    HCHECK(parse_request("PATCH /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", used, r) == Parse::Bad);
    HCHECK(parse_request("get /x HTTP/1.1\r\n\r\n", used, r) == Parse::Bad);       // methods are upper-case tokens
    // too many headers
    std::string many = "GET /x HTTP/1.1\r\n"; for (int i = 0; i < 40; ++i) many += "H" + std::to_string(i) + ": v\r\n"; many += "\r\n";
    HCHECK(parse_request(many, used, r) == Parse::Bad);
    // HTTP/1.0 defaults to close; Connection header overrides
    HCHECK(parse_request("GET /x HTTP/1.0\r\n\r\n", used, r) == Parse::Ok && !r.keep_alive);
    HCHECK(parse_request("GET /x HTTP/1.1\r\nConnection: close\r\n\r\n", used, r) == Parse::Ok && !r.keep_alive);
    // response / SSE framing
    std::string resp = response(422, "application/json", "{}", true);
    HCHECK(resp.rfind("HTTP/1.1 422 Unprocessable Entity\r\n", 0) == 0 && resp.find("Content-Length: 2\r\n") != std::string::npos && resp.find("\r\n\r\n{}") != std::string::npos);
    HCHECK(sse_event("lifecycle", "{\"a\":1}") == "event: lifecycle\ndata: {\"a\":1}\n\n");
    HCHECK(sse_headers().find("text/event-stream") != std::string::npos);
    HCHECK(std::string(status_text(409)) == "Conflict" && std::string(status_text(503)) == "Service Unavailable");

    // forward_request (front-door relay to the internal OpenIPC WebUI):
    // rebuilds HTTP/1.0 + Connection: close, keeps path+query, forwards
    // Authorization/Cookie, rewrites Host, drops hop-by-hop + our own length.
    {
        Request fr;
        // A /cgi-bin/j/*.cgi backend is rewritten onto the machino shim so it
        // gets the GET_/POST_ env busybox does not set; the target rides along
        // as PATH_INFO and the query is left intact.
        HCHECK(parse_request("GET /cgi-bin/j/pulse.cgi?x=1 HTTP/1.1\r\nHost: cam\r\n"
                             "Authorization: Basic Zm9v\r\nCookie: s=1\r\nConnection: keep-alive\r\n\r\n",
                             used, fr) == Parse::Ok);
        std::string w = forward_request(fr, "127.0.0.1");
        // header NAMES are lower-cased by the parser and forwarded verbatim
        // (valid HTTP; CGI reads them case-insensitively).
        HCHECK(w.rfind("GET /cgi-bin/machino-cgi-run.cgi/j/pulse.cgi?x=1 HTTP/1.0\r\n", 0) == 0);
        HCHECK(w.find("authorization: Basic Zm9v\r\n") != std::string::npos);
        HCHECK(w.find("cookie: s=1\r\n") != std::string::npos);
        HCHECK(w.find("Host: 127.0.0.1\r\n") != std::string::npos);
        HCHECK(w.find("Host: cam\r\n") == std::string::npos);          // original Host dropped
        HCHECK(w.find("Connection: close\r\n") != std::string::npos);
        HCHECK(w.find("keep-alive") == std::string::npos);              // hop-by-hop dropped
        HCHECK(w.rfind("\r\n\r\n") == w.size() - 4);                    // GET: empty body
    }
    // The j/ rewrite is narrow: only that one directory, only .cgi, one level
    // deep, and everything else relays byte-for-byte.
    {
        Request fr;
        HCHECK(parse_request("POST /cgi-bin/j/files.cgi HTTP/1.1\r\nHost: cam\r\n"
                             "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 9\r\n\r\nop=delete",
                             used, fr) == Parse::Ok);
        std::string w = forward_request(fr, "127.0.0.1");
        HCHECK(w.rfind("POST /cgi-bin/machino-cgi-run.cgi/j/files.cgi HTTP/1.0\r\n", 0) == 0);
        HCHECK(w.size() >= 9 && w.substr(w.size() - 9) == "op=delete");  // body preserved
    }
    {   // a haserl page under cgi-bin (not j/) is NOT rewritten
        Request fr;
        HCHECK(parse_request("GET /cgi-bin/network.cgi HTTP/1.1\r\nHost: cam\r\n\r\n", used, fr) == Parse::Ok);
        std::string w = forward_request(fr, "127.0.0.1");
        HCHECK(w.rfind("GET /cgi-bin/network.cgi HTTP/1.0\r\n", 0) == 0);
    }
    {   // a nested path under j/ is NOT rewritten (only one level, no slashes)
        Request fr;
        HCHECK(parse_request("GET /cgi-bin/j/sub/x.cgi HTTP/1.1\r\nHost: cam\r\n\r\n", used, fr) == Parse::Ok);
        std::string w = forward_request(fr, "127.0.0.1");
        HCHECK(w.rfind("GET /cgi-bin/j/sub/x.cgi HTTP/1.0\r\n", 0) == 0);
    }
    {   // a non-.cgi under j/ is NOT rewritten
        Request fr;
        HCHECK(parse_request("GET /cgi-bin/j/locale.txt HTTP/1.1\r\nHost: cam\r\n\r\n", used, fr) == Parse::Ok);
        std::string w = forward_request(fr, "127.0.0.1");
        HCHECK(w.rfind("GET /cgi-bin/j/locale.txt HTTP/1.0\r\n", 0) == 0);
    }
    {
        Request fr;
        HCHECK(parse_request("POST /save HTTP/1.1\r\nHost: cam\r\nContent-Type: application/json\r\n"
                             "Content-Length: 7\r\n\r\n{\"a\":1}",
                             used, fr) == Parse::Ok);
        std::string w = forward_request(fr, "127.0.0.1");
        HCHECK(w.rfind("POST /save HTTP/1.0\r\n", 0) == 0);
        HCHECK(w.find("content-type: application/json\r\n") != std::string::npos);
        HCHECK(w.find("Content-Length: 7\r\n") != std::string::npos);  // re-added from the body
        HCHECK(w.size() >= 7 && w.substr(w.size() - 7) == "{\"a\":1}");
    }

    // ---- ambiguous framing is refused, not resolved (review) ----------------
    {
        size_t used = 0;
        Request r;
        // Two Content-Length headers: disagreeing about which one counts is
        // the whole of request smuggling. RFC 7230 3.3.3 says reject.
        HCHECK(parse_request("POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
                             "Content-Length: 6\r\nConnection: close\r\n\r\nhelloX",
                             used, r) == Parse::Bad);
        // even when they agree
        HCHECK(parse_request("POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
                             "Content-Length: 5\r\nConnection: close\r\n\r\nhello",
                             used, r) == Parse::Bad);
        // a single one still works
        HCHECK(parse_request("POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello",
                             used, r) == Parse::Ok && r.body == "hello");

        // A signed or padded length is not a valid field-value. "-1" used to
        // wrap to a huge number and merely trip the size check.
        HCHECK(parse_request("POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n",
                             used, r) == Parse::Bad);
        HCHECK(parse_request("POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: +5\r\n\r\nhello",
                             used, r) == Parse::Bad);
        HCHECK(parse_request("POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 5x\r\n\r\nhello",
                             used, r) == Parse::Bad);
        HCHECK(parse_request("POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 0x5\r\n\r\nhello",
                             used, r) == Parse::Bad);
        // Transfer-Encoding was already refused outright; keep it that way
        HCHECK(parse_request("POST /x HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
                             used, r) == Parse::Bad);
    }
}

// Relayed responses may only keep the browser connection open when the end of
// the body is knowable WITHOUT the upstream close - the relay uses EOF as its
// framing and never parses chunk syntax. Getting this wrong truncates or
// concatenates replies, so every branch is pinned here.
void run_relay_keepalive_tests() {
    std::string out; size_t len = 0;

    // static asset from busybox: has Content-Length -> may stay open
    {
        const std::string head =
            "HTTP/1.0 200 OK\r\nDate: x\r\nConnection: close\r\nContent-type: application/javascript\r\n"
            "Content-Length: 14923\r\nETag: \"abc\"\r\n\r\n";
        HCHECK(relay_head_keepalive(head, out, len));
        HCHECK(len == 14923);
        HCHECK(out.find("Connection: keep-alive\r\n") != std::string::npos);
        HCHECK(out.find("Connection: close") == std::string::npos);   // the upstream's is gone
        HCHECK(out.find("Content-Length: 14923\r\n") != std::string::npos);
        HCHECK(out.find("ETag: \"abc\"\r\n") != std::string::npos);   // other headers survive
        HCHECK(out.compare(0, 15, "HTTP/1.0 200 OK") == 0);           // status line untouched
        HCHECK(out.size() >= 4 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0);
    }

    // CGI without Content-Length: EOF is the only framing -> must NOT stay open
    {
        const std::string head = "HTTP/1.0 200 OK\r\nContent-type: text/html\r\n\r\n";
        out.clear(); len = 12345;
        HCHECK(!relay_head_keepalive(head, out, len));
        HCHECK(out == head);                                          // unchanged
        HCHECK(len == 0);
    }

    // chunked: we do not parse chunk framing, so we cannot know the end
    {
        const std::string head =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n";
        out.clear(); len = 0;
        HCHECK(!relay_head_keepalive(head, out, len));
        HCHECK(out == head);
        HCHECK(len == 0);
    }

    // 304 carries no body by definition - no Content-Length needed, and none
    // may be invented. This is the common case for cached WebUI assets.
    {
        const std::string head = "HTTP/1.0 304 Not Modified\r\nETag: \"abc\"\r\nConnection: close\r\n\r\n";
        out.clear(); len = 99;
        HCHECK(relay_head_keepalive(head, out, len));
        HCHECK(len == 0);
        HCHECK(out.find("Connection: keep-alive\r\n") != std::string::npos);
        HCHECK(out.find("Content-Length") == std::string::npos);
    }
    {
        const std::string head = "HTTP/1.0 204 No Content\r\n\r\n";
        out.clear(); len = 99;
        HCHECK(relay_head_keepalive(head, out, len));
        HCHECK(len == 0);
    }

    // a redirect WITH a length is still framed exactly
    {
        const std::string head = "HTTP/1.0 302 Found\r\nLocation: /x\r\nContent-Length: 0\r\n\r\n";
        out.clear(); len = 7;
        HCHECK(relay_head_keepalive(head, out, len));
        HCHECK(len == 0);
        HCHECK(out.find("Location: /x\r\n") != std::string::npos);
    }

    // hop-by-hop headers from the upstream must never be forwarded
    {
        const std::string head =
            "HTTP/1.0 200 OK\r\nKeep-Alive: timeout=5\r\nProxy-Connection: close\r\n"
            "Content-Length: 3\r\n\r\n";
        out.clear(); len = 0;
        HCHECK(relay_head_keepalive(head, out, len));
        HCHECK(out.find("Keep-Alive: timeout") == std::string::npos);
        HCHECK(out.find("Proxy-Connection") == std::string::npos);
    }

    // garbage in, old behaviour out - never a crash, never a guess
    {
        out.clear(); len = 5;
        HCHECK(!relay_head_keepalive("", out, len));
        HCHECK(!relay_head_keepalive("HTTP/1.0 200 OK\r\nContent-Length: 5\r\n", out, len));  // unterminated
        HCHECK(!relay_head_keepalive("garbage\r\n\r\n", out, len));
        HCHECK(!relay_head_keepalive("\r\n\r\n", out, len));
        HCHECK(len == 0);
    }
}

// busybox writes CRLF for static files but hands a CGI's own BARE-LF headers
// through untouched. Taken verbatim off the camera:
//
//   static: "HTTP/1.1 200 OK\r\nDate: ...\r\nConnection: close\r\n...\r\n\r\n"
//   CGI:    "HTTP/1.1 200 OK\r\nContent-type: text/html\nPragma: no-cache\n\n"
//
// Looking only for "\r\n\r\n" made every CGI look like an endless header and
// turned the whole WebUI into 502s. That shipped once; these pin it.
void run_relay_head_end_tests() {
    size_t sep = 0;

    // CRLF head
    {
        const std::string h = "HTTP/1.1 200 OK\r\nA: b\r\n\r\nBODY";
        const size_t e = relay_head_end(h, sep);
        HCHECK(sep == 4);
        HCHECK(h.substr(e + sep) == "BODY");
    }
    // bare-LF head, as haserl emits
    {
        const std::string h = "HTTP/1.1 200 OK\r\nContent-type: text/html; charset=UTF-8\n"
                              "Cache-Control: no-store\nPragma: no-cache\n\n<!DOCTYPE html>";
        const size_t e = relay_head_end(h, sep);
        HCHECK(e != std::string::npos);
        HCHECK(sep == 2);
        HCHECK(h.substr(e + sep) == "<!DOCTYPE html>");
    }
    // a CRLF terminator must win when it comes first, and never alias with "\n\n"
    {
        const std::string h = "HTTP/1.1 200 OK\r\n\r\nx\n\ny";
        const size_t e = relay_head_end(h, sep);
        HCHECK(sep == 4);
        HCHECK(h.substr(e + sep) == "x\n\ny");
    }
    // ... and a bare-LF terminator must win when IT comes first
    {
        const std::string h = "HTTP/1.1 200 OK\nA: b\n\nbody\r\n\r\ntail";
        const size_t e = relay_head_end(h, sep);
        HCHECK(sep == 2);
        HCHECK(h.substr(e + sep) == "body\r\n\r\ntail");
    }
    // unterminated: npos, and sep cleared so a caller cannot use a stale one
    {
        sep = 7;
        HCHECK(relay_head_end("HTTP/1.1 200 OK\r\nA: b\r\n", sep) == std::string::npos);
        HCHECK(sep == 0);
        HCHECK(relay_head_end("", sep) == std::string::npos);
    }

    // The real CGI head must be judged, not choked on: no Content-Length, so it
    // keeps the old close-the-connection behaviour - but it must be RECOGNISED.
    {
        const std::string cgi = "HTTP/1.1 200 OK\r\nContent-type: text/html; charset=UTF-8\n"
                                "Cache-Control: no-store\nPragma: no-cache\n\n";
        std::string out; size_t len = 99;
        HCHECK(!relay_head_keepalive(cgi, out, len));
        HCHECK(out == cgi);
        HCHECK(len == 0);
    }
    // a mixed-ending head WITH a length is framed exactly and kept alive
    {
        const std::string mixed = "HTTP/1.1 200 OK\r\nContent-type: text/plain\nContent-Length: 42\n\n";
        std::string out; size_t len = 0;
        HCHECK(relay_head_keepalive(mixed, out, len));
        HCHECK(len == 42);
        HCHECK(out.find("Connection: keep-alive\r\n") != std::string::npos);
        HCHECK(out.find("Content-Length: 42\r\n") != std::string::npos);   // normalised to CRLF
        HCHECK(out.find("Content-type: text/plain\r\n") != std::string::npos);
    }
    // AP30: header smuggling through the relay.
    //
    // A header line is split on CRLF, so a BARE LF inside it used to survive
    // as part of the value - and forward_request writes headers back out
    // verbatim, so "X-Foo: a\nContent-Length: 99" reached busybox as TWO
    // headers, one of which Machino never accounted for. Demonstrated against
    // the real parser before the check existed.
    {
        size_t used = 0; Request r;
        HCHECK(parse_request("GET /x HTTP/1.1\r\nHost: h\r\nX-Foo: a\nContent-Length: 99\r\n\r\n",
                             used, r) == Parse::Bad);
        // every other control character too - a field-value is VCHAR/SP/HTAB
        HCHECK(parse_request("GET /x HTTP/1.1\r\nX-Foo: a\x01b\r\n\r\n", used, r) == Parse::Bad);
        // a tab is legal and must still pass
        HCHECK(parse_request("GET /x HTTP/1.1\r\nX-Foo: a\tb\r\n\r\n", used, r) == Parse::Ok);
        // and the same class in the request TARGET, which is also written back
        HCHECK(parse_request("GET /x\x01y HTTP/1.1\r\nHost: h\r\n\r\n", used, r) == Parse::Bad);
        HCHECK(parse_request("GET /ok?a=1 HTTP/1.1\r\nHost: h\r\n\r\n", used, r) == Parse::Ok);
    }
    // AP30: dot-segments in the path. busybox rejects these too (measured:
    // both forms come back 400), but that made the property the upstreams and
    // not ours.
    {
        size_t used = 0; Request r;
        HCHECK(parse_request("GET /cgi-bin/../../../etc/shadow HTTP/1.1\r\nHost: h\r\n\r\n", used, r) == Parse::Bad);
        HCHECK(parse_request("GET /%2e%2e/%2e%2e/etc/shadow HTTP/1.1\r\nHost: h\r\n\r\n", used, r) == Parse::Bad);
        HCHECK(parse_request("GET /a/%2E%2E/b HTTP/1.1\r\nHost: h\r\n\r\n", used, r) == Parse::Bad);
        // a dot inside a NAME is ordinary and must pass, and so is one in the
        // query - /api/v1/reset?key=video.bitrate is a real request
        HCHECK(parse_request("GET /a.b/c..d HTTP/1.1\r\nHost: h\r\n\r\n", used, r) == Parse::Bad);
        HCHECK(parse_request("GET /api/v1/reset?key=video.bitrate HTTP/1.1\r\nHost: h\r\n\r\n", used, r) == Parse::Ok);
        HCHECK(parse_request("GET /a/b?p=../x HTTP/1.1\r\nHost: h\r\n\r\n", used, r) == Parse::Ok);
    }

    // inject_machino_nav: add the two links into a relayed OpenIPC page without
    // touching any file. Modelled on the real header.cgi rendered output.
    {
        // A trimmed but faithful System dropdown, as haserl renders it (labels
        // already substituted for <% page_label %>).
        const std::string page =
            "<!DOCTYPE html><html><body><ul class=\"navbar-nav\">"
            "<li class=\"nav-item dropdown\"><a id=\"dropdownSystem\">System</a>"
            "<ul class=\"dropdown-menu\">"
            "<li><h6 class=\"dropdown-header\">Setup</h6></li>"
            "<li><a class=\"dropdown-item\" href=\"network.cgi\">Network</a></li>"
            "<li><a class=\"dropdown-item\" href=\"time.cgi\">Time</a></li>"
            "<li><a class=\"dropdown-item\" href=\"access.cgi\">Access</a></li>"
            "</ul></li></ul></body></html>";

        bool changed = false;
        const std::string out = inject_machino_nav(page, changed);
        HCHECK(changed);
        // All four links present, exactly once each -- one per split page.
        // NO machino-wifi.cgi: station Wi-Fi is configured on OpenIPC's own
        // network page (two-owners finding, 2026-09-25).
        for (const char* p : {"machino-usb.cgi", "machino-cellular.cgi",
                              "machino-uplinks.cgi", "machino-devices.cgi"}) {
            HCHECK(out.find(std::string("href=\"") + p + "\"") != std::string::npos);
            HCHECK(out.find(p) == out.rfind(p));
        }
        HCHECK(out.find("machino-wifi.cgi") == std::string::npos);
        // Inserted AFTER the Network item (inside Setup), before Time.
        HCHECK(out.find("machino-usb.cgi") > out.find("network.cgi"));
        HCHECK(out.find("machino-devices.cgi") < out.find("time.cgi"));
        // The stock entries are untouched and still there.
        HCHECK(out.find("href=\"network.cgi\"") != std::string::npos);
        HCHECK(out.find("href=\"time.cgi\"") != std::string::npos);
        HCHECK(out.find("href=\"access.cgi\"") != std::string::npos);

        // Idempotent: running it again changes nothing and adds no second copy.
        bool again = false;
        const std::string twice = inject_machino_nav(out, again);
        HCHECK(!again);
        HCHECK(twice == out);
        HCHECK(twice.find("machino-devices.cgi") == twice.rfind("machino-devices.cgi"));

        // Single-quoted href variant is accepted too.
        bool sqc = false;
        const std::string sqp = "<li><a class=\"dropdown-item\" href='network.cgi'>Network</a></li>";
        const std::string sqout = inject_machino_nav(sqp, sqc);
        HCHECK(sqc);
        HCHECK(sqout.find("machino-devices.cgi") != std::string::npos);

        // No anchor -> byte-identical, changed=false.
        const std::string noanchor = "<html><body><ul><li>nothing here</li></ul></body></html>";
        bool nc = true;
        const std::string same = inject_machino_nav(noanchor, nc);
        HCHECK(!nc);
        HCHECK(same == noanchor);

        // Already-integrated page (carries /cgi-bin/machino-devices.cgi) -> untouched.
        bool ic = true;
        const std::string pre = "<li><a href=\"machino-devices.cgi\">DM</a></li>"
                                "<li><a class=\"dropdown-item\" href=\"network.cgi\">Network</a></li>";
        const std::string preout = inject_machino_nav(pre, ic);
        HCHECK(!ic);
        HCHECK(preout == pre);
    }

    // inject_machino_network_cards: a native card next to "Wireless adapter"
    // on OpenIPC's network.cgi, spliced into the relayed reply. Modelled on
    // the measured page structure (row g-4 mt-0 with the adapter card and the
    // Advanced <details> as siblings).
    {
        const std::string page =
            "<html><body><main><div class=\"container\">"
            "<div class=\"row g-4 mt-0\">\n"
            "<div class=\"col-12 col-lg-6\">\n"
            "<div class=\"card h-100\" id=\"adapter\"><div class=\"card-body\">Wireless adapter</div></div>\n"
            "</div>\n"
            "<div class=\"col-12 col-lg-6\">\n"
            "<details class=\"mj-advanced\"><summary>Advanced</summary></details>\n"
            "</div>\n"
            "</div></div></main></body></html>";

        bool changed = false;
        const std::string out = inject_machino_network_cards(page, changed);
        HCHECK(changed);
        HCHECK(out.find("mchnw-card") != std::string::npos);
        // Between the adapter card and the Advanced column, as a sibling col.
        HCHECK(out.find("mchnw-card") > out.find("id=\"adapter\""));
        HCHECK(out.find("mchnw-card") < out.find("mj-advanced"));
        // Talks to machino's own API, links to the machino pages.
        HCHECK(out.find("/api/v1/usb") != std::string::npos);
        HCHECK(out.find("/api/v1/devices") != std::string::npos);
        HCHECK(out.find("machino-usb.cgi") != std::string::npos);
        HCHECK(out.find("machino-devices.cgi") != std::string::npos);
        // The stock page around it is untouched.
        HCHECK(out.find("Wireless adapter") != std::string::npos);
        HCHECK(out.find("<details class=\"mj-advanced\">") != std::string::npos);

        // Idempotent.
        bool again = true;
        const std::string twice = inject_machino_network_cards(out, again);
        HCHECK(!again);
        HCHECK(twice == out);

        // No anchor -> byte-identical, changed=false (fail-closed: an OpenIPC
        // update that moves the block loses our card, never the page).
        bool nc = true;
        const std::string plain = "<html><body>no advanced block</body></html>";
        HCHECK(inject_machino_network_cards(plain, nc) == plain);
        HCHECK(!nc);

        // Unknown structure between column wrapper and <details> (something
        // other than whitespace) -> untouched as well.
        bool uc = true;
        const std::string odd =
            "<div class=\"col-12 col-lg-6\"><b>x</b><details class=\"mj-advanced\"></details></div>";
        HCHECK(inject_machino_network_cards(odd, uc) == odd);
        HCHECK(!uc);
    }

    // relay_head_is_html: only text/html, and never a chunked body (we do not
    // parse chunk framing, so those must not be buffered for injection).
    {
        HCHECK(relay_head_is_html("HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\n\r\n"));
        HCHECK(relay_head_is_html("HTTP/1.1 200 OK\nContent-type: text/html\n\n")); // bare LF, lowercase
        HCHECK(!relay_head_is_html("HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n\r\n"));
        HCHECK(!relay_head_is_html("HTTP/1.0 200 OK\r\nContent-Type: image/png\r\n\r\n"));
        HCHECK(!relay_head_is_html("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nTransfer-Encoding: chunked\r\n\r\n"));
        HCHECK(!relay_head_is_html("HTTP/1.0 200 OK\r\n\r\n")); // no content-type
    }

    // relay_head_stream_close: drop Content-Length + hop-by-hop, force close.
    {
        const std::string in = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: 40\r\n"
                               "Connection: keep-alive\r\nCache-Control: no-store\r\n\r\n";
        const std::string out = relay_head_stream_close(in);
        HCHECK(out.find("Content-Length:") == std::string::npos);   // the length would be wrong after injection
        HCHECK(out.find("Connection: close") != std::string::npos);
        HCHECK(out.find("keep-alive") == std::string::npos);
        HCHECK(out.find("Cache-Control: no-store") != std::string::npos); // unrelated headers kept
        HCHECK(out.find("Content-Type: text/html") != std::string::npos);
        HCHECK(out.rfind("HTTP/1.0 200 OK", 0) == 0);               // status line kept, first
        HCHECK(out.size() >= 4 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0); // properly terminated
    }

    // relay_head_with_length: declare an exact length with the chosen disposition.
    {
        const std::string in = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: 9\r\n\r\n";
        std::string keep = relay_head_with_length(in, 123, true);
        HCHECK(keep.find("Content-Length: 123") != std::string::npos);
        HCHECK(keep.find("Content-Length: 9") == std::string::npos);
        HCHECK(keep.find("Connection: keep-alive") != std::string::npos);
        std::string cl = relay_head_with_length(in, 5, false);
        HCHECK(cl.find("Content-Length: 5") != std::string::npos);
        HCHECK(cl.find("Connection: close") != std::string::npos);
    }
}
