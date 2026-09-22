// WS-Discovery message layer. A Probe arrives over UDP from anyone on the
// segment, unauthenticated and unsolicited, so the parsing side gets the same
// scrutiny the SOAP endpoint got.
#include "app/onvif/discovery.hpp"
#include "app/onvif/soap.hpp"
#include <cstdio>
#include <string>

using namespace machino;
using namespace machino::onvif;

extern int g_fail_ext, g_pass_ext;
#define DCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

std::string probe_msg(const std::string& msg_id, const std::string& types,
                      const char* body_name = "Probe") {
    std::string t = types.empty() ? "" : "<d:Types>" + types + "</d:Types>";
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
           " xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
           " xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
           " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
           "<s:Header>"
           "<a:MessageID>" + msg_id + "</a:MessageID>"
           "<a:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>"
           "<a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action>"
           "</s:Header>"
           "<s:Body><d:" + body_name + ">" + t + "</d:" + body_name + "></s:Body></s:Envelope>";
}

bool has(const std::string& h, const std::string& n) { return h.find(n) != std::string::npos; }

const char* MID = "urn:uuid:11111111-2222-3333-4444-555555555555";

Announcement ann() {
    Announcement a;
    a.uuid = device_uuid("SN1");
    a.scopes = "onvif://www.onvif.org/Profile/Streaming onvif://www.onvif.org/name/T40NN";
    a.xaddr = "http://192.168.1.10:80/onvif/device_service";
    return a;
}

} // namespace

