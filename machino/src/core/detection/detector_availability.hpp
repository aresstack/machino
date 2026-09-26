// Der EINE Availability-Vertrag (AP-NNA5): welche Detectoren es gibt, ob sie
// JETZT nutzbar sind, und wenn nicht, WARUM — mit stabilen Reason-Codes.
// Keine zweite Zustandslogik: die Ingenic-Fabrik und die API lesen dieselbe
// Bewertung; der Adapter sammelt nur die Fakten (I/O), die Bewertung ist
// eine reine Funktion und damit hostgetestet (Availability-Matrix).
//
// Wichtig ist die Trennung, die die Spec verlangt: ai.person = "vom Code
// unterstuetzt" bleibt eine STATISCHE Capability (Unknown bis zur Hardware-
// Abnahme); dieses Modul beantwortet die LAUFZEIT-Frage "geht es auf dieser
// Kamera in diesem Moment" — beides zusammen ist die Wahrheit, keins allein.
#pragma once
#include <string>
#include <vector>

namespace machino { namespace detection {

struct DetectorStatus {
    std::string id;        // "motion" | "person"
    std::string label;     // UI-Text ("Person (NNA)")
    bool available = false;   // alle Laufzeitvoraussetzungen erfuellt
    bool selectable = false;  // == available (getrennt, falls das je auseinanderfaellt)
    std::vector<std::string> reason_codes;   // stabil, leere Liste wenn available
    std::vector<std::string> reason_details; // menschenlesbar, parallel zu codes
};

// Die vom Adapter GESAMMELTEN Fakten fuer den person-Detector. Reine Daten:
// wer sie fuellt, macht I/O; wer sie bewertet, nicht.
struct NnaFacts {
    std::string soc;              // hw.platform.model, z.B. "t40nn"
    bool cmdline_has_nmem = false;   // /proc/cmdline traegt nmem= (dieser Boot!)
    bool device_node = false;        // /dev/soc-nna existiert
    bool helper_exec = false;        // /usr/sbin/machino-nna ausfuehrbar
    std::string model_path;          // ai.model_path (roh, fuer Meldungen)
    bool model_readable = false;     // Datei lesbar und nicht leer
    bool manifest_present = false;   // manifest.json neben dem Modell
    std::string manifest_json;       // dessen Inhalt (leer wenn nicht da)
};

// Bewertet person. VOLLSTAENDIGE Liste in deterministischer Reihenfolge
// (Boot-Reservierung zuerst: sie ist die Voraussetzung aller weiteren) —
// die Spec verlangt ausdruecklich alle Gruende, nicht nur den ersten.
// Ein fehlendes Manifest ist KEIN Grund (Entwicklungsmodus, wird geloggt);
// ein vorhandenes, unpassendes schon.
DetectorStatus evaluate_person(const NnaFacts& f);

// motion auf einer Plattform, deren Capability es traegt: immer verfuegbar.
DetectorStatus evaluate_motion(bool platform_supports_motion);

}} // namespace machino::detection
