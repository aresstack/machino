// The /api/v1/usb and /api/v1/network surfaces.
//
// Separate from ApiService on purpose. ApiService is the MEDIA surface --
// capabilities, state, config, telemetry, snapshot -- and it is already large.
// Nothing here may reach the pipeline, the encoder or IMP, and keeping the two
// in different objects is the cheapest way to keep that true: this class has
// no reference to any of them.
//
// Everything it needs is optional. A build with no WiFi adapter, no USB
// backend or no transaction store still answers, with 404 and a reason,
// instead of pretending the feature exists and failing at the hardware.
//
// Host-testable in full: the services it talks to are interfaces, so the whole
// routing table, every status code and every rejection is exercised without a
// socket or a camera.
//
// Secrets: a passphrase arrives in a POST body and is never read back. No
// document produced here contains one, no log line here contains one, and the
// confirmed/pending records go through IStateStore, which writes 0600.
#pragma once
#include "app/api/api_service.hpp"
#include "app/api/net_views.hpp"
#include "core/net/cellular_uplink.hpp"
#include "core/net/connectivity.hpp"
#include "core/devices/device_package.hpp"
#include "core/net/network_txn.hpp"
#include "core/usb/usb_host_service.hpp"
#include "ports/inetwork.hpp"
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace machino { namespace api {

class NetApiService {
public:
    // Milliseconds from the runtime's monotonic clock -- the same source the
    // caller later feeds to tick(), or the confirmation window is measured
    // against a different timeline than it is enforced on.
    using ClockFn = std::function<uint32_t()>;

    // Persist the USB configuration. Returns false with a reason; the API then
    // reports the failure instead of silently applying something that will be
    // gone after a reboot.
    using SaveUsbFn = std::function<bool(const usb::UsbConfig&, std::string& err)>;

    // Persist the uplink policy. Same contract.
    using SavePolicyFn = std::function<bool(const net::UplinkPolicy&, std::string& err)>;


    struct Deps {
        usb::UsbHostService*      usb = nullptr;
        net::ConnectivityManager* conn = nullptr;
        net::IWifiAdapter*        wifi = nullptr;
        net::CellularUplink*      cellular = nullptr;
        net::NetworkTxn*          txn = nullptr;
        // Der Geraetemanager (WLAN-/Modem-Pakete). Optional wie alles hier: ein
        // Build ohne ihn antwortet mit 404 und Begruendung statt so zu tun.
        devices::DeviceManager*   devices = nullptr;
        ClockFn                   now_ms;

        // Called AFTER a staged change has been confirmed, with the candidate
        // that was confirmed.
        //
        // This is where a change becomes permanent. It matters for the cellular
        // configuration and it is the difference between a rollback that works
        // and one that only looks like it: the stored configuration is the
        // source of truth, an unconfirmed change deviates from it live, and
        // every apply -- including the rollback's -- resets to it first. If the
        // store were written when the change was APPLIED instead, there would
        // be nothing left to roll back to.
        using ConfirmedFn = std::function<void(const std::string& candidate)>;
        ConfirmedFn               on_confirmed;
        SaveUsbFn                 save_usb;
        SavePolicyFn              save_policy;

        // How long the user has to confirm a network change before it is
        // rolled back. Long enough to reconnect over the new configuration and
        // click a button, short enough that an unattended camera recovers on
        // its own.
        uint32_t confirm_window_ms = 120000;
    };

    explicit NetApiService(Deps d);

    // Returns false when the path is none of ours, so the caller can carry on
    // with its own routing table rather than getting a 404 it has to unpick.
    bool handle(const std::string& method, const std::string& path,
                const std::string& body, Response& out);

    // Roll back an unconfirmed change once its window has passed. Driven from
    // the same place that ticks the rest of the runtime.
    bool tick();

private:
    Response usb_get() const;
    Response usb_patch(const std::string& body);
    Response usb_devices() const;
    // Geraetemanager: Liste und die beiden Aktionen. Installieren aktiviert
    // nicht -- der gemeinsame Port wird weiter ueber /api/v1/usb gesetzt.
    Response devices_get() const;
    Response devices_action(const std::string& id, bool install);

    Json     net_views_policy() const;
    Response network_get() const;
    Response policy_patch(const std::string& body);
    Response wifi_get() const;
    Response wifi_scan();
    Response wifi_station(const std::string& body);
    Response wifi_ap(const std::string& body);
    Response cellular_get() const;
    Response cellular_patch(const std::string& body);
    Response cellular_presets() const;
    Response change_get() const;
    Response change_confirm(const std::string& token_text);

    // Starts a staged change carrying `candidate` (the request body verbatim,
    // so a rollback re-applies exactly what was confirmed before).
    Response stage(const std::string& path, const std::string& candidate);

    Deps       d_;
    std::mutex m_;          // one network change at a time, like the media PATCH

    // The candidate currently staged. NetworkTxn allows exactly one at a
    // time, so this is unambiguous -- and it saves reading the pending record
    // back out of the store just to learn what was confirmed.
    std::string staged_candidate_;
};

}} // namespace machino::api
