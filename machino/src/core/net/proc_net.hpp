// Parsing of the /proc and /etc files that describe the current IP setup.
//
// Separate from the adapter that reads them, for the same reason wpa_parse is
// separate from the socket: these formats are fixed-width, hex, byte-swapped
// and full of edge cases (no default route at all, several default routes, a
// route through an interface that is down), and every one of those can be
// exercised here against real captured text with no network involved.
//
// Nothing here opens a file. The adapter reads, these functions interpret.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace machino { namespace net {

// The default route (destination 0.0.0.0) out of /proc/net/route.
//
// The columns are little-endian hex of a network-order address, which is to
// say the bytes come out reversed twice and read back in the order you want
// them printed. Getting that wrong yields a plausible-looking gateway with
// the octets backwards, which is why it is tested rather than inlined.
struct DefaultRoute {
    std::string ifname;
    std::string gateway;      // dotted quad, "" for a link-local default route
    int         metric = 0;
};

// Every default route, in file order. A camera with Ethernet and LTE up at
// once really does have two, and which one wins is the metric's business --
// so this reports all of them instead of quietly picking one.
std::vector<DefaultRoute> parse_proc_net_route(const std::string& text);

// The lowest-metric default route, or false when there is none.
bool best_default_route(const std::string& text, DefaultRoute& out);

// nameserver lines from resolv.conf, in order, comments ignored.
std::vector<std::string> parse_resolv_conf(const std::string& text);

// rx_bytes / tx_bytes for one interface out of /proc/net/dev. False when the
// interface is not listed -- which is different from it being listed at zero.
bool parse_proc_net_dev(const std::string& text, const std::string& ifname,
                        uint64_t& rx_out, uint64_t& tx_out);

// A netmask in either form ("255.255.255.0" or "24") as a prefix length.
// Returns -1 on anything else, including a non-contiguous mask, which is not
// a thing Linux will accept either.
int netmask_to_prefix(const std::string& mask);

// The inverse, for reporting: 24 -> "255.255.255.0". "" when out of range.
std::string prefix_to_netmask(int prefix);

}} // namespace machino::net
