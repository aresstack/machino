// OSD service: owns the overlay state, resolves placement, and serves the two
// documents the unmodified majestic-webui asks for.
//
// It sits between the HTTP layer and the backend and is the ONLY place that
// turns config into draw calls, so no IMP call is ever reachable from the
// HTTP or config code.
//
// Threading contract, which exists because an overlay must never be able to
// stall video: every public method takes the service's own mutex and NOTHING
// ELSE - no pipeline lock, no StreamHub lock, ever.
//
// The backend is MUTATED (draw/clear/set_image) only from pump(), which the
// pipeline thread calls, and pump() releases the mutex before it touches the
// backend. report() additionally calls measure(), which is a pure const query
// - the rectangles it publishes have to come from the real font metrics, so
// it cannot be avoided, and an implementation must keep it side-effect free.
//
// The consequence worth stating: a slow or wedged backend delays the next
// overlay update and nothing else. It is never called while a lock the media
// path wants is held.
#pragma once
#include "core/config.hpp"
#include "core/json.hpp"
#include "ports/iosd.hpp"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace machino { namespace osd {

// Resolve one of majestic's length specs against a frame extent.
// "12" = pixels, "2%" = share of the extent, "1.5em" = multiple of the text
// size. Returns false on anything else - fail closed, because a silently
// zeroed offset looks like a working setting that does not work.
bool resolve_length(const std::string& spec, int extent, int em, int& out);

// Place a measured text box in a frame. `posx`/`posy` are majestic's legacy
// -16..16 grid (16 = left/top, -16 = right/bottom) and apply only when the
// anchor is Proportional; the offsets apply otherwise.
OsdRect place(OsdAnchor anchor, int offx, int offy, int posx, int posy,
              int text_w, int text_h, int frame_w, int frame_h);

// Expand a majestic OSD template. strftime plus `%f` for milliseconds.
// `%@` (lens zoom magnification) expands to nothing here: this camera has no
// motorised lens to report, and printing a made-up magnification would be
// worse than printing none.
std::string expand_template(const std::string& tpl, int64_t unix_s, int ms);

// One output the overlay can be drawn on, as the pipeline currently has it.
struct StreamGeometry {
    int  unit = 0;
    int  w = 0, h = 0;
    bool osd_on = true;      // majestic video<N>.osd
};

struct ImageInfo {
    int w = 0, h = 0, ref = 0;
};

class OsdService {
public:
    // A logo bigger than this cannot be justified on this camera: the store
    // lives on an overlay filesystem with single-digit megabytes free, and the
    // hardware keeps two buffers per region. 512x512 BGRA is exactly 1 MiB and
    // is already far larger than any watermark anyone has asked for.
    static constexpr int    MAX_OVERLAY_INDEX = 3;
    static constexpr int    MAX_IMAGE_DIM     = 512;
    static constexpr size_t MAX_IMAGE_BYTES   = 512u * 512u * 4u;

    OsdService(IOsdBackend& backend, std::string image_dir);

    void set_config(const OsdConfig& cfg);
    OsdConfig config() const;

    // Pushed by the pipeline whenever the set of outputs changes.
    void set_streams(const std::vector<StreamGeometry>& s);

    // The GET /api/v1/osd document. Returns false when this build cannot say,
    // which the caller turns into a 404 - the stock page treats that as a fact
    // about the build and stops asking.
    bool report(Json& out) const;

    // Push the current plan at the backend. Pipeline thread only.
    void pump(int64_t unix_s, int ms);

    // --- image store -----------------------------------------------------
    // status is the HTTP status to answer with; message is the plain-text body
    // the settings page shows when an upload is refused.
    struct ImageResult {
        int         status = 200;
        std::string message;
        bool ok() const { return status >= 200 && status < 300; }
    };

    ImageResult store_image(int overlay, int w, int h, int ref,
                            const uint8_t* bgra, size_t len);
    ImageResult delete_image(int overlay);
    bool        load_image(int overlay, ImageInfo& info, std::string& bgra) const;

private:
    struct Plan {
        bool        draw = false;
        std::string text;
        std::vector<OsdDraw> items;
    };

    Plan build_plan_locked(int64_t unix_s, int ms) const;
    std::string image_path(int overlay) const;

    IOsdBackend&                backend_;
    std::string                 dir_;
    mutable std::mutex          m_;
    OsdConfig                   cfg_;
    std::vector<StreamGeometry> streams_;
    std::string                 last_text_;      // so an unchanged second is not redrawn
    std::vector<OsdRect>        last_rects_;
    std::vector<int>            last_units_;     // so a switch-off clears the units it drew on
    bool                        drew_ = false;
};

}} // namespace machino::osd
