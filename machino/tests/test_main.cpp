// Machino host unit tests (no vendor SDK, no hardware). Plain asserts, one
// binary, exit code = number of failures.
#include "adapters/ingenic/sensor_params.hpp"
#include "core/capabilities.hpp"
#include "core/config.hpp"
#include "core/hw/board_profile_parser.hpp"
#include "core/hw/registry.hpp"
#include "core/hw/resolve.hpp"
#include "profiles/builtin_profiles.hpp"

#include <cstdio>
#include <string>

using namespace machino;

static int g_fail = 0, g_pass = 0;
int g_fail_ext = 0, g_pass_ext = 0;     // shared with test_lifecycle.cpp
void run_lifecycle_tests();
void run_power_tests();
void run_json_tests();
void run_event_tests();
void run_http_parse_tests();
void run_session_tests();
void run_fmp4_tests();
void run_sps_tests();
void run_webrtc_tests();
void run_dtls_tests();
void run_rtsp_auth_tests();
void run_rtsp_claim_tests();
void run_websocket_tests();
void run_api_tests();
void run_tuning_tests();
void run_multistream_tests();
void run_detection_tests();
void run_compat_tests();
void run_schema_contract_tests();
void run_logging_tests();
void run_osd_tests();
void run_setup_tests();
void run_onvif_tests();
void run_discovery_tests();
void run_onvif_digest_tests();
void run_random_tests();
#define CHECK(cond) do { if (cond) { ++g_pass; } else { ++g_fail; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static hw::Registry make_registry() { hw::Registry r; profiles::register_builtin(r); return r; }

static const char* PROFILE_TEXT =
    "# board A\n"
    "board_id   = test-board\n"
    "platform   = ingenic-t40nn\n"
    "sensor     = imx307\n"
    "i2c_bus    = 1\n"
    "i2c_addr   = 0x1a\n"
    "mclk       = 1\n"
    "reset_gpio = 91\n"
    "pwdn_gpio  = 0\n"
    "mode       = 1920x1080@20\n"
    "hardware_verified = yes\n"
    "notes      = unit test\n";

// ---- BoardProfile parsing --------------------------------------------------
static void test_profile_parsing() {
    hw::BoardProfile p; std::string err, warn;
    CHECK(hw::parse_board_profile(PROFILE_TEXT, p, err, &warn));
    CHECK(err.empty() && warn.empty());
    CHECK(p.board_id == "test-board" && p.platform == "ingenic-t40nn" && p.sensor == "imx307");
    CHECK(p.wiring.i2c_bus && *p.wiring.i2c_bus == 1);
    CHECK(p.wiring.i2c_addr && *p.wiring.i2c_addr == 0x1a);
    CHECK(p.wiring.mclk && *p.wiring.mclk == 1);
    CHECK(p.wiring.reset_gpio && *p.wiring.reset_gpio == 91);
    CHECK(p.wiring.pwdn_gpio && *p.wiring.pwdn_gpio == 0);
    CHECK(p.default_mode && p.default_mode->width == 1920 && p.default_mode->fps == 20);
    CHECK(p.hardware_verified);

    // "none" and missing pins
    hw::BoardProfile q;
    CHECK(hw::parse_board_profile("board_id=b\nplatform=ingenic-t40nn\nsensor=imx307\nreset_gpio=none\n", q, err));
    CHECK(q.wiring.reset_gpio && *q.wiring.reset_gpio == -1);
    CHECK(!q.wiring.pwdn_gpio.has_value());
    // required keys
    CHECK(!hw::parse_board_profile("platform=x\nsensor=y\n", q, err) && err.find("board_id") != std::string::npos);
    // bad values are errors, unknown keys are warnings
    CHECK(!hw::parse_board_profile("board_id=b\nplatform=p\nsensor=s\nreset_gpio=abc\n", q, err));
    CHECK(!hw::parse_board_profile("board_id=b\nplatform=p\nsensor=s\nmode=1920x1080\n", q, err));
    warn.clear();
    CHECK(hw::parse_board_profile("board_id=b\nplatform=p\nsensor=s\nfoo=1\n", q, err, &warn) && warn.find("foo") != std::string::npos);
    hw::SensorMode m;
    CHECK(hw::parse_mode("1280x720@15", m) && m.width == 1280 && m.height == 720 && m.fps == 15);
    CHECK(!hw::parse_mode("1280x720", m));
}