void run_discovery_tests() {
    // ---- parsing a Probe ----------------------------------------------------
    {
        Probe p;
        DCHECK(parse_probe(probe_msg(MID, "dn:NetworkVideoTransmitter"), p));
        DCHECK(p.valid && p.message_id == MID);
        DCHECK(has(p.types, "NetworkVideoTransmitter"));

        // a Probe with no Types at all is legal and is the first thing most
        // discovery tools send
        Probe q;
        DCHECK(parse_probe(probe_msg(MID, ""), q) && q.valid && q.types.empty());
    }

    // ---- what is NOT a Probe -------------------------------------------------
    {
        Probe p;
        // a different discovery operation must not be answered as a Probe
        DCHECK(!parse_probe(probe_msg(MID, "", "Resolve"), p));
        DCHECK(!parse_probe(probe_msg(MID, "", "Hello"), p));
        // the SOAP endpoint's own refusals apply here too - this arrives from
        // anyone on the segment
        DCHECK(!parse_probe("<!DOCTYPE x []>" + probe_msg(MID, ""), p));
        DCHECK(!parse_probe(std::string(MAX_REQUEST + 1, 'x'), p));
        DCHECK(!parse_probe("", p));
        DCHECK(!parse_probe("garbage", p));
        // no MessageID: the reply has to RelatesTo something, and a client
        // discards a ProbeMatches that relates to nothing
        const std::string no_id =
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
            "<s:Body><d:Probe/></s:Body></s:Envelope>";
        DCHECK(!parse_probe(no_id, p));
    }

    // ---- whom we answer -------------------------------------------------------
    {
        Probe p;
        parse_probe(probe_msg(MID, "dn:NetworkVideoTransmitter"), p);
        DCHECK(probe_matches_us(p));

        // prefix-insensitive: the client chooses its own prefix
        Probe q;
        parse_probe(probe_msg(MID, "tds:NetworkVideoTransmitter"), q);
        DCHECK(probe_matches_us(q));
        Probe r;
        parse_probe(probe_msg(MID, "NetworkVideoTransmitter"), r);
        DCHECK(probe_matches_us(r));

        // absent or whitespace-only Types matches everything
        Probe s;
        parse_probe(probe_msg(MID, ""), s);
        DCHECK(probe_matches_us(s));
        Probe w;
        parse_probe(probe_msg(MID, "   "), w);
        DCHECK(probe_matches_us(w));

        // a Probe for something this camera is NOT is ignored: answering would
        // put it in a list it does not belong in
        Probe d;
        parse_probe(probe_msg(MID, "dn:NetworkVideoDisplay"), d);
        DCHECK(!probe_matches_us(d));
        Probe pr;
        parse_probe(probe_msg(MID, "tds:Device"), pr);
        DCHECK(!probe_matches_us(pr));

        // an unparsed Probe is never a match
        Probe none;
        DCHECK(!probe_matches_us(none));
    }

    // ---- the endpoint identity is stable, and distinct per camera ------------
    {
        const std::string a = device_uuid("SN1");
        DCHECK(a.compare(0, 9, "urn:uuid:") == 0);
        DCHECK(a.size() == 9 + 36);
        // stable across calls: the same camera must present the same endpoint
        // reference after a restart, which is why there is no state file
        DCHECK(device_uuid("SN1") == a);
        // distinct per camera
        DCHECK(device_uuid("SN2") != a);
        DCHECK(device_uuid("") != a);
        // RFC 4122 version and variant nibbles are set
        DCHECK(a[9 + 14] == '5');
        const char v = a[9 + 19];
        DCHECK(v == '8' || v == '9' || v == 'a' || v == 'b');
    }

    // ---- message ids are distinct --------------------------------------------
    {
        const std::string m1 = message_id(1, 1000);
        const std::string m2 = message_id(2, 1000);
        const std::string m3 = message_id(1, 1001);
        DCHECK(m1 != m2 && m1 != m3 && m2 != m3);
        DCHECK(m1.compare(0, 9, "urn:uuid:") == 0);
    }

    // ---- the ProbeMatches reply ----------------------------------------------
    {
        const Announcement a = ann();
        const std::string r = probe_matches(a, MID, message_id(1, 1000));
        // it must relate to the probe, or the client drops it
        DCHECK(has(r, "<a:RelatesTo>" + std::string(MID) + "</a:RelatesTo>"));
        DCHECK(has(r, "ProbeMatches"));
        DCHECK(has(r, "<a:Address>" + a.uuid + "</a:Address>"));
        DCHECK(has(r, "NetworkVideoTransmitter"));
        DCHECK(has(r, "http://192.168.1.10:80/onvif/device_service"));
        DCHECK(has(r, "Profile/Streaming"));
        DCHECK(has(r, "<d:MetadataVersion>1</d:MetadataVersion>"));
        DCHECK(has(r, "discovery/ProbeMatches"));
        // it is the answer to one question, so it is not addressed to the
        // discovery group
        DCHECK(!has(r, "<a:To>urn:schemas-xmlsoap-org"));
        // and it is a well-formed envelope by our own scanner's standards
        DCHECK(soap_acceptable(r));
        std::string act;
        DCHECK(soap_action(r, act) && act == "ProbeMatches");
    }

    // ---- Hello and Bye --------------------------------------------------------
    {
        const Announcement a = ann();
        const std::string h = hello(a, message_id(2, 1000));
        DCHECK(has(h, "<d:Hello>") && has(h, "discovery/Hello"));
        DCHECK(has(h, "<a:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>"));
        DCHECK(!has(h, "RelatesTo"));          // unsolicited
        DCHECK(has(h, a.xaddr));

        const std::string b = bye(a, message_id(3, 1000));
        DCHECK(has(b, "<d:Bye>") && has(b, "discovery/Bye"));
        DCHECK(has(b, "<a:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>"));

        std::string act;
        DCHECK(soap_action(h, act) && act == "Hello");
        DCHECK(soap_action(b, act) && act == "Bye");
    }

    // ---- anything that goes into a message is escaped --------------------------
    {
        Announcement a = ann();
        a.scopes = "onvif://x/name/Cam & <Co>";
        const std::string r = probe_matches(a, "urn:uuid:a&b", message_id(4, 1000));
        DCHECK(has(r, "Cam &amp; &lt;Co&gt;"));
        DCHECK(has(r, "urn:uuid:a&amp;b"));
        DCHECK(!has(r, "<Co>"));
        DCHECK(soap_acceptable(r));
    }
}
