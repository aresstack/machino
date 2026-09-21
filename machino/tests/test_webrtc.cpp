// WebRTC slice, pure parts: SDP offer/answer against a Chrome-shaped offer,
// HMAC-SHA1 against RFC 2202 vectors, CRC32 against the classic reference,
// and a full STUN Binding Request/Response roundtrip with verified
// MESSAGE-INTEGRITY and FINGERPRINT.
#include "app/webrtc/sdp.hpp"
#include "app/webrtc/stun.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace machino;
extern int g_fail_ext, g_pass_ext;
#define WCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

const char* CHROME_OFFER =
    "v=0\r\n"
    "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1\r\n"
    "a=msid-semantic: WMS\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:4ZcD\r\n"
    "a=ice-pwd:2/1muCWoOi3uLifh0NuRHlLH\r\n"
    "a=fingerprint:sha-256 7B:8B:F0:65:5F:78:E2:51:3B:AC:6F:F3:3F:46:1B:35:DC:B8:5F:64:1A:24:C2:43:F0:A1:58:D0:A1:2C:19:08\r\n"
    "a=setup:actpass\r\n"
    "a=mid:0\r\n"
    "a=recvonly\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96 98 102 103\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:4ZcD\r\n"
    "a=ice-pwd:2/1muCWoOi3uLifh0NuRHlLH\r\n"
    "a=fingerprint:sha-256 7B:8B:F0:65:5F:78:E2:51:3B:AC:6F:F3:3F:46:1B:35:DC:B8:5F:64:1A:24:C2:43:F0:A1:58:D0:A1:2C:19:08\r\n"
    "a=setup:actpass\r\n"
    "a=mid:1\r\n"
    "a=recvonly\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:96 VP8/90000\r\n"
    "a=rtpmap:98 H264/90000\r\n"
    "a=fmtp:98 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42001f\r\n"
    "a=rtpmap:102 H264/90000\r\n"
    "a=rtcp-fb:102 nack\r\n"
    "a=rtcp-fb:102 nack pli\r\n"
    "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
    "a=rtpmap:103 rtx/90000\r\n"
    "a=fmtp:103 apt=102\r\n";

bool has_line(const std::string& sdp, const std::string& line) {
    return sdp.find(line + "\r\n") != std::string::npos;
}

void be16(std::vector<uint8_t>& b, uint16_t v) { b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v); }
void be32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back((uint8_t)(v >> (8 * i))); }

} // namespace

