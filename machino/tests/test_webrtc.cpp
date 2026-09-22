// WebRTC slice, pure parts: SDP offer/answer against a Chrome-shaped offer,
// HMAC-SHA1 against RFC 2202 vectors, CRC32 against the classic reference,
// and a full STUN Binding Request/Response roundtrip with verified
// MESSAGE-INTEGRITY and FINGERPRINT.
#include "app/webrtc/aes.hpp"
#include "app/webrtc/rtp.hpp"
#include "app/webrtc/sdp.hpp"
#include "app/webrtc/srtp.hpp"
#include "app/webrtc/stun.hpp"
#include "core/json.hpp"
#include "sdp_offers.hpp"
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

    // --- signalling JSON limits: a real Chrome offer is ~7.5 KB in ONE JSON
    // string; the default JsonLimits reject it (the hardware bug), the raised
    // signalling limits must accept it ---
    {
        std::string big_sdp = CHROME_OFFER;
        while (big_sdp.size() < 8000) big_sdp += "a=extmap:9 urn:example:padding-attribute-for-size\r\n";
        machino::Json esc = machino::Json::string(big_sdp);
        const std::string wire = "{\"req\":\"offer\",\"data\":" + esc.dump() + "}";
        machino::Json out; std::string jerr;
        WCHECK(!machino::Json::parse(wire, out, jerr));            // defaults: too small (the regression)
        const machino::JsonLimits sig{16, 32768, 65536};
        WCHECK(machino::Json::parse(wire, out, jerr, sig));        // signalling limits: accepted
        const machino::Json* d = out.get("data");
        WCHECK(d && d->is_string() && d->as_string() == big_sdp);
        webrtc::Offer big = webrtc::parse_offer(d->as_string());
        WCHECK(big.ok && big.media[(size_t)big.video_index].h264_pt == 102);
    }

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

    // --- RTP packetization (RFC 6184) ---
    {
        // AU: AUD (dropped) + SPS + PPS + one 3001-byte IDR (forces FU-A)
        std::vector<uint8_t> au;
        auto put = [&](std::initializer_list<uint8_t> nal) {
            const uint8_t sc[4] = {0, 0, 0, 1};
            au.insert(au.end(), sc, sc + 4);
            au.insert(au.end(), nal.begin(), nal.end());
        };
        put({0x09, 0xf0});                                    // AUD
        put({0x67, 0x64, 0x00, 0x33, 0xaa});                  // SPS
        put({0x68, 0xeb, 0xe3, 0xcb});                        // PPS
        const uint8_t sc[4] = {0, 0, 0, 1};
        au.insert(au.end(), sc, sc + 4);
        std::vector<uint8_t> idr; idr.push_back(0x65);        // nri=3, type 5
        for (int i = 0; i < 3000; ++i) idr.push_back((uint8_t)i);
        au.insert(au.end(), idr.begin(), idr.end());

        webrtc::RtpParams rp; rp.payload_type = 102; rp.ssrc = 0xCAFEBABE; rp.max_payload = 1200;
        uint16_t seq = 100;
        auto pkts = webrtc::packetize_h264(au.data(), au.size(), 123456, seq, rp);
        // SPS + PPS single-NAL, IDR 3000 payload bytes -> chunks 1198+1198+604
        WCHECK(pkts.size() == 5);
        WCHECK(seq == 105);
        for (size_t i = 0; i < pkts.size(); ++i) {
            const auto& p = pkts[i];
            WCHECK(p.size() > 12);
            WCHECK(p[0] == 0x80);
            WCHECK((p[1] & 0x7f) == 102);
            WCHECK(((p[2] << 8) | p[3]) == (int)(100 + i));   // continuous seq
            uint32_t ts = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
            WCHECK(ts == 123456u);
            uint32_t ssrc = ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) | ((uint32_t)p[10] << 8) | p[11];
            WCHECK(ssrc == 0xCAFEBABEu);
            WCHECK(((p[1] & 0x80) != 0) == (i == pkts.size() - 1));   // marker on the last only
        }
        WCHECK(pkts[0][12] == 0x67 && pkts[1][12] == 0x68);   // SPS/PPS verbatim, AUD gone
        // FU-A structure + reassembly
        std::vector<uint8_t> re; re.push_back((uint8_t)((pkts[2][12] & 0xe0) | (pkts[2][13] & 0x1f)));
        for (size_t i = 2; i < 5; ++i) {
            const auto& p = pkts[i];
            WCHECK((p[12] & 0x1f) == 28);                     // FU-A indicator
            WCHECK((p[12] & 0xe0) == 0x60);                   // NRI carried over
            WCHECK(((p[13] & 0x80) != 0) == (i == 2));        // S
            WCHECK(((p[13] & 0x40) != 0) == (i == 4));        // E
            WCHECK((p[13] & 0x1f) == 5);                      // original type
            re.insert(re.end(), p.begin() + 14, p.end());
        }
        WCHECK(re == idr);
        WCHECK(pkts[2].size() == 14 + 1198 && pkts[3].size() == 14 + 1198 && pkts[4].size() == 14 + 604);
    }
    // --- RTCP: PLI detected, RR alone is not a PLI, RTP is not RTCP ---
    {
        const uint8_t pli[12] = {0x81, 206, 0x00, 0x02, 0,0,0,1, 0xCA,0xFE,0xBA,0xBE};
        WCHECK(webrtc::is_rtcp(pli, 12));
        webrtc::RtcpInfo ri = webrtc::parse_rtcp(pli, 12);
        WCHECK(ri.pli);
        const uint8_t rr[8] = {0x80, 201, 0x00, 0x01, 0,0,0,1};
        webrtc::RtcpInfo ri2 = webrtc::parse_rtcp(rr, 8);
        WCHECK(ri2.receiver_report && !ri2.pli);
        const uint8_t rtp[12] = {0x80, 102, 0x00, 0x01, 0,0,0,0, 0,0,0,1};
        WCHECK(!webrtc::is_rtcp(rtp, 12));
    }

    // --- AES-128: FIPS-197 appendix vector ---
    {
        const uint8_t key[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
        const uint8_t pt[16]  = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
        const uint8_t ct[16]  = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
        webrtc::Aes128 a(key);
        uint8_t out[16];
        a.encrypt(pt, out);
        WCHECK(memcmp(out, ct, 16) == 0);
    }
    // --- SRTP KDF + AES-CM: RFC 3711 B.3 / B.2 (cross-checked with node crypto) ---
    {
        webrtc::SrtpKey mk;
        const uint8_t mkey[16] = {0xE1,0xF9,0x7A,0x0D,0x3E,0x01,0x8B,0xE0,0xD6,0x4F,0xA3,0x2C,0x06,0xDE,0x41,0x39};
        const uint8_t msalt[14] = {0x0E,0xC6,0x75,0xAD,0x49,0x8A,0xFE,0xEB,0xB6,0x96,0x0B,0x3A,0xAB,0xE6};
        memcpy(mk.master_key, mkey, 16); memcpy(mk.master_salt, msalt, 14);
        uint8_t ck[16], ak[20], sk[14];
        webrtc::srtp_kdf(mk, 0, ck, 16);
        webrtc::srtp_kdf(mk, 1, ak, 20);
        webrtc::srtp_kdf(mk, 2, sk, 14);
        const uint8_t eck[16] = {0xC6,0x1E,0x7A,0x93,0x74,0x4F,0x39,0xEE,0x10,0x73,0x4A,0xFE,0x3F,0xF7,0xA0,0x87};
        const uint8_t eak[20] = {0xCE,0xBE,0x32,0x1F,0x6F,0xF7,0x71,0x6B,0x6F,0xD4,0xAB,0x49,0xAF,0x25,0x6A,0x15,0x6D,0x38,0xBA,0xA4};
        const uint8_t esk[14] = {0x30,0xCB,0xBC,0x08,0x86,0x3D,0x8C,0x85,0xD4,0x9D,0xB3,0x4A,0x9A,0xE1};
        WCHECK(memcmp(ck, eck, 16) == 0);
        WCHECK(memcmp(ak, eak, 20) == 0);
        WCHECK(memcmp(sk, esk, 14) == 0);
        const uint8_t cmkey[16] = {0x2B,0x7E,0x15,0x16,0x28,0xAE,0xD2,0xA6,0xAB,0xF7,0x15,0x88,0x09,0xCF,0x4F,0x3C};
        const uint8_t cmiv[16]  = {0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0x00,0x00};
        uint8_t ks[32];
        webrtc::Aes128 cm(cmkey);
        webrtc::aes_cm_keystream(cm, cmiv, ks, 32);
        const uint8_t eks[32] = {0xE0,0x3E,0xAD,0x09,0x35,0xC9,0x5E,0x80,0xE1,0x66,0xB1,0x6D,0xD9,0x2B,0x4E,0xB4,
                                 0xD2,0x35,0x13,0x16,0x2B,0x02,0xD0,0xF7,0x2A,0x43,0xA2,0xFE,0x4A,0x5F,0x97,0xAB};
        WCHECK(memcmp(ks, eks, 32) == 0);
    }
    // --- SRTP/SRTCP session behaviour ---
    {
        webrtc::SrtpKey a{}, b{};
        for (int i = 0; i < 16; ++i) { a.master_key[i] = (uint8_t)i; b.master_key[i] = (uint8_t)(0x40 + i); }
        for (int i = 0; i < 14; ++i) { a.master_salt[i] = (uint8_t)(0x80 + i); b.master_salt[i] = (uint8_t)(0xC0 + i); }
        webrtc::SrtpSession cam(a, b);      // camera: sends with a, reads with b
        webrtc::SrtpSession browser(b, a);  // the peer, mirrored

        // RTP protect: header stays, payload changes, 10-byte tag appended
        std::vector<uint8_t> rtp = {0x80, 102, 0x12, 0x34, 0,0,0x30,0x39, 0xCA,0xFE,0xBA,0xBE, 1,2,3,4,5,6,7,8};
        std::vector<uint8_t> plain = rtp;
        WCHECK(cam.protect_rtp(rtp));
        WCHECK(rtp.size() == plain.size() + 10);
        WCHECK(memcmp(rtp.data(), plain.data(), 12) == 0);
        WCHECK(memcmp(rtp.data() + 12, plain.data() + 12, 8) != 0);

        // RTCP roundtrip: camera protects, browser-side session unprotects
        std::vector<uint8_t> sr = {0x80, 200, 0x00, 0x06, 0x11,0x22,0x33,0x44,
                                   0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};
        std::vector<uint8_t> sr_plain = sr;
        WCHECK(cam.protect_rtcp(sr));
        WCHECK(sr.size() == sr_plain.size() + 14);              // E+index + tag
        std::vector<uint8_t> rx = sr;
        WCHECK(browser.unprotect_rtcp(rx));
        WCHECK(rx == sr_plain);
        // replay of the same packet must be refused
        std::vector<uint8_t> replay = sr;
        WCHECK(!browser.unprotect_rtcp(replay));
        // a tampered byte must fail the auth check
        std::vector<uint8_t> bad = sr; bad[9] ^= 1;
        WCHECK(!browser.unprotect_rtcp(bad));
    }
}

