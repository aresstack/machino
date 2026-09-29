#include "core/cellular/radio_tuner.hpp"

#include <algorithm>

namespace machino { namespace cellular {

namespace {

MaybeInt median_of(std::vector<int> v)
{
    if (v.empty()) return MaybeInt();
    std::sort(v.begin(), v.end());
    return MaybeInt(v[v.size() / 2]);
}

std::string band_list(uint64_t mask)
{
    std::string s;
    for (int b : bands_from_mask(mask)) {
        if (!s.empty()) s += " ";
        s += "B" + std::to_string(b);
    }
    return s.empty() ? std::string("(none)") : s;
}

} // namespace

const char* radio_sync_name(RadioSync s)
{
    switch (s) {
        case RadioSync::InSync:     return "in-sync";
        case RadioSync::Pending:    return "pending";
        case RadioSync::Failed:     return "failed";
        case RadioSync::Unverified: return "unverified";
        case RadioSync::Unknown:    break;
    }
    return "unknown";
}

const char* scan_state_name(ScanState s)
{
    switch (s) {
        case ScanState::Running: return "running";
        case ScanState::Done:    return "done";
        case ScanState::Error:   return "error";
        case ScanState::Idle:    break;
    }
    return "idle";
}

void RadioTuner::set_config(const CellularConfig& c)
{
    const uint64_t mask = profile_mask(c.band_profile, c.band_mask);
    const bool changed = have_cfg_ &&
        (mask != st_.desired_mask || c.net_mode != st_.desired_mode);
    have_cfg_ = true;
    st_.desired_mask = mask;
    st_.desired_mode = c.net_mode;
    if (!changed) return;

    // Eine neue Absicht. Auch dann, wenn gerade ein Scan laeuft: der Scan
    // stellt am Ende die dann gueltige Maske wieder her, und die Pruefung
    // danach schreibt, was fehlt.
    apply_pending_ = true;
    verified_ = false;
    st_.sync = RadioSync::Pending;
    st_.detail = "change waiting for the modem";
}

bool RadioTuner::mask_in_sync(uint64_t modem_mask) const
{
    // Nur die Bits unserer Tabelle zaehlen. Ein Modem darf Bits fuer Baender
    // fuehren, die es aus unserer Sicht nicht gibt; sie zu vergleichen hiesse,
    // bei jedem Start zu schreiben, was wir gar nicht meinen. Bei "auto" ist
    // die Werksmaske (alle Baender des Geraets) damit in Ordnung, ohne dass
    // sie je angefasst wird.
    return (modem_mask & all_supported_mask()) == st_.desired_mask;
}

bool RadioTuner::request_scan(std::string& why_not)
{
    why_not.clear();
    if (!alive_) {
        why_not = "no modem answering";
    } else if (st_.scan == ScanState::Running) {
        why_not = "a scan is already running";
    } else if (st_.desired_mode == NetMode::GsmOnly) {
        why_not = "the SINR scan needs LTE - the network mode is GSM only";
    } else if (bands_from_mask(st_.desired_mask).empty()) {
        why_not = "no LTE band in the active profile";
    } else if (phase_ != Phase::Idle) {
        why_not = "a band change is being written - try again in a moment";
    }
    if (!why_not.empty()) {
        st_.scan = ScanState::Error;
        st_.scan_detail = why_not;
        return false;
    }
    scan_bands_ = bands_from_mask(st_.desired_mask);
    scan_idx_ = 0;
    scan_phase_ = ScanPhase::Lock;
    st_.rows.clear();
    st_.scan = ScanState::Running;
    st_.scan_best = 0;
    st_.scan_current = scan_bands_.front();
    st_.scan_detail = "starting";
    return true;
}

void RadioTuner::on_modem_gone()
{
    // Ein neuer Lebenszyklus. Alles, was "einmal je Lebenszyklus" war, gilt
    // neu, und die Rueckmeldung wird wieder gelesen, sobald es antwortet.
    alive_ = false;
    verified_ = false;
    phase_ = Phase::Idle;
    st_.writes = 0;
    st_.modem_mask = Maybe<uint64_t>();
    st_.modem_nwscanmode = MaybeInt();
    if (st_.scan == ScanState::Running) {
        st_.scan = ScanState::Error;
        st_.scan_detail = "the modem disappeared during the scan - the configured "
                          "bands are restored once it is back";
        st_.scan_current = 0;
        // Das Modem steht auf dem zuletzt gesperrten Band. Sobald es wieder da
        // ist, wird die Absicht geschrieben, nicht nur gelesen: die Maske im
        // Modem ist dann bekannt falsch.
        restore_pending_ = true;
    }
}

bool RadioTuner::tick(IAtTransport& at, const CellularStatus& s)
{
    if (!s.present || !s.responsive) {
        if (alive_) on_modem_gone();
        return false;
    }
    alive_ = true;

    if (st_.scan == ScanState::Running) {
        scan_step(at, s);
        return true;
    }

    if (phase_ == Phase::RfOff) {
        write_step(at);
        return true;
    }

    if (!have_cfg_) return false;

    if (restore_pending_) {
        restore_pending_ = false;
        apply_pending_ = true;
        verified_ = false;
    }

    if (st_.desired_mask == 0) {
        // Kann nur aus einer von Hand editierten Datei kommen; die API lehnt
        // eine leere Auswahl ab. Nichts schreiben: eine Maske 0 hiesse "kein
        // Band", und das Modem faende nie ein Netz.
        st_.sync = RadioSync::Failed;
        st_.detail = "no LTE band selected - nothing written";
        apply_pending_ = false;
        verified_ = true;
        return false;
    }

    if (!apply_pending_ && verified_) return false;

    // ---- zurueckLESEN -----------------------------------------------------
    const QcfgBand qb = parse_qcfg_band(at.command("AT+QCFG=\"band\"").raw);
    const MaybeInt nw = parse_qcfg_int(at.command("AT+QCFG=\"nwscanmode\"").raw, "nwscanmode");
    st_.modem_mask = qb.lte;
    st_.modem_nwscanmode = nw;

    const bool readable = qb.lte.has && nw.has;
    const bool in_sync  = readable && mask_in_sync(qb.lte.value) &&
                          nw.value == net_mode_nwscanmode(st_.desired_mode);
    if (in_sync) {
        verified_ = true;
        apply_pending_ = false;
        st_.sync = RadioSync::InSync;
        st_.detail = st_.writes > 0 ? "applied and confirmed by the modem"
                                    : "the modem matches the configuration";
        return false;
    }

    if (!apply_pending_) {
        // Nur nachsehen. Geschrieben wird hier nur, wenn die Abweichung
        // eindeutig lesbar ist UND in diesem Lebenszyklus noch nie geschrieben
        // wurde -- alles andere ist ein Befund, kein Auftrag.
        if (!readable) {
            verified_ = true;
            st_.sync = RadioSync::Unverified;
            st_.detail = "the modem's band report is not readable - nothing written";
            return false;
        }
        if (st_.writes > 0) {
            verified_ = true;
            st_.sync = RadioSync::Failed;
            st_.detail = "the modem reports " + band_list(qb.lte.value & all_supported_mask()) +
                         " after the write, configured is " + band_list(st_.desired_mask) +
                         " - not written again";
            return false;
        }
    }

    // ---- schreiben, erste Haelfte: RF aus ---------------------------------
    apply_pending_ = false;
    const AtExchange off = at.command("AT+CFUN=0", 10000);
    if (!off.ok()) {
        verified_ = true;
        st_.sync = RadioSync::Failed;
        st_.detail = "AT+CFUN=0 rejected - nothing written";
        return false;
    }
    phase_ = Phase::RfOff;
    st_.sync = RadioSync::Pending;
    st_.detail = "radio off, writing bands and network mode";
    return true;
}

void RadioTuner::write_step(IAtTransport& at)
{
    // Genau die Sequenz der Referenz, samt der EC200A-Eigenheiten: bare-Hex,
    // zwei Parameter, "d3" fuer GSM. Siehe band_plan.hpp.
    const std::string hex = mask_hex(st_.desired_mask);
    const AtExchange b = at.command("AT+QCFG=\"band\",d3," + hex);
    const AtExchange n = at.command("AT+QCFG=\"nwscanmode\"," +
                                    std::to_string(net_mode_nwscanmode(st_.desired_mode)) + ",1");
    // RF IMMER wieder an -- auch nach einer Ablehnung. Ein Modem mit RF aus
    // ist ein Modem ohne Netz, und das waere die schlechteste Art zu
    // scheitern.
    at.command("AT+CFUN=1", 10000);
    phase_ = Phase::Idle;
    ++st_.writes;

    if (!b.ok() || !n.ok()) {
        // Kein zweiter Versuch. Die zuletzt gelesene Maske bleibt stehen --
        // sie IST der Stand des Modems, weil nichts davon angenommen wurde.
        verified_ = true;
        st_.sync = RadioSync::Failed;
        st_.detail = !b.ok()
            ? "the modem rejected the band mask " + hex + " (" + band_list(st_.desired_mask) +
              ") - a band outside this variant's set?"
            : "the modem rejected the network mode";
        return;
    }
    verified_ = false;          // naechster Takt liest zurueck
    st_.sync = RadioSync::Pending;
    st_.detail = "written, waiting for the modem to confirm";
}

// ---------------------------------------------------------------- Scan ----

bool RadioTuner::lock_mask(IAtTransport& at, uint64_t mask)
{
    if (!mask) return false;
    if (!at.command("AT+CFUN=0", 10000).ok()) return false;
    const AtExchange cfg = at.command("AT+QCFG=\"band\",d3," + mask_hex(mask));
    const AtExchange on  = at.command("AT+CFUN=1", 10000);
    return cfg.ok() && on.ok();
}

void RadioTuner::scan_advance()
{
    ++scan_idx_;
    scan_phase_ = scan_idx_ < scan_bands_.size() ? ScanPhase::Lock : ScanPhase::Restore;
}

void RadioTuner::scan_finish()
{
    int best = 0;
    MaybeInt best_sinr, best_rsrp;
    for (const ScanRow& r : st_.rows) {
        if (!r.ok || !r.sinr.has) continue;
        const bool better = !best_sinr.has || r.sinr.value > best_sinr.value ||
            (r.sinr.value == best_sinr.value && r.rsrp.has &&
             (!best_rsrp.has || r.rsrp.value > best_rsrp.value));
        if (better) { best = r.band; best_sinr = r.sinr; best_rsrp = r.rsrp; }
    }
    st_.scan_current = 0;
    st_.scan_best = best;
    if (!best) {
        st_.scan = ScanState::Error;
        st_.scan_detail = "no usable SINR measurement on any band - the configured bands are restored";
    } else {
        st_.scan = ScanState::Done;
        st_.scan_detail = "best band: B" + std::to_string(best) +
                          " (SINR " + std::to_string(best_sinr.value) + " dB" +
                          (best_rsrp.has ? ", RSRP " + std::to_string(best_rsrp.value) + " dBm" : "") +
                          "). The configured bands are restored; lock it as a separate step.";
    }
    // Die Sicherung liest jetzt zurueck und schreibt, falls die Wieder-
    // herstellung nicht gegriffen hat.
    verified_ = false;
}

void RadioTuner::scan_step(IAtTransport& at, const CellularStatus& s)
{
    switch (scan_phase_) {
    case ScanPhase::Lock: {
        const LteBand* b = lte_band_by_number(scan_bands_[scan_idx_]);
        ScanRow row;
        row.band = b->num;
        row.mhz  = b->mhz;
        st_.rows.push_back(row);
        st_.scan_current = b->num;
        st_.scan_detail = "measuring B" + std::to_string(b->num);
        sinr_.clear(); rsrp_.clear(); rsrq_.clear();
        if (!lock_mask(at, 1ULL << b->bit)) {
            // Abgelehnt: dieses Band kennt die Variante nicht. Weiter, nicht
            // abbrechen -- die anderen Baender sind trotzdem messbar.
            scan_advance();
            return;
        }
        scan_deadline_ms_ = now() + kBandTimeoutMs;
        scan_phase_ = ScanPhase::Measure;
        return;
    }
    case ScanPhase::Measure: {
        ScanRow& row = st_.rows.back();
        // Nur ein Messwert, der nachweislich vom erwarteten Band kommt. Direkt
        // nach dem Lock meldet QENG fuer einen Moment noch die alte Zelle.
        if (reg_is_registered(s.registration) && s.cell.band.has &&
            s.cell.band.value == row.band && s.cell.sinr.has) {
            sinr_.push_back(s.cell.sinr.value);
            if (s.cell.rsrp.has) rsrp_.push_back(s.cell.rsrp.value);
            if (s.cell.rsrq.has) rsrq_.push_back(s.cell.rsrq.value);
            row.samples = (int)sinr_.size();
            if (row.samples >= kSamplesPerBand) {
                row.ok   = true;
                row.sinr = median_of(sinr_);
                row.rsrp = median_of(rsrp_);
                row.rsrq = median_of(rsrq_);
                scan_advance();
            }
            return;
        }
        if (now() >= scan_deadline_ms_) {
            // Kein Netz auf diesem Band an diesem Ort. Das ist ein Ergebnis,
            // kein Fehler.
            row.ok = false;
            scan_advance();
        }
        return;
    }
    case ScanPhase::Restore:
        // Ob das Wiederherstellen gelingt, prueft die Sicherung danach; hier
        // wird nur der Versuch gemacht.
        lock_mask(at, st_.desired_mask);
        scan_finish();
        return;
    }
}

}} // namespace machino::cellular
