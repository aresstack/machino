#include "app/webrtc/dtls.hpp"
#include "core/log.hpp"

// mbedTLS 3.x hides struct members; the DTLS-SRTP negotiation result has no
// public accessor, so this unit opts into private access for that one read.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/timing.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>

static const char* MOD = "DTLS";

namespace machino { namespace webrtc {

struct DtlsTransport::Impl {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context       key;
    mbedtls_x509_crt         cert;
    mbedtls_ssl_config       conf;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_cookie_ctx   cookie;
    mbedtls_timing_delay_context timer;
    uint8_t  cli_id[6] = {0};    // peer ip(4)+port(2), the cookie's transport id
    bool     have_cli_id = false;

    std::deque<std::vector<uint8_t>> in, out;
    std::string fp;
    bool ok = false, done = false, fatal = false;

    // RFC 5764 exporter inputs, captured by the export-keys callback.
    uint8_t  master[48];
    uint8_t  randoms[64];                 // client_random || server_random
    mbedtls_tls_prf_types prf = MBEDTLS_SSL_TLS_PRF_NONE;
    bool have_secret = false;

    static int bio_send(void* v, const unsigned char* buf, size_t n) {
        Impl* s = (Impl*)v;
        s->out.emplace_back(buf, buf + n);
        return (int)n;
    }
    static int bio_recv(void* v, unsigned char* buf, size_t n) {
        Impl* s = (Impl*)v;
        if (s->in.empty()) return MBEDTLS_ERR_SSL_WANT_READ;
        std::vector<uint8_t>& d = s->in.front();
        if (d.size() > n) { s->in.pop_front(); return MBEDTLS_ERR_SSL_WANT_READ; }  // never happens: MTU-sized
        const size_t len = d.size();
        memcpy(buf, d.data(), len);
        s->in.pop_front();
        return (int)len;
    }
    static void keys_cb(void* v, mbedtls_ssl_key_export_type type,
                        const unsigned char* secret, size_t len,
                        const unsigned char client_random[32],
                        const unsigned char server_random[32],
                        mbedtls_tls_prf_types tls_prf_type) {
        Impl* s = (Impl*)v;
        if (type != MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET || len != 48) return;
        memcpy(s->master, secret, 48);
        memcpy(s->randoms, client_random, 32);
        memcpy(s->randoms + 32, server_random, 32);
        s->prf = tls_prf_type;
        s->have_secret = true;
    }
    // WebRTC verifies certificates by SDP fingerprint, not by a CA: accept at
    // the TLS layer (the caller compares fingerprints at the session layer).
    static int verify_cb(void*, mbedtls_x509_crt*, int, uint32_t* flags) {
        *flags = 0;
        return 0;
    }
};

namespace {

// Seed the DRBG straight from /dev/urandom. mbedtls_entropy_func would call
// getrandom() in BLOCKING mode, and this camera's kernel never credits its
// CRNG ("jitterentropy failed", DTRNG unbound) - so getrandom() blocks
// forever and, run from the HTTP poll thread on the first WebRTC offer,
// freezes the whole WebUI. /dev/urandom never blocks on Linux 4.4; for a
// LAN camera's per-session DTLS keys that is the right trade, and it is the
// best this box can offer anyway.
int urandom_entropy(void*, unsigned char* out, size_t len) {
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        const size_t got = fread(out, 1, len, f);
        fclose(f);
        if (got == len) return 0;
    }
    // Dev-host fallback ONLY (a box without /dev/urandom, i.e. the Windows
    // unit-test run): never reached on the camera, where the node always
    // exists. Keeps the in-memory handshake test runnable off-Linux.
    static bool seeded = false;
    if (!seeded) { srand((unsigned)time(nullptr) ^ (unsigned)(uintptr_t)out); seeded = true; }
    for (size_t i = 0; i < len; ++i) out[i] = (unsigned char)(rand() & 0xff);
    return 0;
}

std::string fingerprint_of(const uint8_t* der, size_t len) {
    uint8_t h[32];
    if (mbedtls_sha256(der, len, h, 0) != 0) return "";
    char buf[3 * 32 + 1];
    for (int i = 0; i < 32; ++i) snprintf(buf + 3 * i, 4, "%02X:", h[i]);
    buf[3 * 32 - 1] = 0;
    return buf;
}

} // namespace

DtlsTransport::DtlsTransport() : im_(new Impl) {
    Impl& s = *im_;
    mbedtls_entropy_init(&s.entropy);
    mbedtls_ctr_drbg_init(&s.drbg);
    mbedtls_pk_init(&s.key);
    mbedtls_x509_crt_init(&s.cert);
    mbedtls_ssl_config_init(&s.conf);
    mbedtls_ssl_init(&s.ssl);
    mbedtls_ssl_cookie_init(&s.cookie);

    const char* pers = "machino-dtls";
    // NON-BLOCKING entropy (see urandom_entropy): never mbedtls_entropy_func,
    // which would block forever on this kernel's uncredited getrandom().
    if (mbedtls_ctr_drbg_seed(&s.drbg, urandom_entropy, nullptr,
                              (const unsigned char*)pers, strlen(pers)) != 0) return;
    // EC P-256 key + self-signed certificate, regenerated per process start.
    if (mbedtls_pk_setup(&s.key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) return;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(s.key),
                            mbedtls_ctr_drbg_random, &s.drbg) != 0) return;
    mbedtls_x509write_cert wc;
    mbedtls_x509write_crt_init(&wc);
    mbedtls_x509write_crt_set_version(&wc, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&wc, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&wc, &s.key);
    mbedtls_x509write_crt_set_issuer_key(&wc, &s.key);
    unsigned char serial[8];
    mbedtls_ctr_drbg_random(&s.drbg, serial, sizeof serial);
    serial[0] &= 0x7f;                                    // positive
    int rc = mbedtls_x509write_crt_set_serial_raw(&wc, serial, sizeof serial);
    if (rc == 0) rc = mbedtls_x509write_crt_set_subject_name(&wc, "CN=machino");
    if (rc == 0) rc = mbedtls_x509write_crt_set_issuer_name(&wc, "CN=machino");
    if (rc == 0) rc = mbedtls_x509write_crt_set_validity(&wc, "20200101000000", "20400101000000");
    unsigned char der[1024];
    int n = rc == 0 ? mbedtls_x509write_crt_der(&wc, der, sizeof der,
                                                mbedtls_ctr_drbg_random, &s.drbg) : -1;
    mbedtls_x509write_crt_free(&wc);
    if (n <= 0) { LOGE(MOD, "certificate write failed (%d)", n); return; }
    const unsigned char* start = der + sizeof(der) - n;   // written at the end
    if (mbedtls_x509_crt_parse_der(&s.cert, start, (size_t)n) != 0) return;
    s.fp = fingerprint_of(start, (size_t)n);

