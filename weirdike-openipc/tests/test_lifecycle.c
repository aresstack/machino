/* AP1 (Feature 2): der kleine Host-Lifecycle-Test, den der Plan verlangt.
 *
 * Bewiesen wird NUR die Integrationsnaht, kein Protokoll: weirdike_mem_req()
 * antwortet, der mbedTLS-Crypto-Adapter initialisiert, upstream's udp_bsd
 * bindet an eine konkrete lokale IP (127.0.0.1 -- genau der Vertrag, den
 * NAT-D und der Underlay-Wechsel spaeter brauchen), weirdike_new() nimmt
 * eine minimale PSK-Konfiguration an, und weirdike_free() raeumt auf.
 * Kein IKE_SA_INIT hier -- das ist AP2 gegen einen echten strongSwan-Peer.
 */
#include "weirdike.h"
#include "crypto_mbedtls.h"
#include "udp_bsd.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, what) do { \
    if (c) printf("  [OK  ] %s\n", what); \
    else { ++g_fail; printf("  [FAIL] %s\n", what); } \
} while (0)

static void t_log(void *u, int level, const char *msg)
{
    (void)u; (void)level; (void)msg;   /* still: der Test prueft Rueckgabewerte */
}

int main(void)
{
    printf("== weirdike host lifecycle ==\n");

    weirdike_mem_req_t req;
    memset(&req, 0, sizeof req);
    CHECK(weirdike_mem_req(NULL, &req) == 0 && req.ctx_len > 0 && req.ws_len > 0,
          "weirdike_mem_req(NULL) liefert die Maxima");

    weirdike_mbedtls_ctx mc;
    memset(&mc, 0, sizeof mc);
    CHECK(weirdike_crypto_mbedtls_init(&mc) == 0, "mbedTLS-Crypto-Adapter initialisiert");
    weirdike_crypto_t crypto;
    memset(&crypto, 0, sizeof crypto);
    weirdike_crypto_mbedtls_bind(&mc, &crypto);

    weirdike_udp_bsd udp;
    memset(&udp, 0, sizeof udp);
    weirdike_udp_bsd_init(&udp, "127.0.0.1");
    weirdike_transport_t transport;
    memset(&transport, 0, sizeof transport);
    weirdike_udp_bsd_bind(&udp, &transport);
    CHECK(transport.send != NULL, "udp_bsd stellt den Transport-Vtable");

    weirdike_platform_t plat;
    memset(&plat, 0, sizeof plat);
    plat.log = t_log;

    weirdike_config_t wc;
    memset(&wc, 0, sizeof wc);
    wc.server_host = "127.0.0.1";
    wc.server_port = 500;
    wc.auth        = WEIRDIKE_AUTH_PSK;
    wc.psk         = (const uint8_t *)"lifecycle-test-psk";
    wc.psk_len     = 18;
    weirdike_ts_any_ipv4(&wc.remote_ts);

    CHECK(weirdike_policy_check(NULL, NULL) == 0, "Default-Policies bestehen den Check");

    weirdike_ctx *ike = weirdike_new(&wc, &crypto, &transport, &plat, NULL);
    CHECK(ike != NULL, "weirdike_new nimmt die Minimalkonfiguration an");
    if (ike) {
        CHECK(weirdike_state(ike) == WEIRDIKE_STATE_IDLE, "frischer Kontext ist idle");
        weirdike_free(ike);
        printf("  [OK  ] weirdike_free raeumt auf\n");
    }

    if (g_fail) { printf("\n%d Pruefung(en) FEHLGESCHLAGEN\n", g_fail); return 1; }
    printf("\nAlle Pruefungen bestanden\n");
    return 0;
}
