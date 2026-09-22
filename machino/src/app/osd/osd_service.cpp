#include "app/osd/osd_service.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace machino {

const char* osd_anchor_name(OsdAnchor a) {
    switch (a) {
        case OsdAnchor::Proportional: return "proportional";
        case OsdAnchor::TopLeft:      return "top-left";
        case OsdAnchor::Top:          return "top";
        case OsdAnchor::TopRight:     return "top-right";
        case OsdAnchor::Left:         return "left";
        case OsdAnchor::Center:       return "center";
        case OsdAnchor::Right:        return "right";
        case OsdAnchor::BottomLeft:   return "bottom-left";
        case OsdAnchor::Bottom:       return "bottom";
        case OsdAnchor::BottomRight:  return "bottom-right";
        case OsdAnchor::COUNT:        break;
    }
    return "proportional";
}

bool osd_anchor_parse(const std::string& s, OsdAnchor& out) {
    for (int i = 0; i < (int)OsdAnchor::COUNT; ++i) {
        const OsdAnchor a = (OsdAnchor)i;
        if (s == osd_anchor_name(a)) { out = a; return true; }
    }
    return false;
}

// The declared model, not a font. 32 px em at scale 1.0 on a 1920 px frame,
// scaled with the frame so the text keeps its share of a sub stream; advance
// 0.6 em (a monospace-ish ratio, which is what the default UbuntuMono face
// is), line box 1.25 em. Every number here is a stated assumption, which is
// why available() is false and nothing publishes these as camera facts.
OsdMetrics SoftOsdBackend::measure(const std::string& text, double size, int frame_w) const {
    OsdMetrics m;
    if (frame_w <= 0 || size <= 0) return m;
    const double em = 32.0 * size * ((double)frame_w / 1920.0);
    m.em = (int)std::lround(em);
    if (m.em < 1) m.em = 1;
    // Count characters, not bytes: a UTF-8 continuation byte is not a glyph.
    size_t glyphs = 0;
    for (unsigned char c : text) if ((c & 0xC0) != 0x80) ++glyphs;
    m.w = (int)std::lround(0.6 * em * (double)glyphs);
    m.h = (int)std::lround(1.25 * em);
    return m;
}

namespace osd {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

// strtod plus "the whole token had to be a number". A partially parsed value
// is a typo the operator should see, not a silently halved offset.
bool whole_number(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (!end || *end != '\0') return false;
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

bool resolve_length(const std::string& spec, int extent, int em, int& out) {
    const std::string s = trim(spec);
    if (s.empty()) return false;
    double v = 0;
    if (s.size() > 1 && s.back() == '%') {
        if (!whole_number(s.substr(0, s.size() - 1), v)) return false;
        out = (int)std::lround(v / 100.0 * (double)extent);
        return true;
    }
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "em") == 0) {
        if (!whole_number(s.substr(0, s.size() - 2), v)) return false;
        out = (int)std::lround(v * (double)em);
        return true;
    }
    if (!whole_number(s, v)) return false;
    out = (int)std::lround(v);
    return true;
}

OsdRect place(OsdAnchor anchor, int offx, int offy, int posx, int posy,
              int text_w, int text_h, int frame_w, int frame_h) {
    OsdRect r;
    r.w = text_w;
    r.h = text_h;
    if (frame_w <= 0 || frame_h <= 0) return r;

    const int free_x = frame_w - text_w;
    const int free_y = frame_h - text_h;
    const int mid_x  = free_x / 2;
    const int mid_y  = free_y / 2;

    switch (anchor) {
        case OsdAnchor::Proportional: {
            // majestic's grid: 16 is the left/top edge, -16 the right/bottom.
            const int px = clampi(posx, -16, 16);
            const int py = clampi(posy, -16, 16);
            r.x = (int)std::lround((16.0 - px) / 32.0 * (double)free_x);
            r.y = (int)std::lround((16.0 - py) / 32.0 * (double)free_y);
            break;
        }
        // Where an offset is not offered by the stock page for an anchor (it
        // hides offsetX on top/bottom/center and offsetY on left/right/center)
        // it is not applied here either, so the two agree about the result.
        case OsdAnchor::TopLeft:     r.x = offx;            r.y = offy;            break;
        case OsdAnchor::Top:         r.x = mid_x;           r.y = offy;            break;
        case OsdAnchor::TopRight:    r.x = free_x - offx;   r.y = offy;            break;
        case OsdAnchor::Left:        r.x = offx;            r.y = mid_y;           break;
        case OsdAnchor::Center:      r.x = mid_x;           r.y = mid_y;           break;
        case OsdAnchor::Right:       r.x = free_x - offx;   r.y = mid_y;           break;
        case OsdAnchor::BottomLeft:  r.x = offx;            r.y = free_y - offy;   break;
        case OsdAnchor::Bottom:      r.x = mid_x;           r.y = free_y - offy;   break;
        case OsdAnchor::BottomRight: r.x = free_x - offx;   r.y = free_y - offy;   break;
        case OsdAnchor::COUNT:                                                     break;
    }
    // A reported rectangle has to be a real region: the settings page counts
    // bytes from it, and hardware cannot be handed one that leaves the frame.
    r.x = clampi(r.x, 0, free_x < 0 ? 0 : free_x);
    r.y = clampi(r.y, 0, free_y < 0 ? 0 : free_y);
    if (r.w > frame_w) r.w = frame_w;
    if (r.h > frame_h) r.h = frame_h;
    return r;
}

