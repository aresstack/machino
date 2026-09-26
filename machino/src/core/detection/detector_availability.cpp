#include "core/detection/detector_availability.hpp"
#include "core/detection/nna_manifest.hpp"

namespace machino { namespace detection {

DetectorStatus evaluate_motion(bool platform_supports_motion)
{
    DetectorStatus s;
    s.id = "motion";
    s.label = "Motion";
    s.available = platform_supports_motion;
    s.selectable = s.available;
    if (!platform_supports_motion) {
        s.reason_codes.push_back("MOTION_PLATFORM_UNSUPPORTED");
        s.reason_details.push_back("Diese Plattform traegt kein IMP-IVS-Motion-Backend.");
    }
    return s;
}

DetectorStatus evaluate_person(const NnaFacts& f)
{
    DetectorStatus s;
    s.id = "person";
    s.label = "Person (NNA)";

    auto add = [&s](const char* code, std::string detail) {
        s.reason_codes.push_back(code);
        s.reason_details.push_back(std::move(detail));
    };

    // Reihenfolge = Abhaengigkeitskette: ohne Boot-Reservierung gibt es den
    // Treiber nicht, ohne Treiber kein Geraet — der Benutzer soll das ERSTE
    // fehlende Glied zuerst lesen, aber alle sehen.
    if (f.soc != "t40nn")
        add("NNA_PLATFORM_UNSUPPORTED",
            "SoC '" + f.soc + "': das NNA-Fenster ist nur fuer den T40NN vermessen.");
    if (!f.cmdline_has_nmem)
        add("NNA_BOOT_MEMORY_MISSING",
            "Kein nmem in der gebooteten Cmdline - Reservierung ueber den "
            "Cam-Tool-NNA-Dialog setzen, dann neu starten.");
    if (!f.device_node)
        add("NNA_DEVICE_MISSING",
            "/dev/soc-nna fehlt - soc-nna.ko nicht geladen.");
    if (!f.helper_exec)
        add("NNA_RUNTIME_MISSING",
            "/usr/sbin/machino-nna fehlt - NNA-Payload nicht installiert.");
    if (!f.model_readable)
        add("AI_MODEL_MISSING",
            "Modell '" + f.model_path + "' nicht lesbar - ai.model_path pruefen.");
    else if (f.manifest_present) {
        // Nur ein VORHANDENES Manifest kann ablehnen; ohne ist es der
        // gesagte Entwicklungsmodus.
        const size_t slash = f.model_path.find_last_of('/');
        const std::string base =
            slash == std::string::npos ? f.model_path : f.model_path.substr(slash + 1);
        const NnaManifestCheck chk = nna_manifest_check(f.manifest_json, base, f.soc);
        if (!chk.ok)
            add(chk.reason_code.c_str(), chk.detail);
    }

    s.available = s.reason_codes.empty();
    s.selectable = s.available;
    return s;
}

}} // namespace machino::detection