// ---- multiple sensor modes ------------------------------------------------
static void test_sensor_modes() {
    hw::SensorDescriptor s; s.model = "x"; s.interface = hw::SensorInterface::MipiCsi;
    s.modes = { hw::SensorMode{1920, 1080, 20}, hw::SensorMode{1280, 720, 30} };
    CHECK(s.has_mode(hw::SensorMode{1280, 720, 30}));
    CHECK(!s.has_mode(hw::SensorMode{1280, 720, 60}));
    CHECK(s.default_mode() && s.default_mode()->width == 1920);
    hw::Registry r; r.add_platform({"v", "f", "m"}); r.add_sensor(s);
    hw::BoardProfile b; b.board_id = "bb"; b.platform = "v-m"; b.sensor = "x";
    b.wiring.i2c_bus = 0; b.wiring.i2c_addr = 0x10; b.wiring.mclk = 0;
    r.add_board(b);
    hw::UserHardwareConfig u; u.board_id = "bb"; u.mode = hw::SensorMode{1280, 720, 30};
    hw::ResolvedHardware hw; std::string err;
    CHECK(hw::resolve_hardware(u, r, {}, hw, err));
    CHECK(hw.mode.value.width == 1280 && hw.mode.source == hw::Source::UserConfig);
    u.mode = hw::SensorMode{640, 480, 30};
    CHECK(!hw::resolve_hardware(u, r, {}, hw, err) && err.find("not a verified mode") != std::string::npos);
    u.mode.reset();
    CHECK(hw::resolve_hardware(u, r, {}, hw, err) && hw.mode.value.width == 1920 && hw.mode.source == hw::Source::PlatformDefault);
}

// ---- precedence -------------------------------------------------------------
static void test_precedence() {
    hw::Registry r = make_registry();
    hw::UserHardwareConfig u; u.board_id = "t40nn-imx307-board-a";
    hw::ResolvedHardware hw; std::string err;

    // board only
    CHECK(hw::resolve_hardware(u, r, {}, hw, err));
    CHECK(hw.reset_gpio.value == 91 && hw.reset_gpio.source == hw::Source::BoardProfile);
    CHECK(hw.pwdn_gpio.value == 0 && hw.pwdn_gpio.source == hw::Source::BoardProfile);
    CHECK(hw.i2c_bus.value == 1 && hw.i2c_addr.value == 0x1a && hw.mclk.value == 1);
    CHECK((hw.mode.value == hw::SensorMode{1920, 1080, 20}) && hw.mode.source == hw::Source::BoardProfile);
    CHECK(hw.board_verified && hw.conflicts.empty());
    CHECK(hw.platform.vendor == "ingenic" && hw.platform.model == "t40nn");

    // board says 91, user says 92 -> effective 92, conflict logged
    u.wiring.reset_gpio = 92;
    CHECK(hw::resolve_hardware(u, r, {}, hw, err));
    CHECK(hw.reset_gpio.value == 92 && hw.reset_gpio.source == hw::Source::UserConfig);
    CHECK(hw.conflicts.find("reset_gpio=92 [user-config] overrides 91 [board-profile]") != std::string::npos);

    // user explicitly says "none" -> -1 wins over the board's 91
    u.wiring.reset_gpio = -1;
    CHECK(hw::resolve_hardware(u, r, {}, hw, err) && hw.reset_gpio.value == -1 && hw.reset_gpio.source == hw::Source::UserConfig);

    // user overrides the sensor mode with a verified one
    u.wiring.reset_gpio.reset(); u.mode = hw::SensorMode{1920, 1080, 20};
    CHECK(hw::resolve_hardware(u, r, {}, hw, err) && hw.mode.source == hw::Source::UserConfig);

    // platform default applies only when neither user nor board set mclk
    hw::Registry r2; r2.add_platform({"v", "f", "m"});
    hw::SensorDescriptor s; s.model = "s"; s.interface = hw::SensorInterface::MipiCsi; s.modes = { {640, 480, 30} }; r2.add_sensor(s);
    hw::UserHardwareConfig u2; u2.platform = "v-m"; u2.sensor = "s"; u2.wiring.i2c_bus = 0; u2.wiring.i2c_addr = 0x36;
    hw::PlatformDefaults d; d.mclk = 0;
    CHECK(hw::resolve_hardware(u2, r2, d, hw, err) && hw.mclk.value == 0 && hw.mclk.source == hw::Source::PlatformDefault);
    CHECK(!hw::resolve_hardware(u2, r2, {}, hw, err) && err.find("mclk") != std::string::npos);
}

