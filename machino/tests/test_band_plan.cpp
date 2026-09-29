// Bandwahl, Netzmodus, SINR-Scan und die Einmal-Aktionen -- gegen Attrappen.
//
// Was hier vor allem geprueft wird, ist das, was NICHT passieren darf: ein
// RF-Zyklus bei jedem Start, ein zweiter Schreibversuch nach einer Ablehnung,
// ein Scan, der die Maske nicht wiederherstellt, eine PIN, die zweimal
// hinausgeht oder in einer Antwort auftaucht.
#include "core/cellular/band_plan.hpp"
#include "core/cellular/modem_actions.hpp"
#include "core/cellular/radio_tuner.hpp"
#include "core/net/cellular_uplink.hpp"
#include "scripted_at_transport.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace machino;
using namespace machino::cellular;
using machino::test::ScriptedAtTransport;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// ------------------------------------------------------------ Tabelle ----

void test_the_band_table_is_the_eu_set_with_the_reference_bits()
{
    // Zehn Baender, bit = num - 1, die vier TDD-Baender markiert -- LTE_BANDS
    // aus ec200a_modem.cpp, unveraendert.
    TCHECK(lte_bands().size() == 10);
    for (const LteBand& b : lte_bands()) TCHECK(b.bit == b.num - 1);
    TCHECK(lte_band_by_number(20) && lte_band_by_number(20)->mhz == 800);
    TCHECK(lte_band_by_number(38) && lte_band_by_number(38)->tdd);
    TCHECK(!lte_band_by_number(2));                   // US-Band, nicht EU
    // Die Maske "alle" ist genau die aus dem README des Referenzprojekts.
    TCHECK(mask_hex(all_supported_mask()) == "1a0080800d5");
}

void test_profiles_give_the_reference_masks()
{
    TCHECK(profile_mask(BandProfile::Mid, 0) == (1ULL << 2));           // B3
    TCHECK(bands_from_mask(profile_mask(BandProfile::Low, 0)) == std::vector<int>({5, 8, 20, 28}));
    TCHECK(profile_mask(BandProfile::Auto, 0x4) == all_supported_mask()); // custom-Maske egal
    // Custom: nur Bits aus der Tabelle, und leer bleibt leer -- kein stilles "alle".
    TCHECK(profile_mask(BandProfile::Custom, (1ULL << 19) | (1ULL << 1)) == (1ULL << 19));
    TCHECK(profile_mask(BandProfile::Custom, 0) == 0);
}

void test_masks_round_trip_through_band_numbers_and_hex()
{
    uint64_t m = 0; int bad = 0;
    TCHECK(mask_from_bands({3, 20}, m, bad));
    TCHECK(mask_hex(m) == "80004");                   // B3 + B20, wie im Handbuch
    TCHECK(bands_from_mask(m) == std::vector<int>({3, 20}));
    TCHECK(!mask_from_bands({3, 66}, m, bad) && bad == 66);

    uint64_t back = 0;
    TCHECK(parse_mask_hex("80004", back) && back == m);
    TCHECK(parse_mask_hex("0x80004", back) && back == m);
    TCHECK(parse_mask_hex("\"1a0080800d5\"", back) && back == all_supported_mask());
    TCHECK(!parse_mask_hex("", back));
    TCHECK(!parse_mask_hex("zz", back));
    TCHECK(!parse_mask_hex("0x", back));
}

void test_net_mode_maps_to_nwscanmode_and_back()
{
    TCHECK(net_mode_nwscanmode(NetMode::Auto) == 0);
    TCHECK(net_mode_nwscanmode(NetMode::LteOnly) == 3);
    TCHECK(net_mode_nwscanmode(NetMode::GsmOnly) == 1);
    NetMode m;
    TCHECK(net_mode_from_nwscanmode(3, m) && m == NetMode::LteOnly);
    TCHECK(!net_mode_from_nwscanmode(2, m));          // WCDMA: hat das EC200A nicht
    TCHECK(net_mode_parse("lte", m) && m == NetMode::LteOnly);
    TCHECK(!net_mode_parse("4g", m));
    BandProfile p;
    TCHECK(band_profile_parse("low", p) && p == BandProfile::Low);
    TCHECK(!band_profile_parse("high", p));
}

void test_qcfg_band_readback_accepts_both_hex_spellings_and_nothing_else()
{
    QcfgBand q = parse_qcfg_band("+QCFG: \"band\",d3,1a0080800d5\r\nOK\r\n");
    TCHECK(q.lte.has && q.lte.value == all_supported_mask());
    TCHECK(q.gsm.has && q.gsm.value == 0xd3);
    q = parse_qcfg_band("+QCFG: \"band\",0xf,0x80004,0x0\r\nOK\r\n");
    TCHECK(q.lte.has && q.lte.value == 0x80004);
    // Nicht lesbar heisst abwesend -- nicht 0.
    q = parse_qcfg_band("ERROR\r\n");
    TCHECK(!q.lte.has && !q.gsm.has);
    q = parse_qcfg_band("+QCFG: \"nwscanmode\",0\r\nOK\r\n");
    TCHECK(!q.lte.has);
}

