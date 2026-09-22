#include "app/onvif/soap.hpp"

#include <cctype>
#include <cstring>

namespace machino { namespace onvif {

namespace {

// Case-insensitive find, because element and attribute names arrive with
// whatever prefix the client chose and some clients vary the case of
// attribute names even though XML says they must not.
size_t ifind(const std::string& h, const std::string& n, size_t from = 0) {
    if (n.empty() || n.size() > h.size()) return std::string::npos;
    for (size_t i = from; i + n.size() <= h.size(); ++i) {
        size_t k = 0;
        while (k < n.size() && (char)tolower((unsigned char)h[i + k]) == (char)tolower((unsigned char)n[k])) ++k;
        if (k == n.size()) return i;
    }
    return std::string::npos;
}

bool name_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.';
}

// Strip an XML namespace prefix: "tds:GetX" -> "GetX".
std::string local_of(const std::string& qname) {
    const size_t c = qname.rfind(':');
    return c == std::string::npos ? qname : qname.substr(c + 1);
}

std::string decode_entities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        const size_t sc = s.find(';', i + 1);
        // Only the five predefined entities and nothing else: no numeric
        // references, no DTD-defined ones. An unknown entity is left as
        // written rather than expanded.
        if (sc == std::string::npos || sc - i > 6) { out += s[i]; continue; }
        const std::string e = s.substr(i + 1, sc - i - 1);
        if      (e == "amp")  out += '&';
        else if (e == "lt")   out += '<';
        else if (e == "gt")   out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else { out += s[i]; continue; }
        i = sc;
    }
    return out;
}

// Find the opening tag of an element with this local name. Returns the offset
// of '<' and, via tag_end, the offset just past '>'. self_closing reports
// "<x/>".
// Advance past a comment, so nothing inside one is ever mistaken for markup.
// Returns npos when the comment is unterminated, which ends the scan.
size_t skip_comment(const std::string& xml, size_t lt) {
    if (xml.compare(lt, 4, "<!--") != 0) return lt;
    const size_t e = xml.find("-->", lt + 4);
    return e == std::string::npos ? std::string::npos : e + 2;
}

bool find_open_tag(const std::string& xml, const std::string& local, size_t from,
                   size_t& tag_start, size_t& tag_end, bool& self_closing) {
    for (size_t i = from; (i = xml.find('<', i)) != std::string::npos; ++i) {
        if (i + 1 >= xml.size()) return false;
        if (xml.compare(i, 4, "<!--") == 0) {
            const size_t s = skip_comment(xml, i);
            if (s == std::string::npos) return false;
            i = s;
            continue;
        }
        const char c1 = xml[i + 1];
        if (c1 == '/' || c1 == '?' || c1 == '!') continue;
        size_t j = i + 1;
        while (j < xml.size() && (name_char(xml[j]) || xml[j] == ':')) ++j;
        const std::string qname = xml.substr(i + 1, j - i - 1);
        const size_t gt = xml.find('>', j);
        if (gt == std::string::npos) return false;
        if (local_of(qname) == local) {
            tag_start = i;
            tag_end = gt + 1;
            self_closing = gt > 0 && xml[gt - 1] == '/';
            return true;
        }
        i = gt;
    }
    return false;
}

} // namespace

bool soap_acceptable(const std::string& xml) {
    if (xml.empty() || xml.size() > MAX_REQUEST) return false;
    // No DTD, no entity declarations. Nothing in ONVIF needs them, and they
    // are the whole of the expansion attack.
    if (ifind(xml, "<!DOCTYPE") != std::string::npos) return false;
    if (ifind(xml, "<!ENTITY") != std::string::npos) return false;
    return true;
}

bool soap_action(const std::string& xml, std::string& action) {
    if (!soap_acceptable(xml)) return false;
    size_t s = 0, e = 0;
    bool selfc = false;
    if (!find_open_tag(xml, "Body", 0, s, e, selfc) || selfc) return false;
    // The first element start inside the Body.
    for (size_t i = e; (i = xml.find('<', i)) != std::string::npos; ++i) {
        if (i + 1 >= xml.size()) return false;
        // A comment is skipped WHOLE: without this, markup written inside one
        // becomes the operation, which is a disagreement between this scanner
        // and any real parser - exactly the class of confusion to avoid in
        // code that runs before authentication.
        if (xml.compare(i, 4, "<!--") == 0) {
            const size_t s = skip_comment(xml, i);
            if (s == std::string::npos) return false;
            i = s;
            continue;
        }
        const char c1 = xml[i + 1];
        if (c1 == '?' || c1 == '!') continue;
        if (c1 == '/') return false;               // </Body>: an empty Body
        size_t j = i + 1;
        while (j < xml.size() && (name_char(xml[j]) || xml[j] == ':')) ++j;
        action = local_of(xml.substr(i + 1, j - i - 1));
        return !action.empty();
    }
    return false;
}