// ---- unknown / missing GPIO -> no GPIO access ------------------------------
static void test_missing_gpio() {
    hw::Registry r = make_registry();
    hw::UserHardwareConfig u;                      // no board
    u.platform = "ingenic-t40nn"; u.sensor = "imx307";
    u.wiring.i2c_bus = 1; u.wiring.i2c_addr = 0x1a; u.wiring.mclk = 1;
    hw::ResolvedHardware hw; std::string err;
    CHECK(hw::resolve_hardware(u, r, {}, hw, err));
    CHECK(hw.reset_gpio.value == -1 && hw.reset_gpio.source == hw::Source::None);
    CHECK(hw.pwdn_gpio.value == -1 && hw.pwdn_gpio.source == hw::Source::None);
    CHECK(hw.board_id.empty());
    ingenic::SensorParams p;
    CHECK(ingenic::sensor_params_from(hw, p, err));
    CHECK(p.reset_gpio == -1 && p.pwdn_gpio == -1 && p.power_gpio == -1);   // the driver never touches a pin

    // no bus / no address / no board -> refuse (never guess)
    hw::UserHardwareConfig u3; u3.platform = "ingenic-t40nn"; u3.sensor = "imx307";
    CHECK(!hw::resolve_hardware(u3, r, {}, hw, err) && err.find("i2c_bus") != std::string::npos);
    u3.wiring.i2c_bus = 1;
    CHECK(!hw::resolve_hardware(u3, r, {}, hw, err) && err.find("i2c_addr") != std::string::npos);
    // unknown board / platform / sensor
    hw::UserHardwareConfig u4; u4.board_id = "does-not-exist";
    CHECK(!hw::resolve_hardware(u4, r, {}, hw, err) && err.find("unknown board") != std::string::npos);
    hw::UserHardwareConfig u5; u5.platform = "ingenic-t40nn"; u5.sensor = "nosuch"; u5.wiring.i2c_bus = 0; u5.wiring.i2c_addr = 1; u5.wiring.mclk = 0;
    CHECK(!hw::resolve_hardware(u5, r, {}, hw, err) && err.find("unknown sensor") != std::string::npos);
}

// ---- a board profile only describes its own board --------------------------
static void test_board_profile_scope() {
    hw::Registry r = make_registry();
    hw::SensorDescriptor other;                        // a second sensor to point the config at
    other.model = "imx335"; other.interface = hw::SensorInterface::MipiCsi;
    other.native_width = 1920; other.native_height = 1080; other.modes = { {1920, 1080, 20} };
    r.add_sensor(other);
    hw::ResolvedHardware hw; std::string err;

    // board alone: wiring, presets and the verified flag all come from it
    hw::UserHardwareConfig u; u.board_id = "t40nn-imx307-board-a";
    CHECK(hw::resolve_hardware(u, r, {}, hw, err));
    CHECK(hw.reset_gpio.value == 91 && hw.board_verified && hw.presets.battery_fps && *hw.presets.battery_fps == 10);

    // same board, different sensor: pin 91 and address 0x1a describe how the
    // imx307 is wired on this board, so they must not carry over. Nothing is
    // left to fall back on -> fail closed instead of guessing.
    hw::UserHardwareConfig v = u; v.sensor = "imx335";
    CHECK(!hw::resolve_hardware(v, r, {}, hw, err) && err.find("i2c_bus") != std::string::npos);

    // with explicit wiring it resolves - without any leftovers from the profile
    v.wiring.i2c_bus = 1; v.wiring.i2c_addr = 0x20; v.wiring.mclk = 1;
    CHECK(hw::resolve_hardware(v, r, {}, hw, err));
    CHECK(hw.reset_gpio.value == -1 && hw.pwdn_gpio.value == -1);        // no pin is ever touched
    CHECK(!hw.presets.battery_fps.has_value());                          // operating points were verified for imx307
    CHECK(!hw.board_verified);                                           // the profile was not verified for this
    CHECK(hw.conflicts.find("no longer describes this hardware") != std::string::npos);

    // overriding the platform drops it just the same
    hw::UserHardwareConfig w = u; w.platform = "ingenic-t40n";
    CHECK(!hw::resolve_hardware(w, r, {}, hw, err) && err.find("i2c_bus") != std::string::npos);

    // overriding with the value the profile already carries changes nothing
    hw::UserHardwareConfig x = u; x.platform = "ingenic-t40nn"; x.sensor = "imx307";
    CHECK(hw::resolve_hardware(x, r, {}, hw, err));
    CHECK(hw.reset_gpio.value == 91 && hw.board_verified && hw.conflicts.empty());
}