    if (mbedtls_ssl_config_defaults(&s.conf, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) return;
    mbedtls_ssl_conf_rng(&s.conf, mbedtls_ctr_drbg_random, &s.drbg);
    mbedtls_ssl_conf_authmode(&s.conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_verify(&s.conf, Impl::verify_cb, &s);
    if (mbedtls_ssl_conf_own_cert(&s.conf, &s.cert, &s.key) != 0) return;
    // Cookie / HelloVerifyRequest: required, not optional - a real browser
    // ClientHello is ~1.4 KB and fragments across DTLS records; only the HVR
    // round-trip puts mbedtls into the stateful path that reassembles it (a
    // fragmented initial ClientHello without HVR fails with DECODE_ERROR).
    if (mbedtls_ssl_cookie_setup(&s.cookie, mbedtls_ctr_drbg_random, &s.drbg) != 0) return;
    mbedtls_ssl_conf_dtls_cookies(&s.conf, mbedtls_ssl_cookie_write,
                                  mbedtls_ssl_cookie_check, &s.cookie);
    static const mbedtls_ssl_srtp_profile profiles[] = {
        MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80,
        MBEDTLS_TLS_SRTP_UNSET,
    };
    if (mbedtls_ssl_conf_dtls_srtp_protection_profiles(&s.conf, profiles) != 0) return;

    if (mbedtls_ssl_setup(&s.ssl, &s.conf) != 0) return;
    mbedtls_ssl_set_export_keys_cb(&s.ssl, Impl::keys_cb, &s);
    mbedtls_ssl_set_timer_cb(&s.ssl, &s.timer, mbedtls_timing_set_delay, mbedtls_timing_get_delay);
    mbedtls_ssl_set_bio(&s.ssl, &s, Impl::bio_send, Impl::bio_recv, nullptr);
    s.ok = true;
}

DtlsTransport::~DtlsTransport() {
    Impl& s = *im_;
    mbedtls_ssl_cookie_free(&s.cookie);
    mbedtls_ssl_free(&s.ssl);
    mbedtls_ssl_config_free(&s.conf);
    mbedtls_x509_crt_free(&s.cert);
    mbedtls_pk_free(&s.key);
    mbedtls_ctr_drbg_free(&s.drbg);
    mbedtls_entropy_free(&s.entropy);
}

bool DtlsTransport::ok() const { return im_->ok; }
std::string DtlsTransport::fingerprint() const { return im_->fp; }
bool DtlsTransport::handshake_done() const { return im_->done; }

void DtlsTransport::set_peer(uint32_t ip_host, uint16_t port) {
    Impl& s = *im_;
    s.cli_id[0] = (uint8_t)(ip_host >> 24); s.cli_id[1] = (uint8_t)(ip_host >> 16);
    s.cli_id[2] = (uint8_t)(ip_host >> 8);  s.cli_id[3] = (uint8_t)ip_host;
    s.cli_id[4] = (uint8_t)(port >> 8);     s.cli_id[5] = (uint8_t)port;
    s.have_cli_id = true;
    mbedtls_ssl_set_client_transport_id(&s.ssl, s.cli_id, sizeof s.cli_id);
}

void DtlsTransport::feed(const uint8_t* p, size_t n) {
    if (n) im_->in.emplace_back(p, p + n);
}

bool DtlsTransport::step() {
    Impl& s = *im_;
    if (!s.ok || s.fatal) return false;
    if (s.done) return true;
    const int rc = mbedtls_ssl_handshake(&s.ssl);
    if (rc == 0) { s.done = true; return true; }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) return true;
    if (rc == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
        // Cookie sent; wait for the client to resend the ClientHello with it.
        mbedtls_ssl_session_reset(&s.ssl);
        if (s.have_cli_id) mbedtls_ssl_set_client_transport_id(&s.ssl, s.cli_id, sizeof s.cli_id);
        return true;
    }
    char err[96];
    mbedtls_strerror(rc, err, sizeof err);
    LOGW(MOD, "handshake failed: -0x%04x (%s)", (unsigned)-rc, err);
    s.fatal = true;
    return false;
}

bool DtlsTransport::take_out(std::vector<uint8_t>& dgram) {
    if (im_->out.empty()) return false;
    dgram = std::move(im_->out.front());
    im_->out.pop_front();
    return true;
}

bool DtlsTransport::export_srtp(SrtpKey& our_send, SrtpKey& their_send) {
    Impl& s = *im_;
    if (!s.done || !s.have_secret) return false;
    mbedtls_dtls_srtp_info info;
    mbedtls_ssl_get_dtls_srtp_negotiation_result(&s.ssl, &info);
    if (info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile) != MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80) {
        LOGW(MOD, "use_srtp not negotiated");
        return false;
    }
    // RFC 5764 4.2: 2 keys + 2 salts out of the TLS exporter, client first.
    uint8_t km[2 * 16 + 2 * 14];
    if (mbedtls_ssl_tls_prf(s.prf, s.master, 48, "EXTRACTOR-dtls_srtp",
                            s.randoms, 64, km, sizeof km) != 0) return false;
    SrtpKey client, server;
    memcpy(client.master_key, km, 16);
    memcpy(server.master_key, km + 16, 16);
    memcpy(client.master_salt, km + 32, 14);
    memcpy(server.master_salt, km + 46, 14);
    our_send = server;                                    // we are the DTLS server
    their_send = client;
    return true;
}

}} // namespace machino::webrtc
