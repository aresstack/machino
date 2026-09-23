// Parsing of wpa_supplicant's control-interface replies.
//
// Deliberately separate from the socket code: a tab-separated table parser is
// exactly the kind of thing that fails quietly on an SSID containing a space,
// an empty field, or a hidden network with no name at all. Here it can be
// tested on the host against real captured output, with no radio involved.
#pragma once
#include <string>
#include <vector>

namespace machino { namespace net {

struct WpaScanEntry {
    std::string bssid;
    int         frequency_mhz = 0;
    int         signal_dbm = 0;
    std::string flags;
    std::string ssid;          // may be empty: hidden networks are real
};

// Parses the SCAN_RESULTS table. The header row is skipped; malformed lines
// are dropped rather than half-read.
std::vector<WpaScanEntry> parse_scan_results(const std::string& text);

// 2412 -> 1, 5180 -> 36. 0 when the frequency is not a channel we know.
int channel_for_frequency(int mhz);

// Maps the flag column to a WifiSecurity value (returned as int to keep this
// header free of the port). Conservative on purpose: an unrecognised flag set
// reports "open", because claiming WPA2 would make the UI demand a passphrase
// for a network that does not want one.
int security_from_flags(const std::string& flags);

// Parses the key=value block STATUS returns. Returns false when the key is
// absent; an absent key and an empty value are different answers.
bool wpa_status_field(const std::string& status, const std::string& key, std::string& out);

}} // namespace machino::net
