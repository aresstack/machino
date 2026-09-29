#include "core/cellular/band_plan.hpp"

#include <cstdio>
#include <cstdlib>

namespace machino { namespace cellular {

const std::vector<LteBand>& lte_bands()
{
    // LTE_BANDS aus ec200a_modem.cpp, Reihenfolge und Werte unveraendert.
    static const std::vector<LteBand> kBands = {
        {  1, 2100,  0, false }, {  3, 1800,  2, false }, {  5,  850,  4, false },
        {  7, 2600,  6, false }, {  8,  900,  7, false }, { 20,  800, 19, false },
        { 28,  700, 27, false }, { 38, 2600, 37, true  }, { 40, 2300, 39, true  },
        { 41, 2500, 40, true  },
    };
    return kBands;
}

const LteBand* lte_band_by_number(int num)
{
    for (const LteBand& b : lte_bands()) if (b.num == num) return &b;
    return nullptr;
}

// ---------------------------------------------------------------- Netzmodus

const char* net_mode_name(NetMode m)
{
    switch (m) {
        case NetMode::LteOnly: return "lte";
        case NetMode::GsmOnly: return "gsm";
        case NetMode::Auto:    break;
    }
    return "auto";
}

bool net_mode_parse(const std::string& s, NetMode& out)
{
    if (s == "auto") { out = NetMode::Auto;    return true; }
    if (s == "lte")  { out = NetMode::LteOnly; return true; }
    if (s == "gsm")  { out = NetMode::GsmOnly; return true; }
    return false;
}

int net_mode_nwscanmode(NetMode m)
{
    // Die Werte aus modemNwScanMode(): 3 = nur LTE, 1 = nur GSM, 0 = auto.
    switch (m) {
        case NetMode::LteOnly: return 3;
        case NetMode::GsmOnly: return 1;
        case NetMode::Auto:    break;
    }
    return 0;
}

bool net_mode_from_nwscanmode(int v, NetMode& out)
{
    if (v == 0) { out = NetMode::Auto;    return true; }
    if (v == 3) { out = NetMode::LteOnly; return true; }
    if (v == 1) { out = NetMode::GsmOnly; return true; }
    return false;   // 2 (WCDMA) und alles andere: nicht unser Modus
}

// ------------------------------------------------------------------ Profile

const char* band_profile_name(BandProfile p)
{
    switch (p) {
        case BandProfile::Mid:    return "mid";
        case BandProfile::Low:    return "low";
        case BandProfile::Custom: return "custom";
        case BandProfile::Auto:   break;
    }
    return "auto";
}

bool band_profile_parse(const std::string& s, BandProfile& out)
{
    if (s == "auto")   { out = BandProfile::Auto;   return true; }
    if (s == "mid")    { out = BandProfile::Mid;    return true; }
    if (s == "low")    { out = BandProfile::Low;    return true; }
    if (s == "custom") { out = BandProfile::Custom; return true; }
    return false;
}

// ------------------------------------------------------------------- Masken

uint64_t all_supported_mask()
{
    uint64_t all = 0;
    for (const LteBand& b : lte_bands()) all |= (1ULL << b.bit);
    return all;
}

uint64_t profile_mask(BandProfile p, uint64_t custom_mask)
{
    switch (p) {
        case BandProfile::Mid:    return (1ULL << 2);                       // B3
        case BandProfile::Low:    return (1ULL << 19) | (1ULL << 7) |       // B20, B8
                                         (1ULL << 27) | (1ULL << 4);        // B28, B5
        case BandProfile::Custom: return custom_mask & all_supported_mask();
        case BandProfile::Auto:   break;
    }
    return all_supported_mask();
}

bool mask_from_bands(const std::vector<int>& bands, uint64_t& out, int& bad)
{
    uint64_t m = 0;
    for (int n : bands) {
        const LteBand* b = lte_band_by_number(n);
        if (!b) { bad = n; return false; }
        m |= (1ULL << b->bit);
    }
    out = m;
    bad = 0;
    return true;
}

std::vector<int> bands_from_mask(uint64_t mask)
{
    std::vector<int> out;
    for (const LteBand& b : lte_bands())
        if (mask & (1ULL << b.bit)) out.push_back(b.num);
    return out;
}

std::string mask_hex(uint64_t mask)
{
    char buf[24];
    std::snprintf(buf, sizeof buf, "%llx", (unsigned long long)mask);
    return buf;
}

bool parse_mask_hex(const std::string& in, uint64_t& out)
{
    std::string s;
    for (char c : in) if (c != ' ' && c != '"' && c != '\t') s += c;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty() || s.size() > 16) return false;
    for (char c : s) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    out = (uint64_t)::strtoull(s.c_str(), nullptr, 16);
    return true;
}

QcfgBand parse_qcfg_band(const std::string& raw)
{
    QcfgBand q;
    const std::string body = at_extract(raw, "+QCFG:");
    if (body.empty()) return q;
    if (csv_field(body, 0) != "band") return q;
    uint64_t v = 0;
    if (parse_mask_hex(csv_field(body, 1), v)) q.gsm = Maybe<uint64_t>(v);
    if (parse_mask_hex(csv_field(body, 2), v)) q.lte = Maybe<uint64_t>(v);
    return q;
}

}} // namespace machino::cellular
