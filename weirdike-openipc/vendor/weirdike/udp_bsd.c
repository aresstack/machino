/*
 * WeirdIKE -- src/transport/udp_bsd.c : weirdike_transport_t on BSD/POSIX sockets.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "udp_bsd.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

static weirdike_udp_bsd *self(void *c) { return (weirdike_udp_bsd *)c; }

static int u_open(void *c, uint16_t port) {
    weirdike_udp_bsd *u = self(c);
    u->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (u->fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = u->bind_ip[0] ? inet_addr(u->bind_ip) : htonl(INADDR_ANY);
    if (bind(u->fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(u->fd); u->fd = -1; return -1; }
    struct sockaddr_in la; socklen_t ll = sizeof(la);
    u->local_port = (getsockname(u->fd, (struct sockaddr *)&la, &ll) == 0) ? ntohs(la.sin_port) : port;
    return 0;
}

static int u_resolve(void *c, const char *host, weirdike_endpoint_t *out) {
    (void)c;
    memset(out, 0, sizeof(*out));
    struct in_addr ia;
    if (inet_pton(AF_INET, host, &ia) == 1) { memcpy(out->ip, &ia, 4); return 0; }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return -1;
    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    memcpy(out->ip, &sa->sin_addr, 4);
    freeaddrinfo(res);
    return 0;
}

static int u_local(void *c, weirdike_endpoint_t *out) {
    weirdike_udp_bsd *u = self(c);
    memset(out, 0, sizeof(*out));
    if (!u->bind_ip[0]) return -1;                 /* must be concrete -- never 0.0.0.0 */
    struct in_addr ia;
    if (inet_pton(AF_INET, u->bind_ip, &ia) != 1) return -1;
    memcpy(out->ip, &ia, 4);
    out->port = u->local_port;
    return 0;
}

static int u_send(void *c, const weirdike_endpoint_t *d, const uint8_t *b, size_t n) {
    weirdike_udp_bsd *u = self(c);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(d->port);
    memcpy(&a.sin_addr, d->ip, 4);
    ssize_t s = sendto(u->fd, b, n, 0, (struct sockaddr *)&a, sizeof(a));
    return (s == (ssize_t)n) ? 0 : -1;
}

static int u_recv(void *c, weirdike_endpoint_t *src, uint8_t *buf, size_t buflen, int timeout_ms) {
    weirdike_udp_bsd *u = self(c);
    struct pollfd p; p.fd = u->fd; p.events = POLLIN; p.revents = 0;
    int pr = poll(&p, 1, timeout_ms);
    if (pr <= 0) return pr;                          /* 0 = nothing pending, <0 = error */
    struct sockaddr_in a; socklen_t al = sizeof(a);
    ssize_t n = recvfrom(u->fd, buf, buflen, 0, (struct sockaddr *)&a, &al);
    if (n < 0) return -1;
    if (src) {
        memset(src, 0, sizeof(*src));
        memcpy(src->ip, &a.sin_addr, 4);
        src->port = ntohs(a.sin_port);
    }
    return (int)n;
}

static void u_close(void *c) {
    weirdike_udp_bsd *u = self(c);
    if (u->fd >= 0) { close(u->fd); u->fd = -1; }
}

void weirdike_udp_bsd_init(weirdike_udp_bsd *u, const char *bind_ip) {
    memset(u, 0, sizeof(*u));
    u->fd = -1;
    if (bind_ip) { strncpy(u->bind_ip, bind_ip, sizeof(u->bind_ip) - 1); }
}

void weirdike_udp_bsd_bind(weirdike_udp_bsd *u, weirdike_transport_t *out) {
    memset(out, 0, sizeof(*out));
    out->ctx = u;
    out->open           = u_open;
    out->resolve        = u_resolve;
    out->local_endpoint = u_local;
    out->send           = u_send;
    out->recv           = u_recv;
    out->close          = u_close;
}