// -------------------------------------------------------------- Tuner ----

struct Clock { uint64_t t = 1000; };

// Genau dieses Kommando, nicht ein Praefix davon: "d3,80000" (B20) ist ein
// Praefix von "d3,8000000" (B28), und count_sent zaehlt Praefixe.
int count_exact(const ScriptedAtTransport& at, const std::string& cmd)
{
    int n = 0;
    for (const std::string& s : at.sent()) if (s == cmd) ++n;
    return n;
}

CellularStatus responsive_status()
{
    CellularStatus s;
    s.present = true;
    s.responsive = true;
    s.sim = SimState::Ready;
    s.registration = RegState::RegisteredHome;
    return s;
}

CellularConfig cfg_with(BandProfile p, NetMode m, uint64_t custom = 0)
{
    CellularConfig c;
    c.enabled = true;
    c.band_profile = p;
    c.net_mode = m;
    c.band_mask = custom;
    return c;
}

void arm_modem_reports(ScriptedAtTransport& at, const char* lte_hex, int nwscanmode)
{
    at.set_reply("AT+QCFG=\"band\"", std::string("+QCFG: \"band\",d3,") + lte_hex + "\r\nOK\r\n");
    at.set_reply("AT+QCFG=\"nwscanmode\"", "+QCFG: \"nwscanmode\"," + std::to_string(nwscanmode) + "\r\nOK\r\n");
    at.set_reply("AT+CFUN=0", "OK\r\n");
    at.set_reply("AT+CFUN=1", "OK\r\n");
}

struct TunerRig {
    ScriptedAtTransport at;
    Clock clock;
    RadioTuner tuner;
    CellularStatus st = responsive_status();
    TunerRig() { tuner.set_clock([this] { return clock.t; }); }
    void ticks(int n, uint64_t step = 2000)
    {
        for (int i = 0; i < n; ++i) { tuner.tick(at, st); clock.t += step; }
    }
};

void test_a_modem_that_matches_is_left_alone()
{
    // Der Normalfall bei jedem Start: Konfiguration auto/auto, das Modem auf
    // Werksmaske. Kein CFUN, kein Schreiben.
    TunerRig r;
    arm_modem_reports(r.at, "1a0080800d5", 0);
    r.tuner.set_config(cfg_with(BandProfile::Auto, NetMode::Auto));
    r.ticks(3);
    TCHECK(r.tuner.state().sync == RadioSync::InSync);
    TCHECK(r.at.count_sent("AT+CFUN") == 0);
    TCHECK(r.at.count_sent("AT+QCFG=\"band\",") == 0);
    // Und nur EINMAL gelesen, nicht in jedem Takt.
    TCHECK(r.at.count_sent("AT+QCFG=\"band\"") == 1);
}

void test_auto_accepts_a_factory_mask_that_is_a_superset()
{
    // Eine Werksmaske traegt Bits fuer Baender, die unsere Tabelle nicht
    // kennt. Bei "auto" ist das in Ordnung -- sie zu beschneiden hiesse, bei
    // jedem Start zu schreiben, was niemand wollte.
    TunerRig r;
    arm_modem_reports(r.at, "ffffffffffff", 0);
    r.tuner.set_config(cfg_with(BandProfile::Auto, NetMode::Auto));
    r.ticks(2);
    TCHECK(r.tuner.state().sync == RadioSync::InSync);
    TCHECK(r.at.count_sent("AT+CFUN=0") == 0);
}

void test_a_change_is_written_once_with_the_reference_sequence()
{
    TunerRig r;
    arm_modem_reports(r.at, "1a0080800d5", 0);
    r.tuner.set_config(cfg_with(BandProfile::Auto, NetMode::Auto));
    r.ticks(2);
    TCHECK(r.tuner.state().sync == RadioSync::InSync);

    // Der Benutzer waehlt "unter 1 GHz" und "nur LTE".
    r.tuner.set_config(cfg_with(BandProfile::Low, NetMode::LteOnly));
    TCHECK(r.tuner.state().sync == RadioSync::Pending);
    r.at.set_reply("AT+QCFG=\"band\",d3,8080090", "OK\r\n");
    r.at.set_reply("AT+QCFG=\"nwscanmode\",3,1", "OK\r\n");
    // Takt 1: Rueckmeldung lesen, RF aus. Takt 2: schreiben, RF an. Takt 3:
    // zurueckLESEN und bestaetigen.
    const bool busy1 = r.tuner.tick(r.at, r.st);
    TCHECK(busy1);
    TCHECK(r.at.count_sent("AT+CFUN=0") == 1);
    TCHECK(r.at.count_sent("AT+QCFG=\"band\",d3,") == 0);   // noch nicht
    const bool busy2 = r.tuner.tick(r.at, r.st);
    TCHECK(busy2);
    TCHECK(r.at.count_sent("AT+QCFG=\"band\",d3,8080090") == 1);
    TCHECK(r.at.count_sent("AT+QCFG=\"nwscanmode\",3,1") == 1);
    TCHECK(r.at.count_sent("AT+CFUN=1") == 1);
    // Das Modem meldet jetzt das Neue.
    arm_modem_reports(r.at, "8080090", 3);
    const bool busy3 = r.tuner.tick(r.at, r.st);
    TCHECK(!busy3);
    TCHECK(r.tuner.state().sync == RadioSync::InSync);
    TCHECK(r.tuner.state().writes == 1);
    TCHECK(r.tuner.state().modem_mask.has && r.tuner.state().modem_mask.value == 0x8080090);

    // Danach Ruhe: keine weiteren Kommandos.
    const size_t before = r.at.sent().size();
    r.ticks(5);
    TCHECK(r.at.sent().size() == before);
}

