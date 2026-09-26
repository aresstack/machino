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
    // Die vollstaendige Liste aus dem WeirdOS-ESP32-WebUI
    // (quectel-ec200a-eu/esp32-modem-host, web_ui_assets.cpp): die Standard-
    // APNs UND die beiden auf oeffentliche IPv4 getesteten Sonderprofile. Der
    // ESP-Dropdown liess telekompublic versehentlich weg -- hier ist es dabei.
    // Vorschlaege, keine Automatik: welcher Anbieter steckt, weiss machino nicht.
    static const std::vector<ApnPreset> kPresets = {
        { "o2", "o2 / Telefónica", "internet",
          PdpType::Ipv4v6, AuthMode::None, "", "",
          "o2/Telefónica Standard (Vertrag & Prepaid): APN internet, keine "
          "Zugangsdaten. Meist CGNAT (private 10.x) -- fuer Erreichbarkeit von "
          "aussen \"o2 (öffentliche IPv4)\" nehmen." },
        { "o2-netpublic", "o2 (öffentliche IPv4)", "netpublic",
          PdpType::Ipv4, AuthMode::None, "", "",
          "o2 netpublic = oeffentliche dynamische IPv4. PDP muss IP sein "
          "(IPv4-only); mit IPV4V6 scheitert die Einwahl auf manchen SIMs." },
        { "telekom", "Telekom", "internet.telekom",
          PdpType::Ipv4v6, AuthMode::Pap, "t-mobile", "tm",
          "Telekom Standard: APN internet.telekom, Benutzer t-mobile / Passwort "
          "tm (PAP). Liefert meist nur CGNAT -- fuer Erreichbarkeit "
          "\"Telekom (öffentliche IPv4)\" nehmen." },
        { "telekom-public", "Telekom (öffentliche IPv4)", "internet.t-d1.de",
          PdpType::Ipv4, AuthMode::None, "", "",
          "internet.telekom gibt nur eine CGNAT-Adresse (10.x/100.64.x); dieser "
          "APN liefert eine oeffentliche dynamische IPv4 (Pendant zu o2 "
          "netpublic). PDP muss IP sein, nicht IPV4V6. Genau das fuer Kamera/VPN "
          "nehmen, NICHT internet.telekom." },
        { "vodafone", "Vodafone", "web.vodafone.de",
          PdpType::Ipv4v6, AuthMode::None, "", "",
          "Vodafone: APN web.vodafone.de, keine Zugangsdaten." },
    };
    return kPresets;
}

}} // namespace machino::cellular
