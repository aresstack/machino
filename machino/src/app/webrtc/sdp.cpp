#include "app/webrtc/sdp.hpp"
#include <cstdio>
#include <cstring>
#include <map>

namespace machino { namespace webrtc {

namespace {

// SDP lines end \r\n (tolerate bare \n); iterate without copying the buffer.
struct Lines {
    const std::string& s; size_t pos = 0;
    explicit Lines(const std::string& v) : s(v) {}
    bool next(std::string& out) {
        if (pos >= s.size()) return false;
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        size_t end = e;
        if (end > pos && s[end - 1] == '\r') --end;
        out.assign(s, pos, end - pos);
        pos = e + 1;
        return true;
    }
};

bool starts(const std::string& l, const char* p) { return l.rfind(p, 0) == 0; }

// "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f"
// -> the value of one key, or "".
std::string fmtp_value(const std::string& params, const char* key) {
    size_t at = 0;
    const std::string k = std::string(key) + "=";
    while (at < params.size()) {
        size_t semi = params.find(';', at);
        if (semi == std::string::npos) semi = params.size();
        std::string part = params.substr(at, semi - at);
        while (!part.empty() && part.front() == ' ') part.erase(0, 1);
        if (part.rfind(k, 0) == 0) return part.substr(k.size());
        at = semi + 1;
    }
    return "";
}

} // namespace

Offer parse_offer(const std::string& sdp) {
    Offer o;
    Lines ls(sdp);
    std::string l;
    int cur = -1;                                     // current m-line index, -1 = session level
    // per-media: payload types seen as H264 in rtpmap, and their fmtp params
    std::map<int, bool> h264_pt;                      // pt -> is H264 (current media only)
    std::map<int, std::string> fmtp;                  // pt -> fmtp params (current media only)
    std::vector<std::string> offered_video;           // every video codec name seen, for the refusal message
    bool saw_h264_any_mode = false;                   // H264 offered, but not in mode 1
    auto settle_video = [&](int idx) {
        // choose the first offered H264 payload with packetization-mode=1
        if (idx < 0 || (size_t)idx >= o.media.size() || o.media[idx].kind != "video") return;
        for (const auto& kv : h264_pt) {
            if (!kv.second) continue;
            saw_h264_any_mode = true;
            auto f = fmtp.find(kv.first);
            const std::string params = f == fmtp.end() ? "" : f->second;
            // RFC 6184: an absent packetization-mode means 0, so it is not
            // enough that H264 is offered - only mode 1 carries the FU-A
            // fragmentation this sender emits.
            if (fmtp_value(params, "packetization-mode") != "1") continue;
            o.media[idx].h264_pt = kv.first;
            o.media[idx].h264_profile = fmtp_value(params, "profile-level-id");
            if (o.video_index < 0) o.video_index = idx;
            return;
        }
    };
    while (ls.next(l)) {
        if (starts(l, "m=")) {
            settle_video(cur);
            h264_pt.clear(); fmtp.clear();
            OfferMedia m;
            size_t sp = l.find(' ');
            m.kind = l.substr(2, sp == std::string::npos ? std::string::npos : sp - 2);
            o.media.push_back(m);
            cur = (int)o.media.size() - 1;
        } else if (starts(l, "a=mid:")) {
            if (cur >= 0) o.media[cur].mid = l.substr(6);
        } else if (starts(l, "a=ice-ufrag:")) {
            if (o.ice_ufrag.empty()) o.ice_ufrag = l.substr(12);
        } else if (starts(l, "a=ice-pwd:")) {
            if (o.ice_pwd.empty()) o.ice_pwd = l.substr(10);
        } else if (starts(l, "a=fingerprint:")) {
            if (o.fingerprint.empty()) {
                std::string v = l.substr(14);
                size_t sp = v.find(' ');
                if (sp != std::string::npos) {
                    o.fingerprint_alg = v.substr(0, sp);
                    o.fingerprint = v.substr(sp + 1);
                }
            }
        } else if (starts(l, "a=rtpmap:")) {
            int pt = -1; char codec[64] = {0};
            if (sscanf(l.c_str() + 9, "%d %63[^/]", &pt, codec) == 2 && pt >= 0) {
                if (strcmp(codec, "H264") == 0) h264_pt[pt] = true;
                // Remember every video codec the browser offered, so a refusal
                // can say what it DID offer. "no H264 packetization-mode=1" on
                // its own does not distinguish a browser with no H264 at all
                // from one that offers H264 only in mode 0 - and without the
                // offer in hand those need opposite answers.
                if (cur >= 0 && o.media[cur].kind == "video" && offered_video.size() < 12) {
                    const std::string name(codec);
                    // Skip the RTP housekeeping payloads: they are in every
                    // offer and say nothing about what can be decoded.
                    if (name != "rtx" && name != "red" && name != "ulpfec" && name != "flexfec-03") {
                        bool seen = false;
                        for (const auto& s : offered_video) if (s == name) { seen = true; break; }
                        if (!seen) offered_video.push_back(name);
                    }
                }
            }
        } else if (starts(l, "a=fmtp:")) {
            int pt = -1; int off = 0;
            if (sscanf(l.c_str() + 7, "%d %n", &pt, &off) >= 1 && pt >= 0 && off > 0)
                fmtp[pt] = l.substr(7 + (size_t)off);
        }
    }
    settle_video(cur);
    if (o.video_index < 0) {
        // Name what the browser DID offer. Without this the log line is the
        // same whether the browser has no H264 at all (a build or platform
        // limitation the user can act on) or offers it only in mode 0 (a
        // negotiation detail) - and those need opposite answers.
        std::string what;
        for (const auto& c : offered_video) { if (!what.empty()) what += ","; what += c; }
        o.error = "no H264 packetization-mode=1 video (offered: " +
                  (what.empty() ? std::string("nothing") : what) + ")";
        if (saw_h264_any_mode) o.error += " - H264 was offered, but not in mode 1";
        return o;
    }
    if (o.ice_ufrag.empty() || o.ice_pwd.empty()) { o.error = "missing ice credentials"; return o; }
    if (o.fingerprint.empty())        { o.error = "missing DTLS fingerprint"; return o; }
    o.ok = true;
    return o;
}

std::string build_answer(const Offer& offer, const AnswerParams& p) {
    if (!offer.ok || offer.video_index < 0) return "";
    const OfferMedia& v = offer.media[(size_t)offer.video_index];
    char buf[256];
    std::string s;
    s += "v=0\r\n";
    s += "o=- 1 2 IN IP4 127.0.0.1\r\n";
    s += "s=-\r\n";
    s += "t=0 0\r\n";
    s += "a=ice-lite\r\n";
    // BUNDLE names only the m-line we accept; rejected m-lines stay out of the group.
    if (!v.mid.empty()) s += "a=group:BUNDLE " + v.mid + "\r\n";
    s += "a=msid-semantic: WMS *\r\n";
    for (size_t i = 0; i < offer.media.size(); ++i) {
        const OfferMedia& m = offer.media[i];
        const bool take = (int)i == offer.video_index;
        const int pt = take ? m.h264_pt : 0;
        snprintf(buf, sizeof buf, "m=%s %u UDP/TLS/RTP/SAVPF %d\r\n",
                 m.kind.c_str(), take ? (unsigned)p.port : 0u, take ? pt : 0);
        s += buf;
        s += "c=IN IP4 " + std::string(take ? p.host_ip : "0.0.0.0") + "\r\n";
        if (!m.mid.empty()) s += "a=mid:" + m.mid + "\r\n";
        if (!take) { s += "a=inactive\r\n"; continue; }
        s += "a=ice-ufrag:" + p.ice_ufrag + "\r\n";
        s += "a=ice-pwd:" + p.ice_pwd + "\r\n";
        s += "a=fingerprint:sha-256 " + p.fingerprint + "\r\n";
        s += "a=setup:passive\r\n";
        s += "a=sendonly\r\n";
        s += "a=rtcp-mux\r\n";
        snprintf(buf, sizeof buf, "a=rtpmap:%d H264/90000\r\n", pt); s += buf;
        snprintf(buf, sizeof buf, "a=fmtp:%d level-asymmetry-allowed=1;packetization-mode=1", pt); s += buf;
        if (!m.h264_profile.empty()) s += ";profile-level-id=" + m.h264_profile;
        s += "\r\n";
        snprintf(buf, sizeof buf, "a=rtcp-fb:%d nack\r\n", pt); s += buf;
        snprintf(buf, sizeof buf, "a=rtcp-fb:%d nack pli\r\n", pt); s += buf;
        snprintf(buf, sizeof buf, "a=ssrc:%u cname:%s\r\n", p.ssrc, p.cname.c_str()); s += buf;
        snprintf(buf, sizeof buf, "a=candidate:1 1 udp 2130706431 %s %u typ host\r\n",
                 p.host_ip.c_str(), (unsigned)p.port); s += buf;
        s += "a=end-of-candidates\r\n";
    }
    return s;
}

}} // namespace machino::webrtc