void test_a_rejected_mask_is_reported_and_not_retried_and_rf_comes_back()
{
    TunerRig r;
    arm_modem_reports(r.at, "1a0080800d5", 0);
    r.tuner.set_config(cfg_with(BandProfile::Auto, NetMode::Auto));
    r.ticks(2);
    r.tuner.set_config(cfg_with(BandProfile::Mid, NetMode::Auto));
    r.at.set_reply("AT+QCFG=\"band\",d3,4", "+CME ERROR: 50\r\n");
    r.at.set_reply("AT+QCFG=\"nwscanmode\",0,1", "OK\r\n");
    r.ticks(6);
    TCHECK(r.tuner.state().sync == RadioSync::Failed);
    TCHECK(r.tuner.state().detail.find("rejected") != std::string::npos);
    TCHECK(r.at.count_sent("AT+QCFG=\"band\",d3,4") == 1);      // genau einmal
    TCHECK(r.at.count_sent("AT+CFUN=1") == 1);                   // RF wieder an
    TCHECK(r.at.count_sent("AT+CFUN=0") == 1);
}

void test_an_unreadable_report_writes_nothing_at_boot_but_a_change_still_does()
{
    TunerRig r;
    r.at.set_reply("AT+QCFG=\"band\"", "ERROR\r\n");
    r.at.set_reply("AT+QCFG=\"nwscanmode\"", "ERROR\r\n");
    r.at.set_reply("AT+CFUN=0", "OK\r\n");
    r.at.set_reply("AT+CFUN=1", "OK\r\n");
    r.tuner.set_config(cfg_with(BandProfile::Low, NetMode::Auto));
    r.ticks(3);
    // Nur nachgesehen, nichts geschrieben, und gesagt warum.
    TCHECK(r.tuner.state().sync == RadioSync::Unverified);
    TCHECK(r.at.count_sent("AT+CFUN") == 0);

    // Eine Aenderung ist ein Auftrag -- die geht auch ohne lesbare Rueckmeldung
    // hinaus, wie beim Referenzprojekt, das nie zurueckliest.
    r.at.set_reply("AT+QCFG=\"band\",d3,4", "OK\r\n");
    r.at.set_reply("AT+QCFG=\"nwscanmode\",0,1", "OK\r\n");
    r.tuner.set_config(cfg_with(BandProfile::Mid, NetMode::Auto));
    r.ticks(4);
    TCHECK(r.at.count_sent("AT+QCFG=\"band\",d3,4") == 1);
    TCHECK(r.tuner.state().sync == RadioSync::Unverified);
    TCHECK(r.tuner.state().writes == 1);
}

void test_a_modem_from_elsewhere_is_brought_in_line_once()
{
    // Das Modem hing vorher am ESP32 und hat dort einen B3-Lock bekommen; die
    // Kamera ist auf auto. Lesbar abweichend, noch nie geschrieben: einmal
    // schreiben. Meldet es danach immer noch B3, ist Schluss.
    TunerRig r;
    arm_modem_reports(r.at, "4", 0);
    r.at.set_reply("AT+QCFG=\"band\",d3,1a0080800d5", "OK\r\n");
    r.at.set_reply("AT+QCFG=\"nwscanmode\",0,1", "OK\r\n");
    r.tuner.set_config(cfg_with(BandProfile::Auto, NetMode::Auto));
    r.ticks(6);
    TCHECK(r.at.count_sent("AT+QCFG=\"band\",d3,1a0080800d5") == 1);
    TCHECK(r.tuner.state().sync == RadioSync::Failed);
    TCHECK(r.tuner.state().detail.find("not written again") != std::string::npos);
}

void test_a_rollback_rewrites_the_old_values()
{
    // Das Bestaetigungsfenster laeuft ab: set_config kommt mit dem
    // gespeicherten Stand zurueck, und der geht ebenso ins Modem.
    TunerRig r;
    arm_modem_reports(r.at, "1a0080800d5", 0);
    r.tuner.set_config(cfg_with(BandProfile::Auto, NetMode::Auto));
    r.ticks(2);
    r.at.set_reply("AT+QCFG=\"band\",d3,4", "OK\r\n");
    r.at.set_reply("AT+QCFG=\"nwscanmode\",3,1", "OK\r\n");
    r.tuner.set_config(cfg_with(BandProfile::Mid, NetMode::LteOnly));
    r.ticks(2);
    arm_modem_reports(r.at, "4", 3);
    r.ticks(1);
    TCHECK(r.tuner.state().sync == RadioSync::InSync);

    r.at.set_reply("AT+QCFG=\"band\",d3,1a0080800d5", "OK\r\n");
    r.at.set_reply("AT+QCFG=\"nwscanmode\",0,1", "OK\r\n");
    r.tuner.set_config(cfg_with(BandProfile::Auto, NetMode::Auto));
    r.ticks(2);
    TCHECK(r.at.count_sent("AT+QCFG=\"band\",d3,1a0080800d5") == 1);
    TCHECK(r.at.count_sent("AT+QCFG=\"nwscanmode\",0,1") == 1);
}