std::string expand_template(const std::string& tpl, int64_t unix_s, int ms) {
    // Substitute the non-strftime specifiers first, then hand the rest to
    // strftime once. Doing it the other way round would feed strftime a code
    // it does not know, which is implementation-defined.
    std::string pre;
    pre.reserve(tpl.size() + 8);
    for (size_t i = 0; i < tpl.size(); ++i) {
        if (tpl[i] != '%') { pre += tpl[i]; continue; }
        if (i + 1 >= tpl.size()) {
            // A lone trailing % is not a specifier, and strftime is entitled
            // to swallow it. Escaped here so it comes out as itself.
            pre += "%%";
            continue;
        }
        const char c = tpl[i + 1];
        if (c == 'f') {
            char b[8];
            std::snprintf(b, sizeof b, "%03d", clampi(ms, 0, 999));
            pre += b;
            ++i;
        } else if (c == '@') {
            // Lens zoom magnification. This camera has no motorised lens to
            // ask, and a made-up magnification would be worse than none.
            ++i;
        } else {
            pre += tpl[i];
            pre += c;
            ++i;
        }
    }

    const time_t t = (time_t)unix_s;
    struct tm tm_buf;
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char out[512];
    const size_t n = strftime(out, sizeof out, pre.c_str(), &tm_buf);
    // strftime returns 0 both for "did not fit" and for "expanded to nothing".
    // An empty template really is empty; a template that overflowed is not
    // something to print half of, so both end up as no text and no region.
    return n ? std::string(out, n) : std::string();
}

OsdService::OsdService(IOsdBackend& backend, std::string image_dir)
    : backend_(backend), dir_(std::move(image_dir)) {}

void OsdService::set_config(const OsdConfig& cfg) {
    std::lock_guard<std::mutex> lk(m_);
    cfg_ = cfg;
    if (!cfg_.image_dir.empty()) dir_ = cfg_.image_dir;
    // Invalidate the DEDUPE key so the next pump redraws - but keep drew_ and
    // last_units_, because they are what a later switch-off needs in order to
    // clear the overlay it burnt in. Resetting them here left the old text on
    // the video forever when OSD was turned off.
    last_text_.clear();
}

OsdConfig OsdService::config() const {
    std::lock_guard<std::mutex> lk(m_);
    return cfg_;
}

void OsdService::set_streams(const std::vector<StreamGeometry>& s) {
    std::lock_guard<std::mutex> lk(m_);
    streams_ = s;
    last_text_.clear();
}

OsdService::Plan OsdService::build_plan_locked(int64_t unix_s, int ms) const {
    Plan p;
    double scale = 1.0;
    if (!whole_number(trim(cfg_.size), scale) || scale <= 0 || scale > 16) {
        // An unparseable font size draws nothing rather than falling back to
        // 1.0: the operator asked for something specific and did not get it.
        return p;
    }
    if (!cfg_.enabled) return p;

    p.text = expand_template(cfg_.tmpl, unix_s, ms);
    if (p.text.empty()) return p;

    for (const StreamGeometry& s : streams_) {
        if (!s.osd_on || s.w <= 0 || s.h <= 0) continue;
        const OsdMetrics m = backend_.measure(p.text, scale, s.w);
        if (m.w <= 0 || m.h <= 0) continue;

        int offx = 0, offy = 0;
        if (!resolve_length(cfg_.offset_x, s.w, m.em, offx)) continue;
        if (!resolve_length(cfg_.offset_y, s.h, m.em, offy)) continue;

        OsdDraw d;
        d.overlay  = 0;
        d.unit     = s.unit;
        d.frame_w  = s.w;
        d.frame_h  = s.h;
        d.rect     = place(cfg_.anchor, offx, offy, cfg_.pos_x, cfg_.pos_y, m.w, m.h, s.w, s.h);
        d.text     = p.text;
        d.size     = scale;
        d.thin     = cfg_.thin;
        d.outline  = cfg_.outline;
        d.bg_alpha = clampi(cfg_.bg_alpha, 0, 100);
        p.items.push_back(d);
    }
    p.draw = !p.items.empty();
    return p;
}

