#include "core/cellular/cellular_config.hpp"

namespace machino { namespace cellular {

const char* pdp_type_name(PdpType t)
{
    return t == PdpType::Ipv4v6 ? "IPV4V6" : "IP";
}

bool pdp_type_parse(const std::string& s, PdpType& out)
{
    if (s == "IP"     || s == "ip")     { out = PdpType::Ipv4;   return true; }
    if (s == "IPV4V6" || s == "ipv4v6") { out = PdpType::Ipv4v6; return true; }
    return false;
}

const char* auth_mode_name(AuthMode a)
{
    switch (a) {
        case AuthMode::Pap:  return "pap";
        case AuthMode::Chap: return "chap";
        case AuthMode::None: break;
    }
    return "none";
}

bool auth_mode_parse(const std::string& s, AuthMode& out)
{
    if (s == "none" || s == "NONE") { out = AuthMode::None; return true; }
    if (s == "pap"  || s == "PAP")  { out = AuthMode::Pap;  return true; }
    if (s == "chap" || s == "CHAP") { out = AuthMode::Chap; return true; }
    return false;
}

const std::vector<ApnPreset>& apn_presets()
{
    static const std::vector<ApnPreset> kPresets = {
        { "telekom-public", "Telekom (öffentliche IPv4)", "internet.t-d1.de",
          PdpType::Ipv4, AuthMode::None,
          "Die Standard-APNs der Telekom geben nur eine CGNAT-Adresse (10.x/100.64.x). "
          "Dieser APN liefert eine öffentliche IPv4; PDP muss IP sein, nicht IPV4V6." },
        { "o2-netpublic", "o2 (öffentliche IPv4)", "netpublic",
          PdpType::Ipv4, AuthMode::None,
          "Der APN \"internet\" gibt bei o2 nur 10.x. netpublic liefert eine "
          "öffentliche IPv4." },
    };
    return kPresets;
}

}} // namespace machino::cellular