void test_a_modem_that_goes_away_is_checked_again_when_it_returns()
{
    TunerRig r;
    arm_modem_reports(r.at, "1a0080800d5", 0);
    r.tuner.set_config(cfg_with(BandProfile::Auto, NetMode::Auto));
    r.ticks(2);
    TCHECK(r.at.count_sent("AT+QCFG=\"band\"") == 1);
    r.st.responsive = false;
    r.ticks(2);
    TCHECK(r.at.count_sent("AT+QCFG=\"band\"") == 1);      // nichts an ein stummes Modem
    r.st.responsive = true;
    r.ticks(2);
    TCHECK(r.at.count_sent("AT+QCFG=\"band\"") == 2);      // einmal nachgesehen
    TCHECK(r.tuner.state().sync == RadioSync::InSync);
}

void test_an_empty_selection_is_never_written()
{
    TunerRig r;
    arm_modem_reports(r.at, "1a0080800d5", 0);
    r.tuner.set_config(cfg_with(BandProfile::Custom, NetMode::Auto, 0));
    r.ticks(3);
    TCHECK(r.tuner.state().sync == RadioSync::Failed);
    TCHECK(r.at.count_sent("AT+CFUN") == 0);
    TCHECK(r.at.count_sent("AT+QCFG=\"band\",") == 0);
}

// ---------------------------------------------------------------- Scan ----

// Eine QENG-Zeile, die dieses Band mit diesen Werten meldet.
void report_cell(CellularStatus& st, int band, int sinr, int rsrp, int rsrq)
{
    st.registration = RegState::RegisteredHome;
    st.cell.band = MaybeInt(band);
    st.cell.sinr = MaybeInt(sinr);
    st.cell.rsrp = MaybeInt(rsrp);
    st.cell.rsrq = MaybeInt(rsrq);
}

void test_the_scan_locks_each_band_measures_medians_and_restores()
{
    TunerRig r;
    arm_modem_reports(r.at, "8080090", 0);            // Profil "low" schon im Modem
    r.tuner.set_config(cfg_with(BandProfile::Low, NetMode::Auto));
    r.ticks(2);
    TCHECK(r.tuner.state().sync == RadioSync::InSync);
    // Jede Einzelmaske wird angenommen.
    for (const char* hex : {"10", "80", "80000", "8000000", "8080090"})
        r.at.set_reply(std::string("AT+QCFG=\"band\",d3,") + hex, "OK\r\n");

    std::string why;
    TCHECK(r.tuner.request_scan(why));
    TCHECK(r.tuner.scanning());
    TCHECK(r.tuner.state().scan == ScanState::Running);

    // Vier Baender (5, 8, 20, 28). Das Modem antwortet auf jedes Band nach
    // einem Takt mit Registrierung; B28 findet kein Netz.
    const int order[] = {5, 8, 20, 28};
    const int sinr[]  = {3, 1, 9, 0};
    for (int i = 0; i < 4; ++i) {
        // Lock-Takt
        TCHECK(r.tuner.tick(r.at, r.st));
        TCHECK(r.tuner.state().scan_current == order[i]);
        r.clock.t += 2000;
        if (order[i] == 28) {
            // kein Netz: Registrierung weg, bis die Frist ablaeuft
            r.st.registration = RegState::Searching;
            r.st.cell = ServingCell{};
            for (int k = 0; k < 20; ++k) { r.tuner.tick(r.at, r.st); r.clock.t += 2000; }
            continue;
        }
        // Erst meldet QENG noch die alte Zelle -- die zaehlt nicht.
        report_cell(r.st, i ? order[i - 1] : 3, 99, -50, -3);
        r.tuner.tick(r.at, r.st); r.clock.t += 2000;
        // Dann vier Messwerte mit einem Ausreisser, den der Median schluckt.
        const int samples[] = {sinr[i], sinr[i] + 20, sinr[i], sinr[i] - 1};
        for (int k = 0; k < RadioTuner::kSamplesPerBand; ++k) {
            report_cell(r.st, order[i], samples[k], -95 - i, -11);
            r.tuner.tick(r.at, r.st); r.clock.t += 2000;
        }
    }
    // Restore-Takt
    r.tuner.tick(r.at, r.st);
    TCHECK(!r.tuner.scanning());
    const RadioState& s = r.tuner.state();
    TCHECK(s.scan == ScanState::Done);
    TCHECK(s.scan_best == 20);
    TCHECK(s.rows.size() == 4);
    TCHECK(s.rows[2].band == 20 && s.rows[2].ok && s.rows[2].sinr.has && s.rows[2].sinr.value == 9);
    TCHECK(s.rows[2].rsrp.has && s.rows[2].rsrp.value == -97);
    TCHECK(!s.rows[3].ok);                               // B28: kein Netz, kein Fehler
    // Jedes Band genau einmal gesperrt, danach die Profilmaske zurueck.
    TCHECK(count_exact(r.at, "AT+QCFG=\"band\",d3,80000") == 1);
    TCHECK(count_exact(r.at, "AT+QCFG=\"band\",d3,8000000") == 1);
    TCHECK(count_exact(r.at, "AT+QCFG=\"band\",d3,8080090") == 1);
    // Das beste Band wurde NICHT gesperrt -- das ist ein eigener Schritt.
    TCHECK(r.tuner.state().desired_mask == profile_mask(BandProfile::Low, 0));
    // Und die Sicherung liest danach zurueck.
    r.ticks(2);
    TCHECK(r.tuner.state().sync == RadioSync::InSync);
}

