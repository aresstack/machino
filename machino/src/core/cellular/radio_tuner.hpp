// Bandwahl und Netzmodus am Modem durchsetzen -- und der SINR-Bandscan.
//
// Portiert aus esp32-modem-host/ec200a_modem.cpp (modemApplyBands,
// modemLockBandMask, bandScanTask). Dort ist beides ein Knopf, der sofort AT
// schickt; hier ist es eine Zustandsmaschine, die im Takt des Hauptthreads
// laeuft, weil auf dieser Kamera nur EIN Sprecher auf dem AT-Port sitzen darf
// und der HTTP-Thread es nicht ist.
//
// WANN GESCHRIEBEN WIRD
//
// Die Bandmaske und der Netzmodus sind im Modem PERSISTENT. machino schreibt
// sie deshalb nicht bei jedem Start, sondern gleicht ab:
//
//   Konfiguration geaendert   -> schreiben, genau einmal je Aenderung. Das
//                                ist die Aktion des Benutzers (und die
//                                Rueckabwicklung derselben, wenn das
//                                Bestaetigungsfenster ablaeuft: die alte
//                                Konfiguration kommt zurueck und wird
//                                ebenso geschrieben).
//   Modem frisch da           -> zurueckLESEN. Stimmt es, nichts tun. Weicht
//                                es lesbar ab (ein Modem, das vorher woanders
//                                haengte), einmal schreiben. Ist die
//                                Rueckmeldung nicht lesbar, NICHTS schreiben
//                                und das sagen -- eine unbekannte Antwort ist
//                                kein Grund fuer einen RF-Zyklus bei jedem
//                                Start.
//
// Nach jedem Schreiben wird zurueckgelesen. Meldet das Modem danach etwas
// anderes, ist das ein Fehler mit Begruendung und KEIN zweiter Versuch: ein
// Modem, das die Maske nicht nimmt, nimmt sie beim dritten Mal auch nicht,
// und jeder Versuch kostet die Verbindung fuer Sekunden.
//
// Der RF-Zyklus (AT+CFUN=0, schreiben, AT+CFUN=1) ist auf zwei Takte
// verteilt: RF aus in dem einen, schreiben und RF an im naechsten. Das ersetzt
// die delay()-Aufrufe der Referenz und haelt den Hauptthread frei. Solange
// das laeuft, meldet tick() "beschaeftigt", und der Datenlink wird in diesem
// Takt nicht angetrieben -- er saehe sonst eine Registrierung verschwinden,
// die gleich wiederkommt, und fienge einen Neuaufbau an.
//
// DER SCAN
//
// Sperrt nacheinander jedes Band des aktiven Profils, wartet, bis das Modem
// darauf registriert ist und QENG wirklich dieses Band meldet, nimmt einige
// Messwerte und den Median davon. Am Ende wird die konfigurierte Maske
// WIEDERHERGESTELLT und das beste Band nur GEMELDET. Die Referenz sperrt es
// sofort; hier nicht, und zwar mit Absicht: ein Band-Lock ist eine
// Netzaenderung, die eine Kamera unerreichbar machen kann, und solche
// Aenderungen laufen auf dieser Kamera ueber das Bestaetigungsfenster. Die
// Oberflaeche bietet den Lock als eigenen Schritt an.
//
// Die Messwerte kommen aus dem Status, den CellularService ohnehin jeden Takt
// liest (QENG servingcell). Der Scan schickt selbst nur die Sperrkommandos.
#pragma once
#include "core/cellular/band_plan.hpp"
#include "core/cellular/cellular_config.hpp"
#include "core/cellular/cellular_status.hpp"
#include "ports/iat_transport.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace machino { namespace cellular {

enum class RadioSync {
    Unknown,      // noch nicht nachgesehen
    InSync,       // Modem meldet, was konfiguriert ist
    Pending,      // Schreiben laeuft oder steht an
    Failed,       // abgelehnt oder nach dem Schreiben abweichend -- kein Retry
    Unverified,   // Rueckmeldung nicht lesbar; nichts geschrieben
};
const char* radio_sync_name(RadioSync s);

enum class ScanState { Idle, Running, Done, Error };
const char* scan_state_name(ScanState s);

struct ScanRow {
    int      band = 0;
    int      mhz = 0;
    bool     ok = false;        // genug Messwerte auf genau diesem Band
    int      samples = 0;
    MaybeInt sinr, rsrp, rsrq;  // Mediane
};

struct RadioState {
    RadioSync   sync = RadioSync::Unknown;
    std::string detail;

    // Die Absicht, aus der Konfiguration abgeleitet.
    uint64_t desired_mask = 0;
    NetMode  desired_mode = NetMode::Auto;

    // Was das Modem zuletzt gemeldet hat. Abwesend = nicht lesbar.
    Maybe<uint64_t> modem_mask;
    MaybeInt        modem_nwscanmode;

    int writes = 0;             // Schreibvorgaenge in diesem Modem-Lebenszyklus

    ScanState            scan = ScanState::Idle;
    std::string          scan_detail;
    int                  scan_current = 0;   // Band in Messung, 0 = keines
    int                  scan_best = 0;      // 0 = keines
    std::vector<ScanRow> rows;
};

class RadioTuner {
public:
    using ClockFn = std::function<uint64_t()>;

    void set_clock(ClockFn now) { now_ = std::move(now); }

    // Die Absicht. Eine Aenderung gegenueber der vorherigen Absicht loest
    // genau einen Schreibvorgang aus; die erste Absicht nach dem Start nicht
    // -- die wird nur geprueft.
    void set_config(const CellularConfig& c);

    // Vom Benutzer angestossen. false mit Begruendung, wenn es nicht geht;
    // dann steht der Grund auch im Scan-Zustand.
    bool request_scan(std::string& why_not);

    // Ein Schritt. true, solange das Modem fuer Funkarbeit gebraucht wird
    // (RF-Zyklus halb fertig, Scan laeuft) -- der Datenlink soll dann in
    // diesem Takt nicht angetrieben werden.
    bool tick(IAtTransport& at, const CellularStatus& status);

    bool scanning() const { return st_.scan == ScanState::Running; }
    const RadioState& state() const { return st_; }

    static const int      kSamplesPerBand = 4;
    static const uint32_t kBandTimeoutMs  = 30000;

private:
    enum class Phase { Idle, RfOff };
    enum class ScanPhase { Lock, Measure, Restore };

    uint64_t now() const { return now_ ? now_() : 0; }
    bool mask_in_sync(uint64_t modem_mask) const;
    void write_step(IAtTransport& at);
    bool lock_mask(IAtTransport& at, uint64_t mask);
    void scan_step(IAtTransport& at, const CellularStatus& s);
    void scan_advance();
    void scan_finish();
    void on_modem_gone();

    RadioState st_;
    ClockFn    now_;

    bool have_cfg_ = false;
    bool alive_ = false;
    bool verified_ = false;        // Rueckmeldung fuer diese Absicht gesehen
    bool apply_pending_ = false;   // Schreiben verlangt (Konfigurationsaenderung)
    bool restore_pending_ = false; // Scan abgebrochen: Maske wiederherstellen
    Phase phase_ = Phase::Idle;

    std::vector<int>      scan_bands_;
    size_t                scan_idx_ = 0;
    ScanPhase             scan_phase_ = ScanPhase::Lock;
    uint64_t              scan_deadline_ms_ = 0;
    std::vector<int>      sinr_, rsrp_, rsrq_;
};

}} // namespace machino::cellular
