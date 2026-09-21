// RFC 6455 essentials for /ws/video: the accept key against the RFC's own
// worked example, frame round-trips, masked-client parsing, refusal of the
// shapes the contract excludes.
#include "app/http/websocket.hpp"
#include <cstdio>
#include <cstring>
#include <string>

using namespace machino;
extern int g_fail_ext, g_pass_ext;
#define WCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// A client frame as a browser would send it (FIN, masked).
std::string client_frame(int opcode, const std::string& payload, const uint8_t mask[4]) {
    std::string out;
    out += (char)(0x80 | opcode);
    if (payload.size() < 126) out += (char)(0x80 | payload.size());
    else { out += (char)(0x80 | 126); out += (char)(payload.size() >> 8); out += (char)(payload.size() & 0xff); }
    out.append((const char*)mask, 4);
    for (size_t i = 0; i < payload.size(); ++i) out += (char)(payload[i] ^ mask[i & 3]);
    return out;
}

} // namespace

void run_websocket_tests() {
    // SHA-1 "abc" (FIPS 180-1 example) and the RFC 6455 handshake example.
    uint8_t d[20];
    ws::sha1((const uint8_t*)"abc", 3, d);
    static const uint8_t abc[20] = {0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
                                    0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    WCHECK(std::memcmp(d, abc, 20) == 0);
    WCHECK(ws::base64((const uint8_t*)"Man", 3) == "TWFu");
    WCHECK(ws::base64((const uint8_t*)"Ma", 2) == "TWE=");
    WCHECK(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    std::string hs = ws::handshake_response("dGhlIHNhbXBsZSBub25jZQ==");
    WCHECK(hs.rfind("HTTP/1.1 101 ", 0) == 0);
    WCHECK(hs.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);

    // server frames: small, medium (16-bit length), text vs binary opcode
    std::string small = ws::frame(true, "hi", 2);
    WCHECK(small.size() == 4 && (uint8_t)small[0] == 0x81 && (uint8_t)small[1] == 2 && small.substr(2) == "hi");
    std::string big(70000, 'x');
    std::string fr = ws::frame(false, big.data(), big.size());
    WCHECK((uint8_t)fr[0] == 0x82 && (uint8_t)fr[1] == 127 && fr.size() == big.size() + 10);

    // masked client frame parses back to the payload
    const uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    std::string idr = client_frame(1, "{\"request\":\"idr\"}", mask);
    size_t used = 0; int op = 0; std::string pl;
    WCHECK(ws::parse_frame(idr, used, op, pl) == ws::Parse::Ok);
    WCHECK(op == 1 && pl == "{\"request\":\"idr\"}" && used == idr.size());
    // two frames back to back: the first consume leaves the second intact
    std::string two = idr + client_frame(9, "ping", mask);
    WCHECK(ws::parse_frame(two, used, op, pl) == ws::Parse::Ok && op == 1);
    std::string rest = two.substr(used);
    WCHECK(ws::parse_frame(rest, used, op, pl) == ws::Parse::Ok && op == 9 && pl == "ping");
    WCHECK(ws::pong_frame(pl).size() == 2 + 4 && (uint8_t)ws::pong_frame(pl)[0] == 0x8a);

    // refusals: unmasked client frame, fragmented message, oversized payload
    std::string unmasked = "\x81\x02hi";
    WCHECK(ws::parse_frame(unmasked, used, op, pl) == ws::Parse::Bad);
    std::string frag = client_frame(1, "x", mask); frag[0] = (char)(frag[0] & 0x7f);   // clear FIN
    WCHECK(ws::parse_frame(frag, used, op, pl) == ws::Parse::Bad);
    std::string huge = client_frame(2, std::string(5000, 'y'), mask);
    WCHECK(ws::parse_frame(huge, used, op, pl, 4096) == ws::Parse::Bad);
    // incomplete: header only
    WCHECK(ws::parse_frame(std::string("\x81", 1), used, op, pl) == ws::Parse::Incomplete);
}