bool OsdService::report(Json& out) const {
    // A build that cannot draw must say so once rather than answer with an
    // empty document: the stock page reads 404 as a property of the build and
    // stops polling, but an empty 200 would have it repaint forever.
    if (!backend_.available()) return false;

    std::lock_guard<std::mutex> lk(m_);
    if (streams_.empty()) return false;

    // The group is the largest output, which on this camera is the main
    // stream: it is the frame the sub stream is a scaled copy of.
    int gw = 0, gh = 0;
    for (const StreamGeometry& s : streams_)
        if ((long long)s.w * s.h > (long long)gw * gh) { gw = s.w; gh = s.h; }
    if (gw <= 0 || gh <= 0) return false;

    out = Json::object();
    Json group = Json::array();
    group.push(Json::number(gw));
    group.push(Json::number(gh));
    out.set("group", group);

    Json streams = Json::array();
    for (const StreamGeometry& s : streams_) {
        if (s.w <= 0 || s.h <= 0) continue;
        Json e = Json::object();
        e.set("stream", Json::number(s.unit));
        Json f = Json::array(); f.push(Json::number(s.w)); f.push(Json::number(s.h));
        e.set("frame", f);
        // Machino never crops a stream out of the group: both outputs show the
        // whole scene, the sub stream just smaller. So the view is the group.
        Json v = Json::array();
        v.push(Json::number(0)); v.push(Json::number(0));
        v.push(Json::number(gw)); v.push(Json::number(gh));
        e.set("view", v);
        streams.push(e);
    }
    out.set("streams", streams);

    Json overlays = Json::array();
    const Plan p = build_plan_locked((int64_t)::time(nullptr), 0);
    for (const OsdDraw& d : p.items) {
        Json o = Json::object();
        o.set("overlay", Json::number(d.overlay));
        Json f = Json::array(); f.push(Json::number(d.frame_w)); f.push(Json::number(d.frame_h));
        o.set("frame", f);
        Json r = Json::array();
        r.push(Json::number(d.rect.x)); r.push(Json::number(d.rect.y));
        r.push(Json::number(d.rect.w)); r.push(Json::number(d.rect.h));
        o.set("rect", r);
        overlays.push(o);
    }
    out.set("overlays", overlays);

    if (const int budget = backend_.overlay_budget(); budget > 0) {
        Json b = Json::object();
        b.set("overlays", Json::number(budget));
        out.set("budget", b);
    }
    return true;
}

void OsdService::pump(int64_t unix_s, int ms) {
    // Build under the lock, draw outside it. The backend can take as long as
    // it likes without any other thread waiting on this service.
    Plan p;
    bool was_drawing = false;
    std::vector<int> previous_units;
    {
        std::lock_guard<std::mutex> lk(m_);
        p = build_plan_locked(unix_s, ms);
        was_drawing = drew_;
        previous_units = last_units_;
        std::vector<OsdRect> now;
        std::vector<int> units;
        now.reserve(p.items.size());
        units.reserve(p.items.size());
        for (const OsdDraw& d : p.items) { now.push_back(d.rect); units.push_back(d.unit); }
        // Nothing changed this tick: the same second, the same geometry. The
        // overlay is redrawn once a second at most, never once a frame.
        if (p.draw && drew_ && p.text == last_text_ && now == last_rects_) return;
        last_text_ = p.text;
        last_rects_ = now;
        drew_ = p.draw;
        if (p.draw) last_units_ = units;
        else last_units_.clear();
    }

    if (!p.draw) {
        // Clear the units that were actually drawn on, not 0..n-1: a build
        // where only the sub stream carries the overlay would otherwise clear
        // the main stream and leave the sub stream burnt in.
        if (was_drawing)
            for (int unit : previous_units) backend_.clear(0, unit);
        return;
    }
    for (const OsdDraw& d : p.items) backend_.draw(d);
}

