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

// ------------------------------------------- building control commands
//
// The control interface is a LINE protocol. An SSID or a passphrase that
// contains a newline would end the command and start another one, so a WiFi
// form on the web page would be a way to issue arbitrary wpa_supplicant
// commands. These two are the only sanctioned way to put user text into a
// command, and both refuse rather than sanitise -- silently dropping a
// character from a passphrase produces a camera that cannot associate and no
// explanation anywhere.

// Hex-encodes a value for the unquoted form: SET_NETWORK 0 ssid 48656c6c6f.
// Preferred for the SSID, which is arbitrary bytes by specification and may
// legitimately contain quotes, backslashes and non-UTF-8. Never fails.
std::string wpa_hex(const std::string& raw);

// Quotes a value for the quoted form. Returns false on anything that cannot
// safely be quoted: a newline, a carriage return or any other control
// character. Used for the passphrase, which has no hex form.
bool wpa_quote(const std::string& raw, std::string& out);

}} // namespace machino::net
