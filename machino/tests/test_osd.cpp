// AP9: OSD service. The contract under test is the one the UNMODIFIED
// majestic-webui asks for - the /api/v1/osd geometry document and the
// /api/v1/osd/image store - plus the placement arithmetic that turns the
// stock settings page's fields into a rectangle.
#include "app/osd/osd_service.hpp"
#include "core/config.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace machino;
using namespace machino::osd;

extern int g_fail_ext, g_pass_ext;
#define LCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// A backend that CAN draw, with metrics simple enough to assert against by
// hand: em = 10 px per unit of scale regardless of frame, advance 10 px a
// character, line height 10 px. Counts every call so the tests can prove the
// service does not touch it from the request path.
struct TestBackend : IOsdBackend {
    bool avail = true;
    int  budget = 8;
    mutable int measures = 0;
    int draws = 0, clears = 0;
    std::vector<OsdDraw> last;

    bool available() const override { return avail; }
    int  overlay_budget() const override { return budget; }
    OsdMetrics measure(const std::string& text, double size, int frame_w) const override {
        (void)frame_w;
        ++measures;
        OsdMetrics m;
        m.em = (int)(10 * size);
        m.w = (int)(10 * size) * (int)text.size();
        m.h = (int)(10 * size);
        return m;
    }
    Result draw(const OsdDraw& d) override { ++draws; last.push_back(d); return Result::ok(); }
    Result clear(int, int) override { ++clears; return Result::ok(); }
};

std::vector<StreamGeometry> two_streams(bool main_on = true, bool sub_on = true) {
    return {StreamGeometry{0, 1920, 1080, main_on}, StreamGeometry{1, 640, 360, sub_on}};
}

OsdConfig fixed_text_config() {
    OsdConfig c;
    c.enabled = true;
    c.tmpl = "ABCD";          // no strftime code: four glyphs, stable in time
    c.image_dir = "tests";
    return c;
}

const Json* at(const Json& o, const char* k) { return o.get(k); }

long long num(const Json& arr, size_t i) {
    return arr.at(i).is_number() ? arr.at(i).as_int() : -12345;
}

} // namespace

