// The DTLS transport against a REAL mbedTLS client, fully in memory: the
// handshake must complete with use_srtp negotiated, and the RFC 5764 exporter
// must yield the SAME SRTP keys on both ends - that is the property the
// browser will rely on. No sockets, no timers firing (lossless queues).
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "app/webrtc/dtls.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/timing.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

using namespace machino;
extern int g_fail_ext, g_pass_ext;
#define DCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

struct Client {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config       conf;
    mbedtls_ssl_context      ssl;
    mbedtls_timing_delay_context timer;
    std::deque<std::vector<uint8_t>> in, out;
    uint8_t master[48]; uint8_t randoms[64];
    mbedtls_tls_prf_types prf = MBEDTLS_SSL_TLS_PRF_NONE;
    bool have_secret = false;

    static int send_cb(void* v, const unsigned char* b, size_t n) {
        ((Client*)v)->out.emplace_back(b, b + n); return (int)n;
    }
    static int recv_cb(void* v, unsigned char* b, size_t n) {
        Client* c = (Client*)v;
        if (c->in.empty()) return MBEDTLS_ERR_SSL_WANT_READ;
        std::vector<uint8_t>& d = c->in.front();
        if (d.size() > n) { c->in.pop_front(); return MBEDTLS_ERR_SSL_WANT_READ; }
        const size_t len = d.size();
        memcpy(b, d.data(), len);
        c->in.pop_front();
        return (int)len;
    }
    static void keys_cb(void* v, mbedtls_ssl_key_export_type t,
                        const unsigned char* secret, size_t len,
                        const unsigned char cr[32], const unsigned char sr[32],
                        mbedtls_tls_prf_types prf_type) {
        Client* c = (Client*)v;
        if (t != MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET || len != 48) return;
        memcpy(c->master, secret, 48);
        memcpy(c->randoms, cr, 32); memcpy(c->randoms + 32, sr, 32);
        c->prf = prf_type; c->have_secret = true;
    }

    bool init() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ssl_init(&ssl);
        if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                  (const unsigned char*)"t", 1) != 0) return false;
        if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) return false;
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);   // fingerprint checking is the app's job
        static const mbedtls_ssl_srtp_profile profiles[] = {
            MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80, MBEDTLS_TLS_SRTP_UNSET,
        };
        if (mbedtls_ssl_conf_dtls_srtp_protection_profiles(&conf, profiles) != 0) return false;
        if (mbedtls_ssl_setup(&ssl, &conf) != 0) return false;
        mbedtls_ssl_set_export_keys_cb(&ssl, keys_cb, this);
        mbedtls_ssl_set_timer_cb(&ssl, &timer, mbedtls_timing_set_delay, mbedtls_timing_get_delay);
        mbedtls_ssl_set_bio(&ssl, this, send_cb, recv_cb, nullptr);
        // Force the client to FRAGMENT its handshake flights: a real browser
        // ClientHello is ~1.4 KB and fragments on the wire, and mbedTLS
        // servers before 3.6.6 could not re-assemble it (mbedtls#7549) - the
        // exact hardware failure this suite must never regress on.
        mbedtls_ssl_set_mtu(&ssl, 512);
        return true;
    }
    void free_all() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
};

} // namespace

void run_dtls_tests() {
    webrtc::DtlsTransport srv;
    DCHECK(srv.ok());
    // cookies (HelloVerifyRequest) are enabled: the server needs the peer's
    // transport id before it will process a ClientHello
    srv.set_peer(0x7f000001, 12345);
    // fingerprint: 32 uppercase hex pairs, colon separated = 95 chars
    const std::string fp = srv.fingerprint();
    DCHECK(fp.size() == 95);
    DCHECK(fp[2] == ':' && fp[92] == ':');

    Client cli;
    DCHECK(cli.init());

    bool cli_done = false;
    int spins = 0;
    for (; spins < 200 && !(cli_done && srv.handshake_done()); ++spins) {
        if (!cli_done) {
            const int rc = mbedtls_ssl_handshake(&cli.ssl);
            if (rc == 0) cli_done = true;
            else if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                fprintf(stderr, "client handshake rc=-0x%04x\n", (unsigned)-rc);
                break;
            }
        }
        while (!cli.out.empty()) { srv.feed(cli.out.front().data(), cli.out.front().size()); cli.out.pop_front(); }
        if (!srv.step()) break;
        std::vector<uint8_t> d;
        while (srv.take_out(d)) cli.in.push_back(d);
    }
    DCHECK(cli_done);
    DCHECK(srv.handshake_done());

    webrtc::SrtpKey cam_send{}, browser_send{};
    DCHECK(srv.export_srtp(cam_send, browser_send));

    // the client derives the same 2x(16+14) exporter block
    DCHECK(cli.have_secret);
    uint8_t km[60];
    DCHECK(mbedtls_ssl_tls_prf(cli.prf, cli.master, 48, "EXTRACTOR-dtls_srtp",
                               cli.randoms, 64, km, sizeof km) == 0);
    DCHECK(memcmp(browser_send.master_key,  km,      16) == 0);   // client key
    DCHECK(memcmp(cam_send.master_key,      km + 16, 16) == 0);   // server key
    DCHECK(memcmp(browser_send.master_salt, km + 32, 14) == 0);
    DCHECK(memcmp(cam_send.master_salt,     km + 46, 14) == 0);

    // and the keys actually interoperate: camera protects RTCP, client-side
    // SRTP session (mirrored keys) unprotects it
    webrtc::SrtpSession cam(cam_send, browser_send);
    webrtc::SrtpSession browser(browser_send, cam_send);
    std::vector<uint8_t> sr = {0x80, 200, 0x00, 0x01, 0x11, 0x22, 0x33, 0x44};
    std::vector<uint8_t> plain = sr;
    DCHECK(cam.protect_rtcp(sr));
    DCHECK(browser.unprotect_rtcp(sr));
    DCHECK(sr == plain);

    cli.free_all();
}
