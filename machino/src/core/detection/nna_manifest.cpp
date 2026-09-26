#include "core/detection/nna_manifest.hpp"
#include "core/json.hpp"

namespace machino { namespace detection {

namespace {

std::string str_field(const Json& o, const char* k)
{
    const Json* v = o.get(k);
    return v && v->is_string() ? v->as_string() : std::string();
}

NnaManifestCheck fail(const char* code, std::string detail)
{
    NnaManifestCheck c;
    c.reason_code = code;
    c.detail = std::move(detail);
    return c;
}

} // namespace

NnaManifestCheck nna_manifest_check(const std::string& manifest_json,
                                    const std::string& model_basename,
                                    const std::string& soc)
{
    Json m;
    std::string err;
    if (!Json::parse(manifest_json, m, err) || !m.is_object())
        return fail("AI_MANIFEST_INVALID", "manifest.json nicht lesbar: " + err);

    const Json* sv = m.get("schemaVersion");
    if (!sv || !sv->is_number() || (int)sv->as_number() != 1)
        return fail("AI_MANIFEST_INVALID", "schemaVersion fehlt oder != 1");

    // Backend und NNA-Generation sind die HARTEN Gates: ein Modell fuer eine
    // andere Runtime laedt bestenfalls nicht — schlimmstenfalls rechnet es
    // Unsinn, und das faellt erst im Feld auf.
    const std::string backend = str_field(m, "backend");
    if (backend != "venus-nna")
        return fail("AI_MODEL_INCOMPATIBLE_BACKEND",
                    "Manifest nennt Backend '" + backend + "', dieser Detector ist venus-nna");
    const std::string gen = str_field(m, "nnaGeneration");
    if (gen != "nna1")
        return fail("AI_MODEL_INCOMPATIBLE_NNA",
                    "Manifest nennt NNA-Generation '" + gen + "', der T40 ist nna1");
    // SoC ist optional (leer = jede); wenn gesetzt, muss es dieser sein.
    const std::string msoc = str_field(m, "soc");
    if (!msoc.empty() && msoc != soc)
        return fail("AI_MODEL_INCOMPATIBLE_SOC",
                    "Manifest ist fuer SoC '" + msoc + "' gebaut, diese Kamera ist '" + soc + "'");
    // modelFile bindet Manifest und Datei aneinander: ein manifest.json neben
    // einer FREMDEN .bin (Datei getauscht, Manifest vergessen) faellt hier auf.
    const std::string mf = str_field(m, "modelFile");
    if (!mf.empty() && mf != model_basename)
        return fail("AI_MODEL_FILE_MISMATCH",
                    "Manifest gehoert zu '" + mf + "', ai.model_path zeigt auf '"
                        + model_basename + "'");

    NnaManifestCheck c;
    c.ok = true;
    return c;
}

}} // namespace machino::detection