void run_webrtc_tests() {
    // --- HMAC-SHA1: RFC 2202 cases 1 + 2 ---
    {
        uint8_t key[20]; memset(key, 0x0b, 20);
        uint8_t mac[20];
        webrtc::hmac_sha1(key, 20, (const uint8_t*)"Hi There", 8, mac);
        const uint8_t exp1[20] = {0xb6,0x17,0x31,0x86,0x55,0x05,0x72,0x64,0xe2,0x8b,0xc0,0xb6,0xfb,0x37,0x8c,0x8e,0xf1,0x46,0xbe,0x00};
        WCHECK(memcmp(mac, exp1, 20) == 0);
        webrtc::hmac_sha1((const uint8_t*)"Jefe", 4, (const uint8_t*)"what do ya want for nothing?", 28, mac);
        const uint8_t exp2[20] = {0xef,0xfc,0xdf,0x6a,0xe5,0xeb,0x2f,0xa2,0xd2,0x74,0x16,0xd5,0xf1,0x84,0xdf,0x9c,0x25,0x9a,0x7c,0x79};
        WCHECK(memcmp(mac, exp2, 20) == 0);
    }
    // --- CRC32 reference ---
    WCHECK(webrtc::crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u);

    // --- SDP offer parse ---
    webrtc::Offer o = webrtc::parse_offer(CHROME_OFFER);
    WCHECK(o.ok);
    WCHECK(o.ice_ufrag == "4ZcD");
    WCHECK(o.ice_pwd == "2/1muCWoOi3uLifh0NuRHlLH");
    WCHECK(o.fingerprint_alg == "sha-256");
    WCHECK(o.fingerprint.rfind("7B:8B:F0", 0) == 0);
    WCHECK(o.media.size() == 2);
    WCHECK(o.media[0].kind == "audio" && o.media[0].mid == "0");
    WCHECK(o.media[1].kind == "video" && o.media[1].mid == "1");
    WCHECK(o.video_index == 1);
    WCHECK(o.media[1].h264_pt == 102);                 // pm=0 payload 98 must be skipped
    WCHECK(o.media[1].h264_profile == "42e01f");

    // --- SDP answer shape ---
    webrtc::AnswerParams ap;
    ap.ice_ufrag = "mfrag"; ap.ice_pwd = "mpwdmpwdmpwdmpwdmpwdmpwd";
    ap.fingerprint = "AA:BB:CC:DD"; ap.host_ip = "192.168.1.10"; ap.port = 51000;
    ap.ssrc = 0x11223344;
    std::string ans = webrtc::build_answer(o, ap);
    WCHECK(!ans.empty());
    WCHECK(has_line(ans, "a=ice-lite"));
    WCHECK(has_line(ans, "a=group:BUNDLE 1"));        // only the accepted mid
    WCHECK(has_line(ans, "m=audio 0 UDP/TLS/RTP/SAVPF 0"));   // declined: port 0
    WCHECK(has_line(ans, "m=video 51000 UDP/TLS/RTP/SAVPF 102"));
    WCHECK(ans.find("m=audio") < ans.find("m=video"));         // offer order mirrored
    WCHECK(has_line(ans, "a=mid:1"));
    WCHECK(has_line(ans, "a=setup:passive"));
    WCHECK(has_line(ans, "a=sendonly"));
    WCHECK(has_line(ans, "a=rtcp-mux"));
    WCHECK(has_line(ans, "a=rtpmap:102 H264/90000"));
    WCHECK(has_line(ans, "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f"));
    WCHECK(has_line(ans, "a=rtcp-fb:102 nack pli"));
    WCHECK(has_line(ans, "a=candidate:1 1 udp 2130706431 192.168.1.10 51000 typ host"));
    WCHECK(has_line(ans, "a=fingerprint:sha-256 AA:BB:CC:DD"));

    // --- SDP refusals ---
    WCHECK(!webrtc::parse_offer("v=0\r\nm=video 9 RTP/AVP 96\r\na=rtpmap:96 VP8/90000\r\n").ok);
    WCHECK(webrtc::build_answer(webrtc::Offer{}, ap).empty());

    // --- STUN roundtrip ---
    const std::string pwd = "mpwdmpwdmpwdmpwdmpwdmpwd";
    std::vector<uint8_t> req;
    be16(req, 0x0001); be16(req, 0); be32(req, 0x2112A442);
    for (int i = 0; i < 12; ++i) req.push_back((uint8_t)(0xA0 + i));
    const char* user = "mfrag:4ZcD";
    be16(req, 0x0006); be16(req, (uint16_t)strlen(user));
    req.insert(req.end(), user, user + strlen(user));
    while (req.size() & 3) req.push_back(0);
    be16(req, 0x0024); be16(req, 4); be32(req, 0x6e7f1eff);       // PRIORITY
    // MESSAGE-INTEGRITY with the length as if the message ends right after it
    {
        uint16_t adj = (uint16_t)(req.size() - 20 + 24);
        req[2] = (uint8_t)(adj >> 8); req[3] = (uint8_t)adj;
        uint8_t mac[20];
        webrtc::hmac_sha1((const uint8_t*)pwd.data(), pwd.size(), req.data(), req.size(), mac);
        be16(req, 0x0008); be16(req, 20);
        req.insert(req.end(), mac, mac + 20);
        uint16_t fin = (uint16_t)(req.size() - 20);
        req[2] = (uint8_t)(fin >> 8); req[3] = (uint8_t)fin;
    }
    WCHECK(webrtc::is_stun(req.data(), req.size()));
    webrtc::StunRequest sr = webrtc::parse_binding_request(req.data(), req.size(), pwd);
    WCHECK(sr.ok);
    WCHECK(sr.username == "mfrag:4ZcD");
    WCHECK(sr.integrity_ok);
    WCHECK(memcmp(sr.tid, req.data() + 8, 12) == 0);
    // wrong password must not verify
    WCHECK(!webrtc::parse_binding_request(req.data(), req.size(), "wrong").integrity_ok);

    std::vector<uint8_t> resp = webrtc::binding_response(sr.tid, 0xC0A80164u /*192.168.1.100*/, 61000, pwd);
    WCHECK(webrtc::is_stun(resp.data(), resp.size()));
    WCHECK(resp[0] == 0x01 && resp[1] == 0x01);                    // Binding Success
    WCHECK(memcmp(resp.data() + 8, sr.tid, 12) == 0);
    // XOR-MAPPED-ADDRESS decodes back to the peer
    {
        WCHECK(resp.size() > 32);
        WCHECK(resp[20] == 0x00 && resp[21] == 0x20);              // type
        uint16_t xport = (uint16_t)((resp[26] << 8) | resp[27]);
        uint32_t xip = ((uint32_t)resp[28] << 24) | ((uint32_t)resp[29] << 16) | ((uint32_t)resp[30] << 8) | resp[31];
        WCHECK((uint16_t)(xport ^ 0x2112) == 61000);
        WCHECK((xip ^ 0x2112A442u) == 0xC0A80164u);
    }
    // FINGERPRINT: recompute over everything before the attribute
    {
        const size_t fp_at = resp.size() - 8;
        uint32_t want = webrtc::crc32(resp.data(), fp_at) ^ 0x5354554eu;
        uint32_t got = ((uint32_t)resp[fp_at + 4] << 24) | ((uint32_t)resp[fp_at + 5] << 16) |
                       ((uint32_t)resp[fp_at + 6] << 8) | resp[fp_at + 7];
        WCHECK(got == want);
    }
    // response MESSAGE-INTEGRITY: verify the same adjusted-length rule
    {
        const size_t mi_at = resp.size() - 8 - 24;                 // MI attr before FINGERPRINT
        WCHECK(resp[mi_at] == 0x00 && resp[mi_at + 1] == 0x08);
        std::vector<uint8_t> copy(resp.begin(), resp.begin() + (long)mi_at);
        uint16_t adj = (uint16_t)(mi_at + 24 - 20);
        copy[2] = (uint8_t)(adj >> 8); copy[3] = (uint8_t)adj;
        uint8_t mac[20];
        webrtc::hmac_sha1((const uint8_t*)pwd.data(), pwd.size(), copy.data(), copy.size(), mac);
        WCHECK(memcmp(mac, resp.data() + mi_at + 4, 20) == 0);
    }
}
