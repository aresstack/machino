// Das Modell-Manifest (AP-NNA4): ein Modell darf nicht auf jede Runtime
// losgelassen werden. Neben der Modelldatei kann ein manifest.json liegen
// (die CI erzeugt es zusammen mit dem Artefakt); WENN es da ist, muss es
// passen — Backend, NNA-Generation, SoC, Dateiname. Fehlt es, bleibt eine
// nackte .bin nutzbar (Entwicklungsmodus), und genau das steht dann im Log.
//
// Die Pruefung ist eine reine Funktion ueber dem JSON-Text, damit sie
// hosttestbar ist; Datei-I/O macht der Aufrufer (die Ingenic-Fabrik).
// Bei Ablehnung traegt das Ergebnis einen STABILEN Reason-Code — "model
// incompatible" ohne Warum war der ausdruecklich verbotene Zustand.
#pragma once
#include <string>

namespace machino { namespace detection {

struct NnaManifestCheck {
    bool ok = false;
    std::string reason_code; // stabil, z.B. AI_MODEL_INCOMPATIBLE_NNA
    std::string detail;      // menschenlesbar, konkret
};

// manifest_json: Dateiinhalt; model_basename: Dateiname des Modells, auf das
// ai.model_path zeigt (Manifest.modelFile muss dazu passen, wenn gesetzt).
// soc: der LAUFENDE SoC (hw.platform.model, z.B. "t40nn") -- ein Manifest
// mit gesetztem soc muss exakt dazu passen. Ein Literal im Checker waere auf
// T40N/T40XP eine stille Fehlpassung gewesen (AP4-Review).
NnaManifestCheck nna_manifest_check(const std::string& manifest_json,
                                    const std::string& model_basename,
                                    const std::string& soc);

}} // namespace machino::detection