void test_the_scan_refuses_gsm_only_and_a_second_start()
{
    TunerRig r;
    arm_modem_reports(r.at, "1a0080800d5", 1);
    r.tuner.set_config(cfg_with(BandProfile::Auto, NetMode::GsmOnly));
    r.ticks(2);
    std::string why;
    TCHECK(!r.tuner.request_scan(why));
    TCHECK(why.find("GSM") != std::string::npos);
    TCHECK(r.tuner.state().scan == ScanState::Error);

    TunerRig r2;
    arm_modem_reports(r2.at, "1a0080800d5", 0);
    r2.tuner.set_config(cfg_with(BandProfile::Auto, NetMode::Auto));
    r2.ticks(2);
    TCHECK(r2.tuner.request_scan(why));
    TCHECK(!r2.tuner.request_scan(why));
    TCHECK(why.find("already") != std::string::npos);
    // Vor dem ersten Takt: kein Modem gesehen -> abgelehnt, nicht gestartet.
    TunerRig r3;
    r3.tuner.set_config(cfg_with(BandProfile::Auto, NetMode::Auto));
    TCHECK(!r3.tuner.request_scan(why));
}

void test_a_modem_lost_mid_scan_gets_its_bands_restored_when_back()
{
    TunerRig r;
    arm_modem_reports(r.at, "8080090", 0);              // Profil "low"
    r.tuner.set_config(cfg_with(BandProfile::Low, NetMode::Auto));
    r.ticks(2);
    r.at.set_reply("AT+QCFG=\"band\",d3,10", "OK\r\n");
    r.at.set_reply("AT+QCFG=\"band\",d3,8080090", "OK\r\n");
    std::string why;
    TCHECK(r.tuner.request_scan(why));
    r.tuner.tick(r.at, r.st);            // Lock B5
    TCHECK(count_exact(r.at, "AT+QCFG=\"band\",d3,10") == 1);
    r.st.responsive = false;
    r.tuner.tick(r.at, r.st);
    TCHECK(r.tuner.state().scan == ScanState::Error);
    TCHECK(!r.tuner.scanning());
    // Das Modem kommt zurueck und steht noch auf B5 -- wie in Wirklichkeit.
    arm_modem_reports(r.at, "10", 0);
    r.st.responsive = true;
    r.ticks(3);
    // Die Absicht wird GESCHRIEBEN, nicht nur nachgesehen.
    TCHECK(count_exact(r.at, "AT+QCFG=\"band\",d3,8080090") == 1);
    // Steht das Modem dagegen schon richtig, wird nichts geschrieben.
    TunerRig r2;
    arm_modem_reports(r2.at, "4", 0);
    r2.tuner.set_config(cfg_with(BandProfile::Mid, NetMode::Auto));
    r2.ticks(2);
    r2.at.set_reply("AT+QCFG=\"band\",d3,4", "OK\r\n");
    TCHECK(r2.tuner.request_scan(why));
    r2.tuner.tick(r2.at, r2.st);
    r2.st.responsive = false;
    r2.tuner.tick(r2.at, r2.st);
    r2.st.responsive = true;
    r2.ticks(3);
    TCHECK(count_exact(r2.at, "AT+QCFG=\"band\",d3,4") == 1);   // nur der Lock des Scans
}

// ------------------------------------------------------------ Aktionen ----

struct ActionRig {
    ScriptedAtTransport at;
    Clock clock;
    ModemActions actions;
    CellularStatus st = responsive_status();
    ActionRig() { actions.set_clock([this] { return clock.t; }); }
};