void run_osd_tests() {
    // ---- length specs: pixels, percent, em, and fail-closed ----------------
    {
        int out = -1;
        LCHECK(resolve_length("12", 1920, 20, out) && out == 12);
        LCHECK(resolve_length("-8", 1920, 20, out) && out == -8);
        LCHECK(resolve_length("2%", 1920, 20, out) && out == 38);        // 2% of 1920
        LCHECK(resolve_length("1.5em", 1920, 20, out) && out == 30);
        LCHECK(resolve_length(" 4 ", 1920, 20, out) && out == 4);        // surrounding space is not an error
        // Anything else is refused rather than silently read as zero: a
        // half-parsed offset looks like a setting that works and does not.
        LCHECK(!resolve_length("", 1920, 20, out));
        LCHECK(!resolve_length("12px", 1920, 20, out));
        LCHECK(!resolve_length("2 %", 1920, 20, out));
        LCHECK(!resolve_length("abc", 1920, 20, out));
        LCHECK(!resolve_length("1.5emx", 1920, 20, out));
        LCHECK(!resolve_length("%", 1920, 20, out));
    }

    // ---- placement ---------------------------------------------------------
    {
        const int fw = 1000, fh = 500, tw = 100, th = 50;
        OsdRect r = place(OsdAnchor::TopLeft, 10, 20, 0, 0, tw, th, fw, fh);
        LCHECK(r.x == 10 && r.y == 20 && r.w == tw && r.h == th);

        r = place(OsdAnchor::BottomRight, 10, 20, 0, 0, tw, th, fw, fh);
        LCHECK(r.x == 1000 - 100 - 10 && r.y == 500 - 50 - 20);

        r = place(OsdAnchor::Center, 999, 999, 0, 0, tw, th, fw, fh);
        // center ignores the offsets, exactly as the stock page hides them
        LCHECK(r.x == (fw - tw) / 2 && r.y == (fh - th) / 2);

        r = place(OsdAnchor::Top, 999, 20, 0, 0, tw, th, fw, fh);
        LCHECK(r.x == (fw - tw) / 2 && r.y == 20);     // offsetX not offered for `top`

        r = place(OsdAnchor::Left, 10, 999, 0, 0, tw, th, fw, fh);
        LCHECK(r.x == 10 && r.y == (fh - th) / 2);     // offsetY not offered for `left`

        // proportional grid: 16 is the left/top edge, -16 the right/bottom
        r = place(OsdAnchor::Proportional, 0, 0, 16, 16, tw, th, fw, fh);
        LCHECK(r.x == 0 && r.y == 0);
        r = place(OsdAnchor::Proportional, 0, 0, -16, -16, tw, th, fw, fh);
        LCHECK(r.x == fw - tw && r.y == fh - th);
        r = place(OsdAnchor::Proportional, 0, 0, 0, 0, tw, th, fw, fh);
        LCHECK(r.x == (fw - tw) / 2 && r.y == (fh - th) / 2);
        // out-of-grid values are clamped, not wrapped
        r = place(OsdAnchor::Proportional, 0, 0, 999, -999, tw, th, fw, fh);
        LCHECK(r.x == 0 && r.y == fh - th);

        // a rectangle never leaves the frame: the settings page counts bytes
        // from it and hardware cannot take one that hangs over the edge
        r = place(OsdAnchor::TopLeft, 100000, 100000, 0, 0, tw, th, fw, fh);
        LCHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= fw && r.y + r.h <= fh);
        r = place(OsdAnchor::TopLeft, -100000, -100000, 0, 0, tw, th, fw, fh);
        LCHECK(r.x == 0 && r.y == 0);
        // text wider than the frame is reported as the frame, not as overflow
        r = place(OsdAnchor::TopLeft, 0, 0, 0, 0, 5000, 5000, fw, fh);
        LCHECK(r.w == fw && r.h == fh && r.x == 0 && r.y == 0);
    }

    // ---- template expansion ------------------------------------------------
    {
        // 2021-01-02 03:04:05 UTC; only the fixed parts are asserted so the
        // test does not depend on the host time zone.
        const int64_t t = 1609556645;
        LCHECK(expand_template("plain", t, 0) == "plain");
        LCHECK(expand_template("%f", t, 7) == "007");
        LCHECK(expand_template("a%fb", t, 999) == "a999b");
        LCHECK(expand_template("%f", t, 5000) == "999");         // clamped, never 5 digits
        LCHECK(expand_template("%f", t, -3) == "000");
        // lens zoom: this camera has no motorised lens, so it expands to
        // nothing rather than to an invented magnification
        LCHECK(expand_template("x%@y", t, 0) == "xy");
        LCHECK(expand_template("100%%", t, 0) == "100%");
        const std::string y = expand_template("%Y", t, 0);
        LCHECK(y == "2021" || y == "2020");                       // time zone may pull it back
        LCHECK(expand_template("", t, 0).empty());
        // a trailing bare % is not a specifier and must not read past the end
        LCHECK(expand_template("abc%", t, 0) == "abc%");
    }

    // ---- report: the /api/v1/osd document ----------------------------------
    {
        TestBackend be;
        OsdService s(be, "tests");
        s.set_config(fixed_text_config());
        s.set_streams(two_streams());

        Json doc;
        LCHECK(s.report(doc));
        const Json* group = at(doc, "group");
        LCHECK(group && group->is_array() && group->size() == 2);
        LCHECK(group && num(*group, 0) == 1920 && num(*group, 1) == 1080);

        const Json* streams = at(doc, "streams");
        LCHECK(streams && streams->is_array() && streams->size() == 2);
        if (streams && streams->size() == 2) {
            const Json& s0 = streams->at(0);
            LCHECK(s0.get("stream") && s0.get("stream")->as_int() == 0);
            LCHECK(s0.get("frame") && num(*s0.get("frame"), 0) == 1920);
            // no cropping: every stream views the whole group
            LCHECK(s0.get("view") && num(*s0.get("view"), 2) == 1920 && num(*s0.get("view"), 3) == 1080);
            const Json& s1 = streams->at(1);
            LCHECK(s1.get("frame") && num(*s1.get("frame"), 0) == 640);
            LCHECK(s1.get("view") && num(*s1.get("view"), 2) == 1920);
        }

        const Json* overlays = at(doc, "overlays");
        LCHECK(overlays && overlays->is_array() && overlays->size() == 2);
        if (overlays && overlays->size()) {
            const Json& o = overlays->at(0);
            LCHECK(o.get("overlay") && o.get("overlay")->as_int() == 0);
            LCHECK(o.get("rect") && o.get("rect")->size() == 4);
            // "ABCD" at scale 1.0 measures 40x10 in the test backend
            LCHECK(o.get("rect") && num(*o.get("rect"), 2) == 40 && num(*o.get("rect"), 3) == 10);
        }
        const Json* budget = at(doc, "budget");
        LCHECK(budget && budget->get("overlays") && budget->get("overlays")->as_int() == 8);

        // the request path must not DRAW; only pump() may
        LCHECK(be.draws == 0 && be.clears == 0);

        // per-stream gating is the only per-stream thing that exists
        s.set_streams(two_streams(true, false));
        Json d2;
        LCHECK(s.report(d2));
        LCHECK(d2.get("overlays") && d2.get("overlays")->size() == 1);
        LCHECK(d2.get("streams") && d2.get("streams")->size() == 2);   // still reported, just not drawn on

        // disabled OSD reports no overlays but still describes the geometry,
        // which is what the preview overlays need
        OsdConfig off = fixed_text_config();
        off.enabled = false;
        s.set_config(off);
        s.set_streams(two_streams());
        Json d3;
        LCHECK(s.report(d3));
        LCHECK(d3.get("overlays") && d3.get("overlays")->size() == 0);
        LCHECK(d3.get("group") != nullptr);
    }

    // ---- a build that cannot draw says so once (404), rather than empty 200 -
    {
        TestBackend be;
        be.avail = false;
        OsdService s(be, "tests");
        s.set_config(fixed_text_config());
        s.set_streams(two_streams());
        Json doc;
        LCHECK(!s.report(doc));

        // no geometry yet is also "cannot say": an empty group would be a lie
        TestBackend be2;
        OsdService s2(be2, "tests");
        s2.set_config(fixed_text_config());
        Json d2;
        LCHECK(!s2.report(d2));
    }

    // ---- invalid values fail closed ----------------------------------------
    {
        TestBackend be;
        OsdService s(be, "tests");
        s.set_streams(two_streams());

        OsdConfig bad = fixed_text_config();
        bad.size = "huge";                      // not a number
        s.set_config(bad);
        Json doc;
        LCHECK(s.report(doc));
        LCHECK(doc.get("overlays") && doc.get("overlays")->size() == 0);
        s.pump(1609556645, 0);
        LCHECK(be.draws == 0);

        bad.size = "0";                         // a zero scale is not a size
        s.set_config(bad);
        s.pump(1609556645, 0);
        LCHECK(be.draws == 0);

        OsdConfig bad2 = fixed_text_config();
        bad2.offset_x = "nonsense";
        s.set_config(bad2);
        s.pump(1609556645, 0);
        LCHECK(be.draws == 0);                  // an unresolvable offset draws nothing

        // ... and a good config after a bad one still draws
        s.set_config(fixed_text_config());
        s.pump(1609556645, 0);
        LCHECK(be.draws == 2);                  // one per stream
    }

    // ---- pump: redraw only on a real change --------------------------------
    {
        TestBackend be;
        OsdService s(be, "tests");
        s.set_config(fixed_text_config());
        s.set_streams(two_streams());

        s.pump(1609556645, 0);
        LCHECK(be.draws == 2);
        s.pump(1609556645, 0);
        LCHECK(be.draws == 2);                  // same text, same rects: no redraw
        s.pump(1609556645, 500);
        LCHECK(be.draws == 2);                  // %f is not in this template either

        OsdConfig c = fixed_text_config();
        c.tmpl = "ABCDE";
        s.set_config(c);
        s.pump(1609556645, 0);
        LCHECK(be.draws == 4);                  // wider text, new rects

        // switching off clears what was drawn instead of leaving it burnt in
        c.enabled = false;
        s.set_config(c);
        s.pump(1609556645, 0);
        LCHECK(be.clears == 2);
        const int after = be.clears;
        s.pump(1609556645, 0);
        LCHECK(be.clears == after);             // and does not clear again forever
    }

    // ---- /api/v1/osd/image -------------------------------------------------
    {
        TestBackend be;
        OsdService s(be, "tests");
        s.set_config(fixed_text_config());

        const int w = 4, h = 3;
        std::vector<uint8_t> px((size_t)w * h * 4);
        for (size_t i = 0; i < px.size(); ++i) px[i] = (uint8_t)(i & 0xff);

        // valid upload, read back with its declared size and reference width
        OsdService::ImageResult r = s.store_image(0, w, h, 1920, px.data(), px.size());
        LCHECK(r.ok() && r.status == 200);
        ImageInfo info;
        std::string back;
        LCHECK(s.load_image(0, info, back));
        LCHECK(info.w == w && info.h == h && info.ref == 1920);
        LCHECK(back.size() == px.size());
        LCHECK(back.size() == px.size() && memcmp(back.data(), px.data(), px.size()) == 0);

        // a missing ref means "drawn pixel for pixel", which is its own width
        LCHECK(s.store_image(1, w, h, 0, px.data(), px.size()).ok());
        ImageInfo i1;
        std::string b1;
        LCHECK(s.load_image(1, i1, b1) && i1.ref == w);

        // refusals, all fail-closed and none of them writing a file
        LCHECK(s.store_image(-1, w, h, 0, px.data(), px.size()).status == 400);
        LCHECK(s.store_image(99, w, h, 0, px.data(), px.size()).status == 400);
        LCHECK(s.store_image(2, 0, h, 0, px.data(), px.size()).status == 400);
        LCHECK(s.store_image(2, w, -1, 0, px.data(), px.size()).status == 400);
        LCHECK(s.store_image(2, 4096, 4096, 0, px.data(), px.size()).status == 400);  // beyond the dimension cap
        LCHECK(s.store_image(2, w, h, 0, px.data(), px.size() - 1).status == 400);    // short body
        LCHECK(s.store_image(2, w, h, 0, px.data(), px.size() + 1).status == 400);    // long body
        LCHECK(s.store_image(2, w, h, 0, nullptr, px.size()).status == 400);
        LCHECK(s.store_image(2, w, h, -5, px.data(), px.size()).status == 400);
        ImageInfo none;
        std::string nb;
        LCHECK(!s.load_image(2, none, nb));      // nothing was written by any refusal

        // the largest picture that is allowed is allowed, and one pixel more
        // in either direction is not
        LCHECK(OsdService::MAX_IMAGE_DIM == 256);   // lowered in review: see the header
        LCHECK((size_t)OsdService::MAX_IMAGE_DIM * OsdService::MAX_IMAGE_DIM * 4 == OsdService::MAX_IMAGE_BYTES);
        LCHECK(s.store_image(2, OsdService::MAX_IMAGE_DIM + 1, 1, 0, px.data(), 4).status == 400);

        // delete, and deleting again is still success: absent is the goal
        LCHECK(s.delete_image(0).ok());
        LCHECK(!s.load_image(0, info, back));
        LCHECK(s.delete_image(0).ok());
        LCHECK(s.delete_image(-1).status == 400);
        LCHECK(s.delete_image(1).ok());

        // a corrupted file on disk is input too and must be refused, not
        // turned into an allocation
        const std::string path = "tests/overlay0.bgra";
        FILE* f = fopen(path.c_str(), "wb");
        if (f) { fwrite("junkjunkjunkjunkjunk", 1, 20, f); fclose(f); }
        LCHECK(!s.load_image(0, info, back));
        remove(path.c_str());

        // the store never touches the backend
        LCHECK(be.draws == 0 && be.clears == 0);
    }

    // ---- config round trip through the service -----------------------------
    {
        TestBackend be;
        OsdService s(be, "tests");
        OsdConfig c = fixed_text_config();
        c.anchor = OsdAnchor::BottomRight;
        c.offset_x = "2%";
        c.bg_alpha = 70;
        c.thin = true;
        s.set_config(c);
        const OsdConfig got = s.config();
        LCHECK(got.anchor == OsdAnchor::BottomRight);
        LCHECK(got.offset_x == "2%");
        LCHECK(got.bg_alpha == 70);
        LCHECK(got.thin);
        LCHECK(got.tmpl == "ABCD");
    }

    // ---- anchor names round trip (the wire values the stock page sends) ----
    {
        const char* all[] = {"proportional", "top-left", "top", "top-right", "left",
                             "center", "right", "bottom-left", "bottom", "bottom-right"};
        for (const char* n : all) {
            OsdAnchor a = OsdAnchor::Center;
            LCHECK(osd_anchor_parse(n, a));
            LCHECK(std::string(osd_anchor_name(a)) == n);
        }
        OsdAnchor a = OsdAnchor::Center;
        LCHECK(!osd_anchor_parse("middle", a));
        LCHECK(!osd_anchor_parse("", a));
    }

    // ---- config file parsing ----------------------------------------------
    {
        AppConfig cfg;
        std::string err;
        const std::string text =
            "osd.enabled = true\n"
            "osd.template = %H:%M\n"
            "osd.anchor = bottom-right\n"
            "osd.weight = thin\n"
            "osd.offset_x = 2%\n"
            "osd.pos_x = -16\n"
            "osd.bg_alpha = 90\n"
            "video.1.osd = false\n";
        LCHECK(parse_config_text(text, cfg, err));
        LCHECK(cfg.osd.enabled);
        LCHECK(cfg.osd.tmpl == "%H:%M");
        LCHECK(cfg.osd.anchor == OsdAnchor::BottomRight);
        LCHECK(cfg.osd.thin);
        LCHECK(cfg.osd.offset_x == "2%");
        LCHECK(cfg.osd.pos_x == -16);
        LCHECK(cfg.osd.bg_alpha == 90);
        LCHECK(cfg.video.osd);            // default stays on
        LCHECK(!cfg.video1.osd);

        // out-of-range and unknown values are ignored with a warning, never
        // applied: the rest of the file must still load
        AppConfig c2;
        const std::string bad =
            "osd.pos_x = 99\n"
            "osd.bg_alpha = 900\n"
            "osd.anchor = diagonal\n"
            "osd.weight = bold\n"
            "osd.enabled = true\n";
        LCHECK(parse_config_text(bad, c2, err));
        LCHECK(c2.osd.pos_x == 16);                        // default kept
        LCHECK(c2.osd.bg_alpha == 25);
        LCHECK(c2.osd.anchor == OsdAnchor::Proportional);
        LCHECK(!c2.osd.thin);
        LCHECK(c2.osd.enabled);                            // the good line still landed
    }
}
