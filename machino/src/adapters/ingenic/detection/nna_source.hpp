// IAnalysisSource auf Ingenic: ein eigener, herunterskalierter
// FrameSource-Kanal, aus dem IMP_FrameSource_GetFrame NV12-Bilder liefert.
// Dieselbe Kanal-Disziplin wie beim Motion-Backend: UNIT_AI ist der
// Analyse-Kanal, main/sub/jpeg bleiben unberuehrt.
#pragma once
#include "ports/ianalysis_source.hpp"
#include <memory>

namespace machino { namespace ingenic {

// Erzeugt die Quelle oder nullptr (Kanal liess sich nicht anlegen). aw/ah
// ist die Analysegeometrie, fps die Inferenzkadenz -- der Kanal liefert
// nicht schneller, als die NNA fragen wird.
std::unique_ptr<IAnalysisSource> create_nna_source(int chn, int aw, int ah, int fps,
                                                   int native_w, int native_h);

}} // namespace machino::ingenic
