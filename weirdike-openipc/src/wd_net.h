/*
 * weirdiked -- TUN device and the two IKE sockets.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Linux only. Kept apart from the daemon body so the protocol loop stays
 * readable and so the parts that need root are all in one place.
 */
#ifndef WD_NET_H
#define WD_NET_H

#include <stdint.h>
#include <stddef.h>

/* Open /dev/net/tun and attach `ifname` (IFF_TUN | IFF_NO_PI: raw IP, no
 * 4-byte prefix -- the ESP payload is exactly the IP packet).
 *
 * Returns the fd, or -1. On -1, err holds a reason. If /dev/net/tun is missing
 * the reason says so explicitly, because on this camera that is the expected
 * first failure: the tun module ships with the image but is not loaded. */
int wd_tun_open(const char *ifname, char *err, size_t errcap);

/* Bring the interface up and give it an address and MTU, via ioctl -- never
 * by shelling out to ifconfig. Returns 0 or -1. */
int wd_tun_configure(const char *ifname, const uint8_t ip[4], uint8_t prefix, int mtu,
                     char *err, size_t errcap);

/* A UDP socket bound to `port` on all addresses, non-blocking. -1 on failure. */
int wd_udp_open(uint16_t port, char *err, size_t errcap);

/* Which local address the kernel would use to reach `ip` -- a connected UDP
 * socket asked for its own name, no traffic sent. NAT-D needs the concrete
 * source address, never 0.0.0.0. Returns 0 or -1. */
int wd_source_addr_for(const uint8_t dst[4], uint8_t out[4]);

/* Resolve a hostname or IPv4 literal to one IPv4 address. Returns 0 or -1. */
int wd_resolve4(const char *host, uint8_t out[4]);

/* Monotonic milliseconds, wrapping like the core expects (uint32_t). */
uint32_t wd_now_ms(void);

#endif /* WD_NET_H */
