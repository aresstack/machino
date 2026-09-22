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
        HCHECK(parse_request("GET /cgi-bin/j/pulse.cgi?x=1 HTTP/1.1\r\nHost: cam\r\n"
                             "Authorization: Basic Zm9v\r\nCookie: s=1\r\nConnection: keep-alive\r\n\r\n",
                             used, fr) == Parse::Ok);
        std::string w = forward_request(fr, "127.0.0.1");
        // header NAMES are lower-cased by the parser and forwarded verbatim
        // (valid HTTP; CGI reads them case-insensitively).
        HCHECK(w.rfind("GET /cgi-bin/j/pulse.cgi?x=1 HTTP/1.0\r\n", 0) == 0);
        HCHECK(w.find("authorization: Basic Zm9v\r\n") != std::string::npos);
        HCHECK(w.find("cookie: s=1\r\n") != std::string::npos);
        HCHECK(w.find("Host: 127.0.0.1\r\n") != std::string::npos);
        HCHECK(w.find("Host: cam\r\n") == std::string::npos);          // original Host dropped
        HCHECK(w.find("Connection: close\r\n") != std::string::npos);
        HCHECK(w.find("keep-alive") == std::string::npos);              // hop-by-hop dropped
        HCHECK(w.rfind("\r\n\r\n") == w.size() - 4);                    // GET: empty body
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
