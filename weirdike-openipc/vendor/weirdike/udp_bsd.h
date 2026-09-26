/*
 * WeirdIKE -- src/transport/udp_bsd.h : weirdike_transport_t on BSD/POSIX sockets.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Host + NuttX variant. The egress "underlay" is a concrete bind IP set at init (never 0.0.0.0),
 * which is exactly what NAT-D and WeirdOS' Issue-#6 source binding need. The ESP32/lwIP variant
 * (bind to modem-ecm/wifi-sta) lands with the WeirdOS integration.
 */
#ifndef WEIRDIKE_UDP_BSD_H
#define WEIRDIKE_UDP_BSD_H

#include "ike_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      fd;
    char     bind_ip[64];   /* concrete source IP to bind (the underlay); required for NAT-D */
    uint16_t local_port;    /* actual bound local port */
} weirdike_udp_bsd;

void weirdike_udp_bsd_init(weirdike_udp_bsd *u, const char *bind_ip);
void weirdike_udp_bsd_bind(weirdike_udp_bsd *u, weirdike_transport_t *out);

#ifdef __cplusplus
}
#endif

#endif /* WEIRDIKE_UDP_BSD_H */
