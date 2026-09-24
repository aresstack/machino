#include "core/cellular/at_framing.hpp"

#include <vector>

namespace machino { namespace cellular {

namespace {

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> lines_of(const std::string& buf)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= buf.size(); ++i) {
        if (i == buf.size() || buf[i] == '\n') {
            out.push_back(trim(buf.substr(start, i - start)));
            start = i + 1;
        }
    }
    return out;
}

bool starts_with(const std::string& s, const char* p)
{
    const size_t n = std::string(p).size();
    return s.size() >= n && s.compare(0, n, p) == 0;
}

} // namespace

AtResult at_scan(const std::string& buf)
{
    for (const std::string& l : lines_of(buf)) {
        if (l.empty()) continue;
        if (l == "OK") return AtResult::Ok;
        // "ERROR" als ganze Zeile, nicht als Teilwort: eine Firmware-Version
        // oder ein Netzname darf die Antwort nicht beenden.
        if (l == "ERROR" || l == "NO CARRIER" || l == "NO ANSWER" ||
            l == "NO DIALTONE" || l == "BUSY") return AtResult::Error;
        if (starts_with(l, "+CME ERROR:") || starts_with(l, "+CMS ERROR:")) return AtResult::Error;
    }
    return AtResult::Pending;
}

std::string at_payload(const std::string& buf, const std::string& command)
{
    const std::string cmd = trim(command);
    std::string out;
    for (const std::string& l : lines_of(buf)) {
        if (l.empty()) continue;
        // Echo. Manche Modems echoen das Kommando, manche nicht (ATE0), also
        // wird es weggelassen, wenn es auftaucht, und sonst nichts vermisst.
        if (!cmd.empty() && l == cmd) continue;
        if (l == "OK" || l == "ERROR") continue;
        if (starts_with(l, "+CME ERROR:") || starts_with(l, "+CMS ERROR:")) continue;
        if (!out.empty()) out += "\n";
        out += l;
    }
    return out;
}

}} // namespace machino::cellular
