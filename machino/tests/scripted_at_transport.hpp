// Ein AT-Transport aus aufgezeichneten Antworten.
//
// Damit ist die Modemlogik ohne Modem pruefbar, und zwar in Faellen, die an
// echter Hardware gar nicht herzustellen sind: eine SIM laesst sich nicht auf
// Zuruf in eine mit aufgebrauchten PIN-Versuchen verwandeln, und ein Netz
// weigert sich nicht auf Kommando.
//
// Zeichnet ausserdem auf, WAS gesendet wurde. Das ist der eigentliche
// Pruefpunkt beim PIN-Schutz: nicht was zurueckkam, sondern dass ein zweites
// AT+CPIN= gar nicht erst hinausging.
#pragma once
#include "ports/iat_transport.hpp"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace machino { namespace test {

class ScriptedAtTransport : public IAtTransport {
public:
    // Antwort auf ein Kommando. Mehrfach gesetzt = nacheinander verbraucht,
    // die letzte bleibt stehen. So laesst sich "erst PIN noetig, dann READY"
    // ohne Zustandsmaschine im Test ausdruecken.
    void reply(const std::string& cmd, const std::string& raw)
    {
        script_[cmd].push_back(raw);
    }

    // Ab jetzt antwortet das Modem anders -- alles Aufgestaute verfaellt.
    //
    // Der Unterschied zu reply() ist im Test leicht zu uebersehen und war
    // einmal die Ursache eines Fehlschlags: nach einem Modem-Neustart soll
    // AT+QCFG="usbnet" 1 liefern statt 3, und mit reply() stand die alte
    // Antwort noch in der Warteschlange. Die Maschine sah nach dem Neustart
    // erneut RNDIS und gab auf -- korrekt, aber die Situation gab es nur im
    // Test.
    void set_reply(const std::string& cmd, const std::string& raw)
    {
        script_[cmd].assign(1, raw);
    }

    void set_available(bool a) { available_ = a; }
    void set_gone(bool g)      { gone_ = g; }
    void set_timeout(const std::string& cmd) { timeout_.insert(cmd); }

    const std::vector<std::string>& sent() const { return sent_; }

    int count_sent(const std::string& prefix) const
    {
        int n = 0;
        for (const std::string& s : sent_)
            if (s.compare(0, prefix.size(), prefix) == 0) ++n;
        return n;
    }

    AtExchange command(const std::string& cmd, int timeout_ms = 3000) override
    {
        (void)timeout_ms;
        sent_.push_back(cmd);

        AtExchange x;
        if (gone_) { x.device_gone = true; return x; }
        if (timeout_.count(cmd)) { x.timed_out = true; return x; }

        auto it = script_.find(cmd);
        if (it == script_.end() || it->second.empty()) {
            // Ein Kommando, fuer das der Test nichts hinterlegt hat, ist ein
            // ERROR und keine stille Leere -- sonst sieht eine vergessene
            // Zeile im Test aus wie ein Modem, das nichts zu sagen hat.
            x.raw = "ERROR\r\n";
            x.result = cellular::at_scan(x.raw);
            return x;
        }
        const std::string raw = it->second.front();
        if (it->second.size() > 1) it->second.erase(it->second.begin());

        x.raw = raw;
        x.result = cellular::at_scan(raw);
        x.payload = cellular::at_payload(raw, cmd);
        return x;
    }

    bool available() const override { return available_ && !gone_; }

private:
    std::map<std::string, std::vector<std::string>> script_;
    std::vector<std::string> sent_;
    std::set<std::string>    timeout_;
    bool available_ = true;
    bool gone_ = false;
};

}} // namespace machino::test