void test_neighbour_cells_are_parsed_and_the_raw_text_kept()
{
    const char* raw =
        "+QENG: \"neighbourcell intra\",\"LTE\",1300,281,-11,-95,-65,12,20,7,16,3,4\r\n"
        "+QENG: \"neighbourcell inter\",\"LTE\",6300,44,-14,-102,-70,-,10,5,0,0\r\n"
        "+QENG: \"neighbourcell\",\"GSM\",1023,13,-80\r\n"
        "\r\nOK\r\n";
    const std::vector<NeighbourCell> cells = parse_qeng_neighbours(raw);
    TCHECK(cells.size() == 3);
    TCHECK(cells[0].kind == "intra" && cells[0].rat == "LTE");
    TCHECK(cells[0].earfcn.has && cells[0].earfcn.value == 1300);
    TCHECK(cells[0].pci.has && cells[0].pci.value == 281);
    // RSRQ vor RSRP -- die Reihenfolge des Handbuchs, nicht die von servingcell.
    TCHECK(cells[0].rsrq.has && cells[0].rsrq.value == -11);
    TCHECK(cells[0].rsrp.has && cells[0].rsrp.value == -95);
    TCHECK(cells[0].sinr.has && cells[0].sinr.value == 12);
    TCHECK(cells[1].kind == "inter" && !cells[1].sinr.has);      // "-" bleibt abwesend
    TCHECK(cells[2].rat == "GSM" && cells[2].kind.empty() && !cells[2].pci.has);
    TCHECK(cells[2].rssi.has && cells[2].rssi.value == -80);

    ActionRig r;
    r.at.set_reply("AT+QENG=\"neighbourcell\"", raw);
    r.actions.request_neighbours();
    r.actions.tick(r.at, r.st);
    TCHECK(r.actions.neighbours().have);
    TCHECK(r.actions.neighbours().cells.size() == 3);
    TCHECK(r.actions.neighbours().raw.find("neighbourcell intra") != std::string::npos);
    // Einmal gefragt, nicht in jedem Takt.
    r.actions.tick(r.at, r.st);
    TCHECK(r.at.count_sent("AT+QENG=\"neighbourcell\"") == 1);
}

void test_sim_lock_status_reads_clck_and_the_counters()
{
    ActionRig r;
    r.at.set_reply("AT+CLCK=\"SC\",2", "+CLCK: 1\r\nOK\r\n");
    r.at.set_reply("AT+QPINC=\"SC\"", "+QPINC: \"SC\",3,10\r\nOK\r\n");
    r.at.set_reply("AT+CPIN?", "+CPIN: READY\r\nOK\r\n");
    std::string why;
    SimLockRequest q;
    q.action = SimLockAction::Status;
    TCHECK(r.actions.request_sim_lock(q, why));
    const ModemActions::Outcome o = r.actions.tick(r.at, r.st);
    TCHECK(!o.pin_changed);
    const SimLockReport& s = r.actions.sim_lock();
    TCHECK(s.have && s.ok);
    TCHECK(s.enabled.has && s.enabled.value == 1);
    TCHECK(s.pin_left.has && s.pin_left.value == 3);
    TCHECK(s.puk_left.has && s.puk_left.value == 10);
    TCHECK(s.cpin == "READY");
    TCHECK(parse_clck("+CLCK: 0\r\nOK\r\n").has);
    TCHECK(!parse_clck("ERROR\r\n").has);
    TCHECK(!parse_qpinc("+QPINC: \"P2\",3,10\r\n").pin_left.has);
}

void test_enabling_the_lock_stores_the_pin_and_goes_out_exactly_once()
{
    ActionRig r;
    r.at.set_reply("AT+CLCK=\"SC\",1,\"4711\"", "OK\r\n");
    r.at.set_reply("AT+CLCK=\"SC\",2", "+CLCK: 1\r\nOK\r\n");
    r.at.set_reply("AT+CPIN?", "+CPIN: READY\r\nOK\r\n");
    std::string why;
    SimLockRequest q;
    q.action = SimLockAction::Enable;
    q.pin = "4711";
    TCHECK(r.actions.request_sim_lock(q, why));
    const ModemActions::Outcome o = r.actions.tick(r.at, r.st);
    TCHECK(o.pin_changed && o.new_pin == "4711");
    TCHECK(r.actions.sim_lock().ok);
    // Der Bericht traegt die PIN nicht.
    TCHECK(r.actions.sim_lock().detail.find("4711") == std::string::npos);
    // Kein zweites Mal, was immer die naechsten Takte bringen.
    for (int i = 0; i < 5; ++i) r.actions.tick(r.at, r.st);
    TCHECK(r.at.count_sent("AT+CLCK=\"SC\",1") == 1);
}

void test_a_rejected_pin_is_not_retried_and_the_report_hides_it()
{
    ActionRig r;
    r.at.set_reply("AT+CLCK=\"SC\",0,\"1234\"", "AT+CLCK=\"SC\",0,\"1234\"\r\n+CME ERROR: 16\r\n");
    r.at.set_reply("AT+CLCK=\"SC\",2", "+CLCK: 1\r\nOK\r\n");
    r.at.set_reply("AT+QPINC=\"SC\"", "+QPINC: \"SC\",2,10\r\nOK\r\n");
    r.at.set_reply("AT+CPIN?", "+CPIN: READY\r\nOK\r\n");
    std::string why;
    SimLockRequest q;
    q.action = SimLockAction::Disable;
    q.pin = "1234";
    TCHECK(r.actions.request_sim_lock(q, why));
    const ModemActions::Outcome o = r.actions.tick(r.at, r.st);
    TCHECK(!o.pin_changed);
    const SimLockReport& s = r.actions.sim_lock();
    TCHECK(!s.ok);
    TCHECK(s.detail.find("16") != std::string::npos);        // der Code
    TCHECK(s.detail.find("1234") == std::string::npos);      // nie die PIN
    TCHECK(s.pin_left.has && s.pin_left.value == 2);
    for (int i = 0; i < 3; ++i) r.actions.tick(r.at, r.st);
    TCHECK(r.at.count_sent("AT+CLCK=\"SC\",0") == 1);
}

