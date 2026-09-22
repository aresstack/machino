#include "app/onvif/onvif_service.hpp"
#include "app/http/websocket.hpp"     // ws::sha1, ws::base64
#include "app/rtsp/rtsp_auth.hpp"      // RtspAuth::digest_response, auth_param - already host-tested

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace machino { namespace onvif {

const char* const OnvifService::DIGEST_REALM = "Machino";

const char* auth_result_name(AuthResult r) {
    switch (r) {
        case AuthResult::Ok:           return "ok";
        case AuthResult::Missing:      return "missing";
        case AuthResult::Bad:          return "bad";
        case AuthResult::Stale:        return "stale";
        case AuthResult::Unverifiable: return "unverifiable";
        case AuthResult::Unclaimed:    return "unclaimed";
    }
    return "?";
}

OnvifService::OnvifService(const OnvifConfig& cfg, CheckFn check, std::string nonce_secret)
    : cfg_(cfg), check_(std::move(check)), nonce_secret_(std::move(nonce_secret)) {}

bool OnvifService::is_onvif_path(const std::string& path) {
    return path.compare(0, 7, "/onvif/") == 0;
}

std::string OnvifService::utc_datetime(int64_t unix_s) {
    const time_t t = (time_t)unix_s;
    struct tm tm_buf;
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    // Sized for what the compiler must ASSUME, not for what a clock produces:
    // every %d here can in principle be 11 characters, and -Wformat-truncation
    // is right to insist the buffer cover that.
    char b[96];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return b;
}

// Only the UTC form, which is the only one ONVIF clients send for Created.
// A value with an offset, or one that is not a date at all, is refused rather
// than guessed at - a misparsed timestamp would silently widen the replay
// window.
bool OnvifService::parse_utc_datetime(const std::string& s, int64_t& unix_s) {
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    if (s.size() < 20) return false;
    if (sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return false;
    if (s.back() != 'Z') return false;
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec > 60) return false;
    // days-from-civil (Howard Hinnant), so this needs no timegm and no TZ.
    int y = Y;
    y -= M <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = (long long)era * 146097 + (long long)doe - 719468;
    unix_s = days * 86400 + h * 3600 + m * 60 + sec;
    return true;
}

std::string OnvifService::password_digest(const std::string& nonce_raw,
                                          const std::string& created,
                                          const std::string& password) {
    std::string buf = nonce_raw + created + password;
    uint8_t d[20];
    ws::sha1((const uint8_t*)buf.data(), buf.size(), d);
    return ws::base64(d, 20);
}

bool OnvifService::seen_nonce(const std::string& nonce_b64, int64_t now_unix) {
    // Drop anything older than the window before looking: a nonce that can no
    // longer produce a fresh digest cannot be replayed either.
    while (!nonces_.empty() && nonces_.front().second < now_unix - CLOCK_SKEW_S)
        nonces_.pop_front();
    for (const auto& n : nonces_) if (n.first == nonce_b64) return true;
    if (nonces_.size() >= MAX_NONCES) nonces_.pop_front();
    nonces_.push_back({nonce_b64, now_unix});
    return false;
}

// A nonce the camera need not remember and a client cannot forge: the
// timestamp is in the clear so freshness is checkable, and the tag binds it to
// the configured password so a client cannot mint one of its own. This is what
// lets Digest work without server-side session state on a box with 42 MB of
// RAM.
std::string OnvifService::make_nonce(int64_t now_unix) const {
    char ts[32];
    snprintf(ts, sizeof ts, "%lld", (long long)now_unix);
    // Keyed with a PROCESS SECRET, never with the password: challenge() hands
    // this value to any unauthenticated caller, so keying it with the password
    // would publish sha1(known-prefix || password) to the whole network - a
    // single-iteration offline oracle. Found in review, after shipping it.
    const std::string material = std::string(ts) + ":" + DIGEST_REALM + ":" + nonce_secret_;
    uint8_t d[20];
    ws::sha1((const uint8_t*)material.data(), material.size(), d);
    char tag[33];
    for (int i = 0; i < 16; ++i) snprintf(tag + i * 2, 3, "%02x", d[i]);
    return std::string(ts) + "." + tag;
}

bool OnvifService::nonce_ok(const std::string& nonce, int64_t now_unix) const {
    const size_t dot = nonce.find('.');
    if (dot == std::string::npos || dot == 0) return false;
    const std::string ts = nonce.substr(0, dot);
    for (char c : ts) if (c < '0' || c > '9') return false;
    if (ts.size() > 18) return false;
    const int64_t issued = (int64_t)strtoll(ts.c_str(), nullptr, 10);
    const int64_t skew = issued > now_unix ? issued - now_unix : now_unix - issued;
    if (skew > CLOCK_SKEW_S) return false;
    // Recompute rather than compare against anything stored.
    return make_nonce(issued) == nonce;
}

std::string OnvifService::challenge(int64_t now_unix) const {
    if (unsafe_) return "";
    std::string out;
    // Digest needs BOTH the cleartext (for HA1) and a nonce secret (so the
    // challenge is not an oracle). Without either it is not offered, because
    // inviting a client to try something that cannot succeed helps nobody.
    if (digest_available()) {
        out += "WWW-Authenticate: Digest realm=\"" + std::string(DIGEST_REALM) +
               "\", nonce=\"" + make_nonce(now_unix) + "\", qop=\"auth\"\r\n";
    }
    out += "WWW-Authenticate: Basic realm=\"" + std::string(DIGEST_REALM) + "\"\r\n";
    return out;
}

AuthResult OnvifService::authenticate(const std::string& xml, const std::string& authorization,
                                      int64_t now_unix, const std::string& method,
                                      const std::string& path) {
    // system.unsafe turns authentication off everywhere, unclaimed included.
    if (unsafe_) return AuthResult::Ok;
    // An unclaimed camera authorises nothing at all - there is no credential
    // yet, so there is nothing that could be right.
    if (!claimed_) return AuthResult::Unclaimed;

    const bool have_cleartext = !cfg_.password.empty();

    WsseToken tok;
    if (wsse_token(xml, tok) && tok.present) {
        if (tok.username != cfg_.username) return AuthResult::Bad;

        if (!tok.digest) {
            // PasswordText: the cleartext is on the wire, so it can be checked
            // against either source.
            if (have_cleartext) return secure_equals(tok.password, cfg_.password) ? AuthResult::Ok : AuthResult::Bad;
            if (check_ && check_(tok.username, tok.password)) return AuthResult::Ok;
            return AuthResult::Bad;
        }

        // PasswordDigest needs the cleartext to recompute the hash. /etc/shadow
        // cannot supply it, so without a configured password this is not a
        // wrong credential - it is one we are unable to judge, and saying so
        // is more useful than calling it bad.
        if (!have_cleartext) return AuthResult::Unverifiable;
        if (tok.created.empty() || tok.nonce_b64.empty()) return AuthResult::Bad;

        int64_t created = 0;
        if (!parse_utc_datetime(tok.created, created)) return AuthResult::Bad;
        const int64_t skew = created > now_unix ? created - now_unix : now_unix - created;
        if (skew > CLOCK_SKEW_S) return AuthResult::Stale;

        std::string nonce_raw;
        if (!b64_decode(tok.nonce_b64, nonce_raw)) return AuthResult::Bad;

        // Compare BEFORE recording the nonce, so a wrong password cannot be
        // used to burn a nonce the real client is about to send.
        if (!secure_equals(password_digest(nonce_raw, tok.created, cfg_.password), tok.password))
            return AuthResult::Bad;
        if (seen_nonce(tok.nonce_b64, now_unix)) return AuthResult::Stale;
        return AuthResult::Ok;
    }

    // HTTP Digest. Upstream's own hint says the cleartext password is what
    // "unlocks WSSE PasswordDigest and HTTP Digest auth so legacy clients
    // (ODM v2.2.x) and digest-only ones (tinyCam Monitor) work" - this is the
    // second half of that. The hashing is RtspAuth's, which is already
    // host-tested, rather than a second implementation that can drift.
    if (authorization.compare(0, 7, "Digest ") == 0) {
        // No cleartext, or no nonce secret to have issued a genuine nonce
        // with: either way this cannot be judged, and it is not the client's
        // fault. Fail closed.
        if (!digest_available()) return AuthResult::Unverifiable;
        const std::string user  = RtspAuth::auth_param(authorization, "username");
        const std::string realm = RtspAuth::auth_param(authorization, "realm");
        const std::string nonce = RtspAuth::auth_param(authorization, "nonce");
        const std::string uri   = RtspAuth::auth_param(authorization, "uri");
        const std::string resp  = RtspAuth::auth_param(authorization, "response");
        const std::string qop   = RtspAuth::auth_param(authorization, "qop");
        const std::string nc    = RtspAuth::auth_param(authorization, "nc");
        const std::string cnonce = RtspAuth::auth_param(authorization, "cnonce");
        if (user != cfg_.username) return AuthResult::Bad;
        if (realm != DIGEST_REALM) return AuthResult::Bad;
        if (resp.empty() || uri.empty()) return AuthResult::Bad;
        // The uri is part of the hash but it is the CLIENT that states it, so a
        // digest computed for one resource would otherwise authorise another.
        // Accept the bare path or an absolute URL ending in it - some clients
        // send each - but not an unrelated one. Skipped when the caller did
        // not supply a path, so direct callers keep working.
        if (!path.empty() && uri != path &&
            !(uri.size() > path.size() && uri.compare(uri.size() - path.size(), path.size(), path) == 0))
            return AuthResult::Bad;
        // An unrecognised or expired nonce is Stale, not Bad: the client is
        // told to retry with the fresh one in the challenge rather than being
        // sent away as if its password were wrong.
        if (!nonce_ok(nonce, now_unix)) return AuthResult::Stale;
        if (!secure_equals(RtspAuth::digest_response(user, cfg_.password, realm, nonce,
                                                      method, uri, nc, cnonce, qop), resp))
            return AuthResult::Bad;
        // Replay: the same nonce may be reused with an increasing nc, so the
        // pair is what must be unique. Recorded only after the response
        // verified, for the same reason as the WSSE path.
        if (seen_nonce("d:" + nonce + ":" + nc, now_unix)) return AuthResult::Stale;
        return AuthResult::Ok;
    }

    // HTTP Basic, which some clients use instead of WS-Security.
    if (authorization.compare(0, 6, "Basic ") == 0) {
        std::string plain;
        if (b64_decode(authorization.substr(6), plain)) {
            const size_t c = plain.find(':');
            if (c != std::string::npos) {
                const std::string u = plain.substr(0, c), p = plain.substr(c + 1);
                if (u != cfg_.username) return AuthResult::Bad;
                if (have_cleartext) return secure_equals(p, cfg_.password) ? AuthResult::Ok : AuthResult::Bad;
                if (check_ && check_(u, p)) return AuthResult::Ok;
            }
        }
        return AuthResult::Bad;
    }
    return AuthResult::Missing;
}

std::string OnvifService::xaddr(const std::string& host, const char* service) const {
    char b[256];
    snprintf(b, sizeof b, "http://%s:%d/onvif/%s", host.c_str(), http_port_, service);
    return b;
}

std::string OnvifService::system_date_and_time(int64_t now_unix) const {
    const time_t t = (time_t)now_unix;
    struct tm g;
#if defined(_WIN32)
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[1024];
    // DateTimeType Manual and DaylightSavings false: Machino does not run an
    // NTP client of its own, so claiming NTP here would be a lie a client can
    // act on.
    snprintf(b, sizeof b,
             "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>"
             "<tt:DateTimeType>Manual</tt:DateTimeType>"
             "<tt:DaylightSavings>false</tt:DaylightSavings>"
             "<tt:UTCDateTime><tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute>"
             "<tt:Second>%d</tt:Second></tt:Time>"
             "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>"
             "</tt:UTCDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>",
             g.tm_hour, g.tm_min, g.tm_sec, g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    return b;
}

std::string OnvifService::device_information() const {
    return "<tds:GetDeviceInformationResponse>"
           "<tds:Manufacturer>" + xml_escape(dev_.manufacturer) + "</tds:Manufacturer>"
           "<tds:Model>" + xml_escape(dev_.model) + "</tds:Model>"
           "<tds:FirmwareVersion>" + xml_escape(dev_.firmware) + "</tds:FirmwareVersion>"
           "<tds:SerialNumber>" + xml_escape(dev_.serial) + "</tds:SerialNumber>"
           "<tds:HardwareId>" + xml_escape(dev_.hardware_id) + "</tds:HardwareId>"
           "</tds:GetDeviceInformationResponse>";
}

std::string OnvifService::capabilities(const std::string& host) const {
    // Only Device and Media are advertised, because only those are
    // implemented. Advertising Events or PTZ here would have clients call
    // operations that answer a fault.
    return "<tds:GetCapabilitiesResponse><tds:Capabilities>"
           "<tt:Device><tt:XAddr>" + xml_escape(xaddr(host, "device_service")) + "</tt:XAddr>"
           "<tt:System><tt:DiscoveryResolve>false</tt:DiscoveryResolve>"
           "<tt:DiscoveryBye>false</tt:DiscoveryBye>"
           "<tt:RemoteDiscovery>false</tt:RemoteDiscovery>"
           "<tt:SystemBackup>false</tt:SystemBackup>"
           "<tt:SystemLogging>false</tt:SystemLogging>"
           "<tt:FirmwareUpgrade>false</tt:FirmwareUpgrade></tt:System></tt:Device>"
           "<tt:Media><tt:XAddr>" + xml_escape(xaddr(host, "media_service")) + "</tt:XAddr>"
           "<tt:StreamingCapabilities>"
           "<tt:RTPMulticast>false</tt:RTPMulticast>"
           "<tt:RTP_TCP>true</tt:RTP_TCP>"
           "<tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>"
           "</tt:StreamingCapabilities></tt:Media>"
           "</tds:Capabilities></tds:GetCapabilitiesResponse>";
}

std::string OnvifService::services(const std::string& host) const {
    return "<tds:GetServicesResponse>"
           "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>"
           "<tds:XAddr>" + xml_escape(xaddr(host, "device_service")) + "</tds:XAddr>"
           "<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
           "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>"
           "<tds:XAddr>" + xml_escape(xaddr(host, "media_service")) + "</tds:XAddr>"
           "<tds:Version><tt:Major>2</tt:Major><tt:Minor>5</tt:Minor></tds:Version></tds:Service>"
           "</tds:GetServicesResponse>";
}

std::string OnvifService::scopes() const {
    const char* fixed[] = {
        "onvif://www.onvif.org/type/video_encoder",
        "onvif://www.onvif.org/Profile/Streaming",
    };
    std::string out = "<tds:GetScopesResponse>";
    for (const char* s : fixed)
        out += "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef>"
               "<tt:ScopeItem>" + xml_escape(s) + "</tt:ScopeItem></tds:Scopes>";
    out += "<tds:Scopes><tt:ScopeDef>Configurable</tt:ScopeDef><tt:ScopeItem>"
           + xml_escape("onvif://www.onvif.org/name/" + dev_.model) + "</tt:ScopeItem></tds:Scopes>";
    out += "</tds:GetScopesResponse>";
    return out;
}

std::string OnvifService::profile_xml(const MediaProfile& p) const {
    char geom[640];
    snprintf(geom, sizeof geom,
             "<tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>"
             "<tt:Quality>4</tt:Quality>"
             "<tt:RateControl><tt:FrameRateLimit>%d</tt:FrameRateLimit>"
             "<tt:EncodingInterval>1</tt:EncodingInterval>"
             "<tt:BitrateLimit>%d</tt:BitrateLimit></tt:RateControl>",
             p.width, p.height, p.fps, p.bitrate_kbps);
    return "<tt:Profiles token=\"" + xml_escape(p.token) + "\" fixed=\"true\">"
           "<tt:Name>" + xml_escape(p.name) + "</tt:Name>"
           "<tt:VideoSourceConfiguration token=\"vsc0\">"
           "<tt:Name>VideoSource</tt:Name><tt:UseCount>1</tt:UseCount>"
           "<tt:SourceToken>vs0</tt:SourceToken>"
           "<tt:Bounds x=\"0\" y=\"0\" width=\"" + std::to_string(p.width) +
           "\" height=\"" + std::to_string(p.height) + "\"/>"
           "</tt:VideoSourceConfiguration>"
           "<tt:VideoEncoderConfiguration token=\"vec-" + xml_escape(p.token) + "\">"
           "<tt:Name>" + xml_escape(p.name) + "</tt:Name><tt:UseCount>1</tt:UseCount>"
           "<tt:Encoding>" + xml_escape(p.encoding) + "</tt:Encoding>"
           + geom +
           "<tt:SessionTimeout>PT60S</tt:SessionTimeout>"
           "</tt:VideoEncoderConfiguration>"
           "</tt:Profiles>";
}

std::string OnvifService::profiles_xml() const {
    std::string out = "<trt:GetProfilesResponse>";
    for (const MediaProfile& p : profiles_) out += profile_xml(p);
    out += "</trt:GetProfilesResponse>";
    return out;
}

std::string OnvifService::stream_uri(const MediaProfile& p, const std::string& host) const {
    char uri[320];
    snprintf(uri, sizeof uri, "rtsp://%s:%d%s", host.c_str(), rtsp_port_, p.rtsp_path.c_str());
    return "<trt:GetStreamUriResponse><trt:MediaUri>"
           "<tt:Uri>" + xml_escape(uri) + "</tt:Uri>"
           "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
           "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
           "<tt:Timeout>PT60S</tt:Timeout>"
           "</trt:MediaUri></trt:GetStreamUriResponse>";
}

std::string OnvifService::video_sources() const {
    int w = 0, h = 0, fps = 0;
    if (!profiles_.empty()) { w = profiles_[0].width; h = profiles_[0].height; fps = profiles_[0].fps; }
    char b[512];
    snprintf(b, sizeof b,
             "<trt:GetVideoSourcesResponse><trt:VideoSources token=\"vs0\">"
             "<tt:Framerate>%d</tt:Framerate>"
             "<tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>"
             "</trt:VideoSources></trt:GetVideoSourcesResponse>", fps, w, h);
    return b;
}

std::string OnvifService::encoder_configurations() const {
    std::string out = "<trt:GetVideoEncoderConfigurationsResponse>";
    for (const MediaProfile& p : profiles_) {
        char geom[640];
        snprintf(geom, sizeof geom,
                 "<tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>"
                 "<tt:Quality>4</tt:Quality>"
                 "<tt:RateControl><tt:FrameRateLimit>%d</tt:FrameRateLimit>"
                 "<tt:EncodingInterval>1</tt:EncodingInterval>"
                 "<tt:BitrateLimit>%d</tt:BitrateLimit></tt:RateControl>",
                 p.width, p.height, p.fps, p.bitrate_kbps);
        out += "<trt:Configurations token=\"vec-" + xml_escape(p.token) + "\">"
               "<tt:Name>" + xml_escape(p.name) + "</tt:Name><tt:UseCount>1</tt:UseCount>"
               "<tt:Encoding>" + xml_escape(p.encoding) + "</tt:Encoding>" + geom +
               "<tt:SessionTimeout>PT60S</tt:SessionTimeout></trt:Configurations>";
    }
    out += "</trt:GetVideoEncoderConfigurationsResponse>";
    return out;
}

OnvifService::Response OnvifService::handle(const Request& req, int64_t now_unix) {
    Response r;

    if (!cfg_.enabled) {
        r.status = 404;
        r.content_type = "text/plain";
        r.body = "ONVIF is disabled\n";
        return r;
    }
    if (!soap_acceptable(req.body)) {
        r.status = 400;
        r.body = fault("s:Sender", "ter:WellFormed", "the request is not an acceptable SOAP document");
        return r;
    }
    std::string action;
    if (!soap_action(req.body, action)) {
        r.status = 400;
        r.body = fault("s:Sender", "ter:WellFormed", "no SOAP operation in the body");
        return r;
    }

    // GetSystemDateAndTime is unauthenticated BY SPEC, and it has to be: a
    // client needs the camera's clock before it can build a PasswordDigest
    // whose Created will pass the freshness check.
    if (action != "GetSystemDateAndTime") {
        const AuthResult a = authenticate(req.body, req.authorization, now_unix, req.method, req.path);
        if (a != AuthResult::Ok) {
            r.status = 401;
            const char* why =
                a == AuthResult::Unclaimed    ? "this camera has not been set up yet"
              : a == AuthResult::Unverifiable ? "PasswordDigest needs onvif.password to be set on this camera"
              : a == AuthResult::Stale        ? "the credential is stale or has been replayed"
              : a == AuthResult::Missing      ? "no credential was offered"
                                              : "the credential was not accepted";
            r.body = fault("s:Sender", "ter:NotAuthorized", why);
            // Offer the challenge so a digest-only client can retry; without
            // it, such a client never sends a credential at all.
            r.extra_headers = challenge(now_unix);
            return r;
        }
    }

    const std::string host = req.host.empty() ? std::string("0.0.0.0") : req.host;

    if      (action == "GetSystemDateAndTime")  r.body = envelope(system_date_and_time(now_unix));
    else if (action == "GetDeviceInformation")  r.body = envelope(device_information());
    else if (action == "GetCapabilities")       r.body = envelope(capabilities(host));
    else if (action == "GetServices")           r.body = envelope(services(host));
    else if (action == "GetScopes")             r.body = envelope(scopes());
    else if (action == "GetProfiles")           r.body = envelope(profiles_xml());
    else if (action == "GetVideoSources")       r.body = envelope(video_sources());
    else if (action == "GetVideoEncoderConfigurations") r.body = envelope(encoder_configurations());
    else if (action == "GetStreamUri") {
        // The token the client sends back is the one GetProfiles handed it.
        std::string token;
        element_text(req.body, "ProfileToken", token);
        const MediaProfile* p = nullptr;
        for (const MediaProfile& c : profiles_) if (c.token == token) { p = &c; break; }
        // An absent token is answered with the first profile, which is what
        // clients that skip it expect; a token we do not have is an error,
        // because guessing would hand back the wrong stream.
        if (!p && token.empty() && !profiles_.empty()) p = &profiles_[0];
        if (!p) {
            r.status = 400;
            r.body = fault("s:Sender", "ter:NoProfile", "no such media profile");
            return r;
        }
        r.body = envelope(stream_uri(*p, host));
    } else {
        // Everything not implemented says so properly rather than answering
        // something a client would act on.
        r.status = 400;
        r.body = fault("s:Sender", "ter:ActionNotSupported",
                       "this camera does not implement " + action);
    }
    return r;
}

}} // namespace machino::onvif
