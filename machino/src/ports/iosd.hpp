// Port: on-screen display.
//
// The split here is deliberate and follows what is actually platform-specific.
//
//   PLACEMENT AND TEXT are not. Resolving "2%" against a frame width, mapping
//   majestic's -16..16 proportional grid onto pixels, and expanding a strftime
//   template are the same arithmetic on every SoC, so they live in OsdService
//   and are host-tested.
//
//   MEASURING and DRAWING are. Only the backend knows how wide a string comes
//   out in the font it has, and only the backend can hand pixels to hardware.
//
// So the backend measures, the service places, the backend draws. That is why
// `measure()` exists as its own call rather than the service guessing glyph
// widths: a rectangle in the /api/v1/osd report is read by the stock settings
// page to count regions and bytes, and a guessed rectangle there would be a
// printed number that is quietly wrong.
//
// No IMP type appears in this header, and no IMP call may be made outside an
// implementation of this interface.
#pragma once
#include "core/result.hpp"
#include <cstdint>
#include <string>

namespace machino {

// Where the text is measured from. Mirrors majestic's osd.anchor enum exactly;
// `Proportional` is the legacy posX/posY grid and ignores the offsets.
enum class OsdAnchor : int {
    Proportional = 0,
    TopLeft, Top, TopRight,
    Left, Center, Right,
    BottomLeft, Bottom, BottomRight,
    COUNT
};

const char* osd_anchor_name(OsdAnchor a);
bool        osd_anchor_parse(const std::string& s, OsdAnchor& out);

struct OsdRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const OsdRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

// What the backend reports about a string in its own font.
struct OsdMetrics {
    int w = 0;    // advance width in frame pixels
    int h = 0;    // line height in frame pixels
    int em = 0;   // the em box, so the service can resolve a "1.5em" offset
};

// One attachment: this overlay, drawn on this output frame.
struct OsdDraw {
    int         overlay = 0;          // overlay index; 0 is the text overlay
    int         unit = 0;             // stream unit the frame belongs to
    int         frame_w = 0, frame_h = 0;
    OsdRect     rect;                 // resolved by the service, in frame pixels
    std::string text;                 // template already expanded
    double      size = 1.0;           // font scale factor
    bool        thin = false;
    bool        outline = true;
    int         bg_alpha = 25;        // 0..100, plate behind the text
};

class IOsdBackend {
public:
    virtual ~IOsdBackend() = default;

    // False means "this build cannot draw an overlay". The service then still
    // serves config and still validates, but /api/v1/osd answers 404 - which
    // the stock page handles as "this camera cannot say" and stops asking.
    virtual bool available() const = 0;

    // How many overlay regions the hardware can carry at once, for the
    // `budget` field. 0 = unknown, and the field is then omitted rather than
    // guessed.
    virtual int overlay_budget() const { return 0; }

    virtual OsdMetrics measure(const std::string& text, double size, int frame_w) const = 0;

    virtual Result draw(const OsdDraw& d) = 0;
    virtual Result clear(int overlay, int unit) = 0;

    // Logo overlay. Pixels are BGRA, w*h*4 bytes, which is the wire format the
    // stock settings page both uploads and reads back.
    virtual Result set_image(int overlay, int unit, int w, int h, const uint8_t* bgra) {
        (void)overlay; (void)unit; (void)w; (void)h; (void)bgra;
        return Result::unsupported();
    }
};

// The backend on a build with no overlay hardware wired up yet. It measures
// with a DECLARED model rather than a real font: advance 0.6 em, line height
// 1.25 em, em = 32 px at size 1.0 scaled by frame width against a 1920 px
// reference. That model is honest about being a model - `available()` is
// false, so nothing publishes its rectangles as facts about a camera.
class SoftOsdBackend : public IOsdBackend {
public:
    bool available() const override { return false; }
    OsdMetrics measure(const std::string& text, double size, int frame_w) const override;
    Result draw(const OsdDraw&) override { return Result::unsupported(); }
    Result clear(int, int) override { return Result::unsupported(); }
};

} // namespace machino