void test_changing_the_pin_stores_the_new_one()
{
    ActionRig r;
    r.at.set_reply("AT+CPWD=\"SC\",\"1234\",\"5678\"", "OK\r\n");
    r.at.set_reply("AT+CLCK=\"SC\",2", "+CLCK: 1\r\nOK\r\n");
    std::string why;
    SimLockRequest q;
    q.action = SimLockAction::Change;
    q.pin = "1234"; q.new_pin = "5678";
    TCHECK(r.actions.request_sim_lock(q, why));
    const ModemActions::Outcome o = r.actions.tick(r.at, r.st);
    TCHECK(o.pin_changed && o.new_pin == "5678");
}

void test_malformed_pins_are_refused_before_anything_is_sent()
{
    ActionRig r;
    std::string why;
    SimLockRequest q;
    q.action = SimLockAction::Enable;
    q.pin = "12";
    TCHECK(!r.actions.request_sim_lock(q, why));
    q.action = SimLockAction::Change;
    q.pin = "1234"; q.new_pin = "";
    TCHECK(!r.actions.request_sim_lock(q, why));
    r.actions.tick(r.at, r.st);
    TCHECK(r.at.sent().empty());
}

void test_actions_against_a_silent_modem_are_answered_not_queued()
{
    ActionRig r;
    r.st.responsive = false;
    std::string why;
    SimLockRequest q;
    q.action = SimLockAction::Enable;
    q.pin = "4711";
    TCHECK(r.actions.request_sim_lock(q, why));
    r.actions.request_neighbours();
    r.actions.request_restart();
    r.actions.tick(r.at, r.st);
    TCHECK(r.at.sent().empty());
    TCHECK(r.actions.sim_lock().have && !r.actions.sim_lock().ok);
    TCHECK(r.actions.neighbours().have && !r.actions.neighbours().error.empty());
    // Kommt das Modem spaeter, geht NICHTS von selbst los.
    r.st.responsive = true;
    r.actions.tick(r.at, r.st);
    TCHECK(r.at.sent().empty());
}

void test_restart_sends_cfun_1_1_once()
{
    ActionRig r;
    r.at.set_reply("AT+CFUN=1,1", "OK\r\n");
    r.actions.request_restart();
    const ModemActions::Outcome o = r.actions.tick(r.at, r.st);
    TCHECK(o.restarted);
    r.actions.tick(r.at, r.st);
    TCHECK(r.at.count_sent("AT+CFUN=1,1") == 1);
}

// -------------------------------------------------------------- Uplink ----
//
// Der Verbund: die Absicht kommt ueber den Uplink, das Modem antwortet, die
// Abschrift zeigt es, und der Datenlink wird waehrend des Scans nicht
// angetrieben.

class AbsentEcm : public IEcmBackend {
public:
    bool find_interface(EcmInterface&) override { return false; }
    bool set_up(const std::string&, bool) override { return true; }
    bool dhcp_start(const std::string&) override { return true; }
    bool dhcp_stop(const std::string&) override { return true; }
    bool read_address(const std::string&, LinkAddress&) override { return false; }
    bool set_address(const std::string&, const LinkAddress&) override { return true; }
    void teardown(const std::string&) override {}
};