// ---- conflicting values ----------------------------------------------------
static void test_conflicts() {
    hw::Registry r = make_registry();
    hw::UserHardwareConfig u; u.board_id = "t40nn-imx307-board-a";
    u.wiring.i2c_bus = 0; u.wiring.mclk = 0;
    hw::ResolvedHardware hw; std::string err;
    CHECK(hw::resolve_hardware(u, r, {}, hw, err));
    CHECK(hw.i2c_bus.value == 0 && hw.i2c_bus.source == hw::Source::UserConfig);
    CHECK(hw.mclk.value == 0 && hw.mclk.source == hw::Source::UserConfig);
    CHECK(hw.conflicts.find("i2c_bus=0 [user-config] overrides 1 [board-profile]") != std::string::npos);
    CHECK(hw.conflicts.find("mclk=0 [user-config] overrides 1 [board-profile]") != std::string::npos);
    CHECK(hw.conflicts.find("reset_gpio") == std::string::npos);     // untouched values are not conflicts
    // same value on both sides is not a conflict
    hw::UserHardwareConfig v; v.board_id = "t40nn-imx307-board-a"; v.wiring.reset_gpio = 91;
    CHECK(hw::resolve_hardware(v, r, {}, hw, err) && hw.conflicts.empty() && hw.reset_gpio.source == hw::Source::UserConfig);
}

// ---- capability representation ---------------------------------------------
static void test_capabilities() {
    CapabilitySet c;
    CHECK(c.video.h264 == Cap::Unknown && c.ai.available == Cap::Unknown && c.video.max_streams == -1);
    CHECK(Cap::Unknown != Cap::Unsupported);
    CHECK(std::string(cap_name(Cap::Unknown)) == "unknown");
    CHECK(std::string(cap_name(Cap::Supported)) == "supported");
    CHECK(std::string(cap_name(Cap::Unsupported)) == "unsupported");
    c.video.h264 = Cap::Supported; c.video.h265 = Cap::Unsupported;
    CHECK(c.video.h264 == Cap::Supported && c.video.h265 == Cap::Unsupported && c.isp.available == Cap::Unknown);
}

// ---- BoardProfile -> Ingenic sensor params --------------------------------
static void test_ingenic_conversion() {
    hw::Registry r = make_registry();
    hw::UserHardwareConfig u; u.board_id = "t40nn-imx307-board-a";
    hw::ResolvedHardware hw; std::string err;
    CHECK(hw::resolve_hardware(u, r, {}, hw, err));
    ingenic::SensorParams p;
    CHECK(ingenic::sensor_params_from(hw, p, err));
    CHECK(p.name == "imx307");
    CHECK(p.i2c_bus == 1 && p.i2c_addr == 0x1a && p.mclk == 1);
    CHECK(p.reset_gpio == 91 && p.pwdn_gpio == 0 && p.power_gpio == -1);
    CHECK(p.mipi);
    // unknown interface cannot be mapped
    hw.sensor.interface = hw::SensorInterface::Unknown;
    CHECK(!ingenic::sensor_params_from(hw, p, err) && err.find("interface") != std::string::npos);
}

