// AP5: die Bruecke ConnectivityManager -> IIpsecUplinks. Ein AUSZUG aus der
// bestehenden Uplink-Wahrheit, keine zweite Policy: "auto" ist, was die
// Machino-Policy JETZT als aktiven Uplink faehrt; ein konkreter Wunsch
// (cellular/ethernet/wifi) ist genau der eine Uplink dieses Typs -- nutzbar
// oder ehrlich nicht. Interface-Namen kommen aus dem Live-Status (ppp0,
// usb0, eth1, ...), nie aus einer Konstante.
#pragma once
#include "core/net/connectivity.hpp"
#include "core/net/ipsec_service.hpp"

namespace machino { namespace ipsec {

class ConnectivityIpsecUplinks : public IIpsecUplinks {
public:
    explicit ConnectivityIpsecUplinks(net::ConnectivityManager& conn) : conn_(conn) {}

    bool select(Underlay wanted, UnderlayView& out, std::string& err) override
    {
        out = UnderlayView{};
        const std::vector<net::UplinkStatus> all = conn_.status();

        if (wanted == Underlay::Auto) {
            for (const auto& u : all) {
                if (!u.active) continue;
                fill(u, out);
                if (!out.usable) err = "aktiver Uplink '" + u.id + "' hat keine Adresse";
                return out.usable;
            }
            err = "kein aktiver Uplink";
            return false;
        }

        const net::UplinkType want = wanted == Underlay::Cellular ? net::UplinkType::Cellular
                              : wanted == Underlay::Wifi     ? net::UplinkType::Wifi
                                                             : net::UplinkType::Ethernet;
        for (const auto& u : all) {
            if (u.type != want) continue;
            fill(u, out);
            if (!out.usable)
                err = std::string("underlay '") + underlay_name(wanted)
                      + "': nicht verbunden (" + u.id + ")";
            return out.usable;
        }
        err = std::string("underlay '") + underlay_name(wanted) + "': kein solcher Uplink";
        return false;
    }

private:
    static void fill(const net::UplinkStatus& u, UnderlayView& out)
    {
        out.kind = net::uplink_type_name(u.type);
        out.ifname = u.info.ifname;
        out.ipv4 = u.info.ipv4;
        out.gateway = u.info.gateway;
        out.usable = u.state == net::LinkState::Connected && !out.ipv4.empty()
                     && !out.ifname.empty();
    }

    net::ConnectivityManager& conn_;
};

}} // namespace machino::ipsec