struct UplinkRig {
    ScriptedAtTransport at;
    AbsentEcm be;
    Clock clock;
    CellularService svc{at};
    EcmLink link{at, be};
    net::CellularUplink up{svc, link};
    std::vector<std::string> persisted_pins;
    UplinkRig()
    {
        svc.set_clock([this] { return clock.t; });
        link.set_clock([this] { return clock.t; });
        up.set_clock([this] { return clock.t; });
        up.set_persist([this](const CellularConfig& c) { persisted_pins.push_back(c.sim_pin); });
        at.reply("AT", "OK\r\n");
        at.reply("ATI", "Quectel\r\nEC200A\r\nOK\r\n");
        at.reply("AT+CPIN?", "+CPIN: READY\r\nOK\r\n");
        at.reply("AT+CEREG?", "+CEREG: 0,1\r\nOK\r\n");
        at.reply("AT+CSQ", "+CSQ: 22,99\r\nOK\r\n");
        at.reply("AT+COPS?", "+COPS: 0,0,\"Telekom.de\",7\r\nOK\r\n");
        at.reply("AT+QENG=\"servingcell\"",
                 "+QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",262,01,1A2B03,281,6300,20,5,5,1A2B,-95,-11,-65,9,-\r\nOK\r\n");
        arm_modem_reports(at, "1a0080800d5", 0);
        at.set_reply("AT+QCFG=\"usbnet\"", "+QCFG: \"usbnet\",1\r\nOK\r\n");
        at.set_reply("AT+QCFG=\"nat\"", "+QCFG: \"nat\",1\r\nOK\r\n");
    }
    void ticks(int n) { for (int i = 0; i < n; ++i) { up.tick(); clock.t += 2000; } }
};

void test_the_uplink_refuses_actions_while_cellular_is_off()
{
    UplinkRig r;
    CellularConfig c;
    c.enabled = false;
    r.up.set_config(c);
    r.ticks(1);
    std::string why;
    TCHECK(!r.up.request_band_scan(why) && why.find("switched off") != std::string::npos);
    TCHECK(!r.up.request_neighbour_cells(why));
    TCHECK(!r.up.request_modem_restart(why));
    SimLockRequest q; q.action = SimLockAction::Status;
    TCHECK(!r.up.request_sim_lock(q, why));
    r.ticks(2);
    TCHECK(r.at.sent().empty());
}

void test_the_uplink_carries_the_band_state_and_holds_the_link_during_a_scan()
{
    UplinkRig r;
    r.up.set_config(cfg_with(BandProfile::Mid, NetMode::Auto));
    r.at.set_reply("AT+QCFG=\"band\",d3,4", "OK\r\n");
    r.at.set_reply("AT+QCFG=\"nwscanmode\",0,1", "OK\r\n");
    r.ticks(4);
    // Die Abschrift zeigt den Tuner.
    TCHECK(r.up.radio_state().writes == 1);
    TCHECK(r.up.radio_state().desired_mask == 4);

    arm_modem_reports(r.at, "4", 0);
    r.ticks(2);
    std::string why;
    TCHECK(r.up.request_band_scan(why));
    TCHECK(!r.up.request_band_scan(why));              // schon angefordert
    const int cgdcont_before = r.at.count_sent("AT+CGDCONT");
    r.ticks(1);                                        // Absicht kommt an, Lock B3
    TCHECK(r.up.radio_state().scan == ScanState::Running);
    TCHECK(r.up.state() != net::LinkState::Connected);
    TCHECK(r.up.detail().find("band scan") != std::string::npos);
    // Waehrend des Scans: kein Antrieb des Datenlinks.
    r.ticks(3);
    TCHECK(r.at.count_sent("AT+CGDCONT") == cgdcont_before);
}

void test_a_sim_action_through_the_uplink_persists_the_pin()
{
    UplinkRig r;
    r.up.set_config(cfg_with(BandProfile::Auto, NetMode::Auto));
    r.ticks(2);
    r.at.set_reply("AT+CLCK=\"SC\",1,\"4711\"", "OK\r\n");
    r.at.set_reply("AT+CLCK=\"SC\",2", "+CLCK: 1\r\nOK\r\n");
    std::string why;
    SimLockRequest q; q.action = SimLockAction::Enable; q.pin = "4711";
    TCHECK(r.up.request_sim_lock(q, why));
    r.ticks(2);
    TCHECK(r.persisted_pins.size() == 1 && r.persisted_pins[0] == "4711");
    TCHECK(r.up.config().sim_pin == "4711");
    TCHECK(r.up.sim_lock().have && r.up.sim_lock().ok);
}

} // namespace

void run_band_plan_tests()
{
    std::printf("== Mobilfunk: Bandwahl, Scan, Aktionen ==\n");
    test_the_band_table_is_the_eu_set_with_the_reference_bits();
    test_profiles_give_the_reference_masks();
    test_masks_round_trip_through_band_numbers_and_hex();
    test_net_mode_maps_to_nwscanmode_and_back();
    test_qcfg_band_readback_accepts_both_hex_spellings_and_nothing_else();

    test_a_modem_that_matches_is_left_alone();
    test_auto_accepts_a_factory_mask_that_is_a_superset();
    test_a_change_is_written_once_with_the_reference_sequence();
    test_a_rejected_mask_is_reported_and_not_retried_and_rf_comes_back();
    test_an_unreadable_report_writes_nothing_at_boot_but_a_change_still_does();
    test_a_modem_from_elsewhere_is_brought_in_line_once();
    test_a_rollback_rewrites_the_old_values();
    test_a_modem_that_goes_away_is_checked_again_when_it_returns();
    test_an_empty_selection_is_never_written();

    test_the_scan_locks_each_band_measures_medians_and_restores();
    test_the_scan_refuses_gsm_only_and_a_second_start();
    test_a_modem_lost_mid_scan_gets_its_bands_restored_when_back();

    test_neighbour_cells_are_parsed_and_the_raw_text_kept();
    test_sim_lock_status_reads_clck_and_the_counters();
    test_enabling_the_lock_stores_the_pin_and_goes_out_exactly_once();
    test_a_rejected_pin_is_not_retried_and_the_report_hides_it();
    test_changing_the_pin_stores_the_new_one();
    test_malformed_pins_are_refused_before_anything_is_sent();
    test_actions_against_a_silent_modem_are_answered_not_queued();
    test_restart_sends_cfun_1_1_once();

    test_the_uplink_refuses_actions_while_cellular_is_off();
    test_the_uplink_carries_the_band_state_and_holds_the_link_during_a_scan();
    test_a_sim_action_through_the_uplink_persists_the_pin();
}
