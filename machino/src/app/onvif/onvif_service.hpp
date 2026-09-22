// ONVIF minimal stack: authentication and the Device/Media operations a
// client needs to find this camera and pull its stream.
//
// Scope is deliberate. This implements the operations without which no client
// works at all, and no more - there is no PTZ, no events, no imaging, no
// recording and no snapshot (Machino has no JPEG path by mandate). An
// operation that is not implemented answers a proper SOAP fault rather than a
// wrong answer.
//
// Pure logic, host-testable: no sockets, no clock of its own, no system
// access. The credential check, the time and the stream facts are all
// injected, so every branch below - including every way authentication can
// fail - is exercised by the host tests.
#pragma once
#include "app/onvif/soap.hpp"
#include "core/config.hpp"
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace machino { namespace onvif {

struct DeviceInfo {
    std::string manufacturer = "OpenIPC";
    std::string model        = "Machino";
    std::string firmware;
    std::string serial;
    std::string hardware_id;
};

// One media profile, as the pipeline currently has it.
struct MediaProfile {
    std::string token;           // stable identifier the client will send back
    std::string name;
    std::string encoding = "H264";
    int width = 0, height = 0, fps = 0, bitrate_kbps = 0;
    std::string rtsp_path;       // "/ch0"
};

enum class AuthResult {
    Ok,
    Missing,        // no credential offered at all
    Bad,            // offered and wrong
    Stale,          // digest whose Created is outside the window, or a replay
    Unverifiable,   // a PasswordDigest with no cleartext password configured
    Unclaimed,      // the camera has not been set up; nothing is authorised
};

const char* auth_result_name(AuthResult r);

class OnvifService {
public:
    using CheckFn = std::function<bool(const std::string& user, const std::string& pass)>;

    struct Request {
        std::string path;
        std::string body;
        std::string authorization;   // HTTP Authorization header, if any
        std::string host;            // "192.168.1.10" - for the URLs handed back
    };
    struct Response {
        int         status = 200;
        std::string content_type = "application/soap+xml; charset=utf-8";
        std::string body;
    };

    OnvifService(const OnvifConfig& cfg, CheckFn check);

    void set_device(const DeviceInfo& d)                  { dev_ = d; }
    void set_profiles(const std::vector<MediaProfile>& p) { profiles_ = p; }
    void set_ports(int http_port, int rtsp_port)          { http_port_ = http_port; rtsp_port_ = rtsp_port; }
    // From AP10: while the camera is unclaimed it streams nothing and ONVIF is
    // unauthorised. From the majestic system section: unsafe turns every check
    // off, unclaimed included.
    void set_claimed(bool c)  { claimed_ = c; }
    void set_unsafe(bool u)   { unsafe_ = u; }

    static bool is_onvif_path(const std::string& path);

    // now_unix is injected so the digest freshness window is testable.
    AuthResult authenticate(const std::string& xml, const std::string& authorization,
                            int64_t now_unix);

    Response handle(const Request& req, int64_t now_unix);

    // A PasswordDigest whose Created is further than this from our clock is
    // refused. Five minutes is the usual ONVIF tolerance and is what keeps a
    // captured digest from being replayable for long.
    static const int64_t CLOCK_SKEW_S = 300;

    // Exposed for tests: Base64(SHA1(nonce || created || password)).
    static std::string password_digest(const std::string& nonce_raw,
                                       const std::string& created,
                                       const std::string& password);
    // xsd:dateTime in UTC, which is the only form ONVIF accepts here.
    static std::string utc_datetime(int64_t unix_s);
    static bool parse_utc_datetime(const std::string& s, int64_t& unix_s);

private:
    std::string device_information() const;
    std::string capabilities(const std::string& host) const;
    std::string services(const std::string& host) const;
    std::string scopes() const;
    std::string system_date_and_time(int64_t now_unix) const;
    std::string profiles_xml() const;
    std::string profile_xml(const MediaProfile& p) const;
    std::string stream_uri(const MediaProfile& p, const std::string& host) const;
    std::string video_sources() const;
    std::string encoder_configurations() const;
    std::string xaddr(const std::string& host, const char* service) const;

    bool seen_nonce(const std::string& nonce_b64, int64_t now_unix);

    OnvifConfig               cfg_;
    CheckFn                   check_;
    DeviceInfo                dev_;
    std::vector<MediaProfile> profiles_;
    int                       http_port_ = 80;
    int                       rtsp_port_ = 554;
    bool                      claimed_ = true;
    bool                      unsafe_  = false;
    // Replay window. Bounded: a digest is only valid for CLOCK_SKEW_S anyway,
    // so the cache never has to outlive that, and the cap stops a flood of
    // distinct nonces from growing it without limit.
    static const size_t MAX_NONCES = 256;
    std::deque<std::pair<std::string, int64_t>> nonces_;
};

}} // namespace machino::onvif