// --- image store ---------------------------------------------------------
//
// One file per overlay under one directory that the config names; the caller
// supplies only an index, so no request can steer the path. The stored form is
// a tiny header (magic, w, h, ref) followed by the BGRA payload, because the
// read-back has to answer with X-Osd-Width/Height/Ref and a bare pixel dump
// cannot say what shape it is.

namespace {
const char  IMG_MAGIC[4] = {'M', 'O', 'S', 'D'};
const size_t IMG_HDR = 16;   // magic + three 32-bit LE fields
void put32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);       p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff); p[3] = (unsigned char)((v >> 24) & 0xff);
}
uint32_t get32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
} // namespace

std::string OsdService::image_path(int overlay) const {
    char name[32];
    std::snprintf(name, sizeof name, "overlay%d.bgra", overlay);
    std::string d = dir_;
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d + name;
}

OsdService::ImageResult OsdService::store_image(int overlay, int w, int h, int ref,
                                                const uint8_t* bgra, size_t len) {
    if (overlay < 0 || overlay > MAX_OVERLAY_INDEX)
        return {400, "overlay index out of range"};
    if (w <= 0 || h <= 0 || w > MAX_IMAGE_DIM || h > MAX_IMAGE_DIM)
        return {400, "picture must be between 1x1 and 512x512"};
    // Multiply in 64 bits: w and h are already bounded above, but the check
    // must not be the thing that overflows.
    const uint64_t need = (uint64_t)w * (uint64_t)h * 4ull;
    if (need > (uint64_t)MAX_IMAGE_BYTES)
        return {413, "picture is too large"};
    if (!bgra || len != (size_t)need)
        return {400, "pixel data does not match the declared size"};
    if (ref < 0) return {400, "reference width must not be negative"};

    std::lock_guard<std::mutex> lk(m_);
    const std::string final_path = image_path(overlay);
    const std::string tmp_path = final_path + ".tmp";

    // Write to a temporary and rename, so a half-written upload can never be
    // the file the camera loads on its next start.
    FILE* f = fopen(tmp_path.c_str(), "wb");
    if (!f) return {500, "cannot write to the overlay store"};
    unsigned char hdr[IMG_HDR];
    std::memcpy(hdr, IMG_MAGIC, 4);
    put32(hdr + 4, (uint32_t)w);
    put32(hdr + 8, (uint32_t)h);
    put32(hdr + 12, (uint32_t)(ref > 0 ? ref : w));
    const bool wrote = fwrite(hdr, 1, IMG_HDR, f) == IMG_HDR &&
                       fwrite(bgra, 1, len, f) == len;
    const bool closed = fclose(f) == 0;
    if (!wrote || !closed) { remove(tmp_path.c_str()); return {500, "the overlay store is full"}; }

    remove(final_path.c_str());           // rename() will not replace on every libc
    if (rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        remove(tmp_path.c_str());
        return {500, "cannot commit the picture"};
    }
    return {200, ""};
}

OsdService::ImageResult OsdService::delete_image(int overlay) {
    if (overlay < 0 || overlay > MAX_OVERLAY_INDEX)
        return {400, "overlay index out of range"};
    std::lock_guard<std::mutex> lk(m_);
    remove(image_path(overlay).c_str());   // absent is the desired end state
    return {200, ""};
}

bool OsdService::load_image(int overlay, ImageInfo& info, std::string& bgra) const {
    if (overlay < 0 || overlay > MAX_OVERLAY_INDEX) return false;
    std::lock_guard<std::mutex> lk(m_);
    FILE* f = fopen(image_path(overlay).c_str(), "rb");
    if (!f) return false;
    unsigned char hdr[IMG_HDR];
    if (fread(hdr, 1, IMG_HDR, f) != IMG_HDR || std::memcmp(hdr, IMG_MAGIC, 4) != 0) {
        fclose(f);
        return false;
    }
    const uint32_t w = get32(hdr + 4), h = get32(hdr + 8), ref = get32(hdr + 12);
    // Re-validate on the way out: a file on disk is input too, and a corrupted
    // header must not become a multi-gigabyte allocation.
    if (w == 0 || h == 0 || w > (uint32_t)MAX_IMAGE_DIM || h > (uint32_t)MAX_IMAGE_DIM) {
        fclose(f);
        return false;
    }
    const size_t need = (size_t)w * (size_t)h * 4u;
    bgra.resize(need);
    const size_t got = fread(&bgra[0], 1, need, f);
    fclose(f);
    if (got != need) { bgra.clear(); return false; }
    info.w = (int)w;
    info.h = (int)h;
    info.ref = (int)(ref ? ref : w);
    return true;
}

}} // namespace machino::osd