// A refused offer has to say what the browser DID offer. The old message was
// the same whether the browser had no H264 at all - a platform limitation the
// user can act on - or offered it only in mode 0, which is a negotiation
// detail. Those need opposite answers, and on 2026-09-22 one such refusal sat
// in the camera log with no way to tell which it had been.
void run_sdp_refusal_tests() {
    auto offer = [](const std::string& video_lines) {
        return std::string(
            "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
            "a=ice-ufrag:abcd\r\na=ice-pwd:0123456789abcdef0123\r\n"
            "a=fingerprint:sha-256 AA:BB:CC\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:0\r\n") + video_lines;
    };

    // no video codec at all beyond the housekeeping payloads
    {
        webrtc::Offer o = webrtc::parse_offer(offer(
            "a=rtpmap:97 rtx/90000\r\na=rtpmap:98 red/90000\r\n"));
        WCHECK(!o.ok);
        WCHECK(o.error.find("offered: nothing") != std::string::npos);
        WCHECK(o.error.find("but not in mode 1") == std::string::npos);
    }

    // VP8/VP9/AV1 but no H264 - a browser build without H264
    {
        webrtc::Offer o = webrtc::parse_offer(offer(
            "a=rtpmap:96 VP8/90000\r\na=rtpmap:98 VP9/90000\r\n"
            "a=rtpmap:99 AV1/90000\r\na=rtpmap:97 rtx/90000\r\n"));
        WCHECK(!o.ok);
        WCHECK(o.error.find("VP8") != std::string::npos);
        WCHECK(o.error.find("VP9") != std::string::npos);
        WCHECK(o.error.find("AV1") != std::string::npos);
        WCHECK(o.error.find("rtx") == std::string::npos);      // housekeeping stays out
        WCHECK(o.error.find("but not in mode 1") == std::string::npos);
    }

    // H264 offered, but only packetization-mode=0 - the other diagnosis
    {
        webrtc::Offer o = webrtc::parse_offer(offer(
            "a=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42e01f\r\n"));
        WCHECK(!o.ok);
        WCHECK(o.error.find("H264") != std::string::npos);
        WCHECK(o.error.find("but not in mode 1") != std::string::npos);
    }
    // H264 with NO fmtp at all: RFC 6184 says mode 0 by default, so also refused
    {
        webrtc::Offer o = webrtc::parse_offer(offer("a=rtpmap:96 H264/90000\r\n"));
        WCHECK(!o.ok);
        WCHECK(o.error.find("but not in mode 1") != std::string::npos);
    }
    // and the happy path still works, with mode 1 among several payloads
    {
        webrtc::Offer o = webrtc::parse_offer(offer(
            "a=rtpmap:96 VP8/90000\r\n"
            "a=rtpmap:102 H264/90000\r\n"
            "a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"));
        WCHECK(o.ok);
        WCHECK(o.video_index == 0);
        WCHECK(o.media[0].h264_pt == 102);
        WCHECK(o.media[0].h264_profile == "42e01f");
        WCHECK(o.error.empty());
    }
    // the codec list is bounded: a hostile offer cannot grow the message
    {
        std::string many;
        for (int i = 0; i < 40; ++i) {
            char b[64];
            snprintf(b, sizeof b, "a=rtpmap:%d CODEC%d/90000\r\n", 96 + i, i);
            many += b;
        }
        webrtc::Offer o = webrtc::parse_offer(offer(many));
        WCHECK(!o.ok);
        WCHECK(o.error.size() < 400);
    }
}

