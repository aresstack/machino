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
}