// ---- config: user keys become optional values ------------------------------
static void test_config_keys() {
    AppConfig c; std::string err;
    CHECK(parse_config_text("board = t40nn-imx307-board-a\nvideo.bitrate = 2500\nsensor.reset_gpio = none\n", c, err));
    CHECK(c.hardware.board_id == "t40nn-imx307-board-a");
    CHECK(!c.hardware.wiring.i2c_bus.has_value());
    CHECK(c.hardware.wiring.reset_gpio && *c.hardware.wiring.reset_gpio == -1);
    CHECK(c.video.bitrate_kbps == 2500 && !c.video.width.has_value());
    AppConfig d;
    CHECK(parse_config_text("sensor.mode = 1920x1080@20\nsensor.i2c_bus = 1\n", d, err));
    CHECK(d.hardware.mode && d.hardware.mode->fps == 20 && d.hardware.wiring.i2c_bus && *d.hardware.wiring.i2c_bus == 1);
    AppConfig e;
    CHECK(parse_config_text("sensor.fps = 20\n", e, err));      // partial legacy mode is discarded
    CHECK(!e.hardware.mode.has_value());
    AppConfig m7;
    CHECK(parse_config_text("latency.profile=low\nlatency.gop=10\nlatency.queue_depth=1\nimage.anti_flicker=50hz\nimage.white_balance_mode=0\n", m7, err));
    CHECK(m7.latency.profile == media::LatencyProfile::Low && m7.latency.gop && *m7.latency.gop == 10);
    CHECK(m7.latency.consumer_queue_depth && *m7.latency.consumer_queue_depth == 1);
    CHECK(m7.image.anti_flicker && *m7.image.anti_flicker == 50 && m7.image.white_balance_mode && *m7.image.white_balance_mode == 0);
    AppConfig rt;
    CHECK(parse_config_text("rtsp.max_clients = 2\n", rt, err) && rt.rtsp.max_clients == 2);
    // out of range is warned about and ignored, like every other bounded key:
    // one bad line in the config must not keep the daemon from coming up
    CHECK(parse_config_text("rtsp.max_clients = 0\n", rt, err) && rt.rtsp.max_clients == 2);
    CHECK(parse_config_text("rtsp.max_clients = 17\n", rt, err) && rt.rtsp.max_clients == 2);
    CHECK(AppConfig{}.rtsp.max_clients == 4);                    // bounded by default, not unlimited

    // M9 detection keys
    AppConfig aic;
    CHECK(parse_config_text("ai.enabled = true\nai.detector = motion\nai.inference_fps = 8\n", aic, err));
    CHECK(aic.ai.enabled && aic.ai.detector == "motion" && aic.ai.inference_fps == 8);
    CHECK(AppConfig{}.ai.enabled == false && AppConfig{}.ai.detector == "motion" && AppConfig{}.ai.inference_fps == 5);
    // out-of-range cadence is warned and ignored, daemon still comes up
    CHECK(parse_config_text("ai.inference_fps = 0\n", aic, err) && aic.ai.inference_fps == 8);
    CHECK(parse_config_text("ai.inference_fps = 61\n", aic, err) && aic.ai.inference_fps == 8);

    // M11 upgrade-safety: config is additive and forward/backward-compatible.
    // A newer machino.conf (unknown future keys) still loads on an older binary;
    // an older/minimal config fills the rest from defaults; revision is kept.
    AppConfig up;
    CHECK(parse_config_text("config.revision = 7\nboard = t40nn-imx307-board-a\nvideo.bitrate = 2200\n"
                            "ai.super_new_feature = 1\nvideo.9.enabled = true\nbrand.new.section = x\n", up, err));
    CHECK(up.revision == 7);                                   // revision preserved across versions
    CHECK(up.hardware.board_id == "t40nn-imx307-board-a" && up.video.bitrate_kbps == 2200);   // known keys applied
    CHECK(up.ai.enabled == false && up.ai.inference_fps == 5); // unknown future keys ignored, defaults stand
    CHECK(!up.video1.enabled && up.rtsp.max_clients == 4);     // unset keys keep their defaults

    // effective stream: video.* overrides, mode fills the rest
    hw::Registry r = make_registry(); hw::UserHardwareConfig u; u.board_id = "t40nn-imx307-board-a";
    hw::ResolvedHardware hw; CHECK(hw::resolve_hardware(u, r, {}, hw, err));
    StreamConfig v; v.fps = 15;
    EffectiveStream s = effective_stream(v, hw);
    CHECK(s.width == 1920 && s.height == 1080 && s.fps == 15 && s.native_width == 1920);
}

int main() {
    test_profile_parsing();
    test_sensor_modes();
    test_precedence();
    test_missing_gpio();
    test_conflicts();
    test_board_profile_scope();
    test_capabilities();
    test_ingenic_conversion();
    test_config_keys();
    run_lifecycle_tests();
    run_power_tests();
    run_json_tests();
    run_event_tests();
    run_http_parse_tests();
    run_session_tests();
    run_fmp4_tests();
    run_sps_tests();
    run_webrtc_tests();
    run_dtls_tests();
    run_rtsp_auth_tests();
    run_rtsp_claim_tests();
    run_websocket_tests();
    run_api_tests();
    run_tuning_tests();
    run_multistream_tests();
    run_detection_tests();
    run_compat_tests();
    run_schema_contract_tests();
    run_logging_tests();
    run_osd_tests();
    run_setup_tests();
    run_onvif_tests();
    run_discovery_tests();
    run_onvif_digest_tests();
    run_random_tests();
    g_pass += g_pass_ext; g_fail += g_fail_ext;
    fprintf(stderr, "machino unit tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