// ---------------------------------------------------------------- AP16
// Browser compatibility, against a REAL captured Chrome 153 offer rather than
// a sketch of one. The sketch is what let the defect below survive: it offered
// a single H264 payload, so payload-number order and preference order were the
// same thing and the difference could not show.
void run_webrtc_ap16_tests() {
    using namespace machino::webrtc;
    const std::string chrome = test::chrome_153_offer();

    // What Chrome 153 actually offers, so the expectations below are anchored
    // in the fixture and not in memory of it.
    WCHECK(chrome.find("a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f") != std::string::npos);
    WCHECK(chrome.find("a=fmtp:41 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=f4001f") != std::string::npos);

    // 1. The defect: picking by payload NUMBER walked past everything the
    //    browser preferred and landed on pt 41, High 4:4:4 Predictive - a
    //    chroma format this camera does not produce. The m-line lists
    //    102 before 41, and 102 is what the answer must name.
    {
        Offer o = parse_offer(chrome);
        WCHECK(o.ok && o.video_index == 0);
        WCHECK(o.media[0].h264_pt == 102);
        WCHECK(o.media[0].h264_profile == "42001f");
        WCHECK(o.media[0].pts.size() == 34);              // the whole preference list is kept
        WCHECK(o.media[0].pts[0] == 96 && o.media[0].pts[10] == 102);
    }

    // 2. An exact profile match beats preference: a camera that really emits
    //    Main gets the Main payload, and the answer names the stream instead
    //    of approximating it.
    {
        Offer o = parse_offer(chrome, "4d001f");
        WCHECK(o.ok && o.media[0].h264_pt == 116 && o.media[0].h264_profile == "4d001f");
    }
    // The level is NOT part of the match - level-asymmetry-allowed exists for
    // exactly that, and a camera at level 5.1 must still find its own profile.
    {
        Offer o = parse_offer(chrome, "4d0033");
        WCHECK(o.ok && o.media[0].h264_pt == 116);
    }
    // This camera emits High (avc1.640033) and Chrome offers no High at all,
    // so it falls back to the browser's own first choice rather than to a
    // number. That fallback is the common case and must stay correct.
    {
        Offer o = parse_offer(chrome, "640033");
        WCHECK(o.ok && o.media[0].h264_pt == 102);
    }

    // 3. The audio m-line is mirrored and declined, and BUNDLE names only the
    //    section actually served - unchanged behaviour, pinned against the
    //    real offer this time.
    {
        Offer o = parse_offer(chrome);
        AnswerParams p;
        p.ice_ufrag = "aaaa"; p.ice_pwd = "bbbbbbbbbbbbbbbbbbbbbbbb";
        p.fingerprint = "AA:BB:CC"; p.host_ip = "192.168.1.10"; p.port = 40000; p.ssrc = 42;
        const std::string a = build_answer(o, p);
        WCHECK(a.find("a=group:BUNDLE 0\r\n") != std::string::npos);
        WCHECK(a.find("m=video 40000 UDP/TLS/RTP/SAVPF 102\r\n") != std::string::npos);
        WCHECK(a.find("a=rtpmap:102 H264/90000\r\n") != std::string::npos);
        WCHECK(a.find("profile-level-id=42001f\r\n") != std::string::npos);
        WCHECK(a.find("a=rtcp-fb:102 nack pli\r\n") != std::string::npos);
        WCHECK(a.find("m=audio 0 UDP/TLS/RTP/SAVPF 0\r\n") != std::string::npos);
        WCHECK(a.find("a=inactive\r\n") != std::string::npos);
        WCHECK(a.find("a=setup:passive\r\n") != std::string::npos);
        WCHECK(a.find("a=sendonly\r\n") != std::string::npos);
        // never the payload the old ordering would have chosen
        WCHECK(a.find(" 41\r\n") == std::string::npos && a.find("f4001f") == std::string::npos);
    }

    // 4. Bare LF, as a permissive peer or a hand-rolled client may send it.
    {
        std::string lf = chrome;
        for (size_t i = lf.find('\r'); i != std::string::npos; i = lf.find('\r')) lf.erase(i, 1);
        Offer o = parse_offer(lf);
        WCHECK(o.ok && o.media[0].h264_pt == 102);
    }

    // 5. Case. rtpmap codec names and fmtp parameter names are both
    //    case-insensitive (RFC 4566, RFC 6184); comparing them exactly is a
    //    refusal waiting for the one peer that writes them differently.
    {
        const std::string sdp =
            "v=0\r\ns=-\r\nt=0 0\r\n"
            "a=ice-ufrag:u\r\na=ice-pwd:pppppppppppppppppppppppp\r\n"
            "a=fingerprint:sha-256 AA:BB\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:0\r\n"
            "a=rtpmap:96 h264/90000\r\n"
            "a=fmtp:96 Packetization-Mode=1;Profile-Level-Id=42E01F\r\n";
        Offer o = parse_offer(sdp);
        WCHECK(o.ok && o.media[0].h264_pt == 96 && o.media[0].h264_profile == "42E01F");
    }

    // 6. The refusals still refuse, and still say which kind of refusal it is.
    //    Synthetic on purpose: no browser is named for these, because none was
    //    captured producing them.
    {
        const std::string mode0 =
            "v=0\r\ns=-\r\nt=0 0\r\n"
            "a=ice-ufrag:u\r\na=ice-pwd:pppppppppppppppppppppppp\r\n"
            "a=fingerprint:sha-256 AA:BB\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\na=mid:0\r\n"
            "a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=0\r\n"
            "a=rtpmap:97 VP8/90000\r\n";
        Offer o = parse_offer(mode0);
        WCHECK(!o.ok);
        WCHECK(o.error.find("H264 was offered, but not in mode 1") != std::string::npos);
        WCHECK(o.error.find("H264") != std::string::npos && o.error.find("VP8") != std::string::npos);
    }
    {
        const std::string noh264 =
            "v=0\r\ns=-\r\nt=0 0\r\n"
            "a=ice-ufrag:u\r\na=ice-pwd:pppppppppppppppppppppppp\r\n"
            "a=fingerprint:sha-256 AA:BB\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\na=mid:0\r\n"
            "a=rtpmap:96 VP8/90000\r\na=rtpmap:97 rtx/90000\r\n";
        Offer o = parse_offer(noh264);
        WCHECK(!o.ok);
        WCHECK(o.error.find("offered: VP8") != std::string::npos);   // rtx is housekeeping, not a codec
        WCHECK(o.error.find("but not in mode 1") == std::string::npos);
    }

    // 7. The m-line is the list and the attributes describe it: when the list
    //    holds a usable payload, that is the one, whatever else was described.
    {
        const std::string odd =
            "v=0\r\ns=-\r\nt=0 0\r\n"
            "a=ice-ufrag:u\r\na=ice-pwd:pppppppppppppppppppppppp\r\n"
            "a=fingerprint:sha-256 AA:BB\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 98\r\na=mid:0\r\n"
            "a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1\r\n"
            "a=rtpmap:98 H264/90000\r\na=fmtp:98 packetization-mode=1;profile-level-id=42e01f\r\n";
        Offer o = parse_offer(odd);
        WCHECK(o.ok && o.media[0].h264_pt == 98);
    }

    // 8. But a malformed offer that describes a usable H264 and forgets to
    //    list it is still answered. Refusing a compatible stream over a
    //    missing number would be the unnecessary rejection this work exists
    //    to remove - and it is what the older sketch-shaped offers look like.
    {
        const std::string missing =
            "v=0\r\ns=-\r\nt=0 0\r\n"
            "a=ice-ufrag:u\r\na=ice-pwd:pppppppppppppppppppppppp\r\n"
            "a=fingerprint:sha-256 AA:BB\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:0\r\n"
            "a=rtpmap:96 VP8/90000\r\n"
            "a=rtpmap:102 H264/90000\r\n"
            "a=fmtp:102 packetization-mode=1;profile-level-id=42e01f\r\n";
        Offer o = parse_offer(missing);
        WCHECK(o.ok && o.media[0].h264_pt == 102 && o.media[0].h264_profile == "42e01f");
    }
}