bool element_text(const std::string& xml, const std::string& local_name, std::string& out) {
    size_t s = 0, e = 0;
    bool selfc = false;
    if (!find_open_tag(xml, local_name, 0, s, e, selfc)) return false;
    if (selfc) { out.clear(); return true; }
    // The matching close tag, found by local name rather than by nesting: the
    // elements this is used for never contain a child of the same name.
    for (size_t i = e; (i = xml.find("</", i)) != std::string::npos; ++i) {
        size_t j = i + 2;
        while (j < xml.size() && (name_char(xml[j]) || xml[j] == ':')) ++j;
        if (local_of(xml.substr(i + 2, j - i - 2)) == local_name) {
            out = decode_entities(xml.substr(e, i - e));
            return true;
        }
    }
    return false;
}

bool element_attr(const std::string& xml, const std::string& local_name,
                  const std::string& attr, std::string& out) {
    size_t s = 0, e = 0;
    bool selfc = false;
    if (!find_open_tag(xml, local_name, 0, s, e, selfc)) return false;
    const std::string tag = xml.substr(s, e - s);
    const size_t a = ifind(tag, attr + "=");
    if (a == std::string::npos) return false;
    size_t q = a + attr.size() + 1;
    if (q >= tag.size()) return false;
    const char quote = tag[q];
    if (quote != '"' && quote != '\'') return false;
    const size_t end = tag.find(quote, q + 1);
    if (end == std::string::npos) return false;
    out = decode_entities(tag.substr(q + 1, end - q - 1));
    return true;
}

bool wsse_token(const std::string& xml, WsseToken& out) {
    out = WsseToken();
    if (!soap_acceptable(xml)) return false;
    size_t s = 0, e = 0;
    bool selfc = false;
    if (!find_open_tag(xml, "UsernameToken", 0, s, e, selfc) || selfc) return false;

    const std::string rest = xml.substr(s);
    if (!element_text(rest, "Username", out.username)) return false;
    if (!element_text(rest, "Password", out.password)) return false;
    element_text(rest, "Nonce", out.nonce_b64);
    element_text(rest, "Created", out.created);

    // Absent Type means PasswordText (WSS UsernameToken Profile 1.1 §3.1).
    std::string type;
    if (element_attr(rest, "Password", "Type", type))
        out.digest = type.find("PasswordDigest") != std::string::npos;

    out.present = true;
    return true;
}

bool b64_decode(const std::string& in, std::string& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    int acc = 0, bits = 0;
    size_t pad = 0;
    for (char c : in) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (c == '=') { ++pad; continue; }
        if (pad) return false;                     // data after padding
        const int v = val(c);
        if (v < 0) return false;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((acc >> bits) & 0xff); }
    }
    return pad <= 2;
}

std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // Control characters are not legal in XML 1.0 content and a
                // client that receives one may reject the whole document.
                if ((unsigned char)c < 0x20 && c != '\t' && c != '\n' && c != '\r') break;
                out += c;
        }
    }
    return out;
}

std::string envelope(const std::string& body_xml) {
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
        " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\""
        " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\""
        " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\">"
        "<s:Body>" + body_xml + "</s:Body></s:Envelope>";
}

std::string fault(const std::string& code, const std::string& subcode,
                  const std::string& reason) {
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:ter=\"http://www.onvif.org/ver10/error\">"
        "<s:Body><s:Fault>"
        "<s:Code><s:Value>" + xml_escape(code) + "</s:Value>"
        "<s:Subcode><s:Value>" + xml_escape(subcode) + "</s:Value></s:Subcode></s:Code>"
        "<s:Reason><s:Text xml:lang=\"en\">" + xml_escape(reason) + "</s:Text></s:Reason>"
        "</s:Fault></s:Body></s:Envelope>";
}

}} // namespace machino::onvif
