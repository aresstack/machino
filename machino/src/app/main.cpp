// Machino entry point: config -> registry -> resolved hardware -> platform
// adapter -> PipelineManager -> PerformanceService -> ApiService/HTTP -> RTSP.
// Single process, event loop on signalfd + lifecycle grace timerfd + optional
// telemetry timerfd (epoll); no polling in the main loop.
//
//   SIGUSR1 = drop the manual hold    SIGUSR2 = take a manual hold
//   SIGHUP  = re-read the configuration and apply performance/stream changes
// The HTTP API (/api/v1) is served by its own bounded poll() thread; API
// access is never media demand.
#include "adapters/ingenic/ingenic_platform.hpp"
#include "app/api/api_service.hpp"
#include "app/compat/majestic_migrate.hpp"
#include "app/http/http_server.hpp"
#include "app/linux_grace_timer.hpp"
#include "app/linux_system_stats.hpp"
#include "app/rtsp/rtsp_server.hpp"
#include "core/capabilities.hpp"
#include "core/config.hpp"
#include "core/config_store.hpp"
#include "core/detection/detection_service.hpp"
#include "core/events.hpp"
#include "core/hw/board_profile_parser.hpp"
#include "core/hw/registry.hpp"
#include "core/hw/resolve.hpp"
#include "core/json.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/log.hpp"
#include "core/media/tuning_service.hpp"
#include "core/power/performance_service.hpp"
#include "core/stream_hub.hpp"
#include "profiles/builtin_profiles.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/prctl.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

using namespace machino;
using lifecycle::ConsumerType;

static const char* MOD = "MAIN";

// Validate credentials against the system account, exactly like Majestic: the
// WebUI login IS the camera's root login (/etc/shadow, crypt(3) - musl carries
// crypt in libc, no extra library).
#include <crypt.h>
static bool shadow_check(const std::string& user, const std::string& pass) {
    FILE* f = fopen("/etc/shadow", "r");
    if (!f) return false;
    char line[512]; bool ok = false;
    while (fgets(line, sizeof line, f)) {
        char* c1 = strchr(line, ':');
        if (!c1) continue;
        *c1 = 0;
        if (user != line) continue;
        char* hash = c1 + 1;
        if (char* c2 = strchr(hash, ':')) *c2 = 0;
        // An EMPTY hash means an UNCLAIMED camera (OpenIPC then forces the
        // /setup flow) - it must NEVER count as "empty password accepted".
        // The setup flow itself is not implemented yet; until it is, an
        // unclaimed camera simply cannot log in over the WebUI.
        if (hash[0] == 0) { ok = false; }
        else if (hash[0] != '!' && hash[0] != '*') {              // '!' / '*' = locked
            const char* enc = crypt(pass.c_str(), hash);
            ok = enc && strcmp(enc, hash) == 0;
        }
        break;
    }
    fclose(f);
    return ok;
}

static void usage(const char* argv0) {
    fprintf(stderr, "usage: %s [-c machino.conf] [-v]\n"
                    "       %s --version\n"
                    "       %s --migrate-majestic <majestic.yaml> [-o machino.conf]\n", argv0, argv0, argv0);
}

// One-way import of an existing OpenIPC majestic.yaml. Prints a full
// classification report to stderr and the resulting machino.conf to the output
// file (or stdout). Never touches the running system.
static int run_migration(int argc, char** argv) {
    const char* in = nullptr; const char* out = nullptr;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (argv[i][0] != '-' && !in) in = argv[i];
        else { usage(argv[0]); return 2; }
    }
    if (!in) { usage(argv[0]); return 2; }
    std::ifstream f(in);
    if (!f) { fprintf(stderr, "migrate: cannot open %s\n", in); return 1; }
    std::stringstream ss; ss << f.rdbuf();
    compat::MigrationResult r = compat::migrate_majestic_yaml(ss.str());
    fputs(compat::migration_report(r).c_str(), stderr);
    if (!r.ok) return 1;
    std::string conf = compat::to_machino_conf(r);
    if (out) {
        std::ofstream o(out);
        if (!o) { fprintf(stderr, "migrate: cannot write %s\n", out); return 1; }
        o << conf;
        fprintf(stderr, "migrate: wrote %s (%d mapped, %d converted, %d unsupported, %d invalid)\n",
                out, r.mapped, r.converted, r.unsupported, r.invalid);
    } else {
        fputs(conf.c_str(), stdout);
    }
    return 0;
}

static std::unique_ptr<IPlatform> make_platform(const hw::ResolvedHardware& hw) {
    if (hw.platform.vendor == "ingenic") return std::make_unique<ingenic::IngenicPlatform>(hw);
    return nullptr;
}

static hw::PlatformDefaults platform_defaults_for(const std::string& vendor) {
    if (vendor == "ingenic") return ingenic::IngenicPlatform::platform_defaults();
    return hw::PlatformDefaults{};
}

static void log_capabilities(const CapabilitySet& c) {
    LOGI(MOD, "capabilities: h264=%s h265=%s isp=%s hw-encoder=%s ai=%s", cap_name(c.video.h264), cap_name(c.video.h265),
         cap_name(c.isp.available), cap_name(c.encoder.hardware), cap_name(c.ai.available));
    LOGI(MOD, "controls: sensor.fps=%s/%s [%d..%d] video.fps=%s/%s video.bitrate=%s/%s | isp.perf=%s enc.perf=%s cpu.freq=%s",
         cap_name(c.sensor.fps.support), apply_mode_name(c.sensor.fps.apply), c.sensor.fps.min, c.sensor.fps.max,
         cap_name(c.video.fps.support), apply_mode_name(c.video.fps.apply),
         cap_name(c.video.bitrate.support), apply_mode_name(c.video.bitrate.apply),
         cap_name(c.isp.performance.support), cap_name(c.encoder.performance.support), cap_name(c.power.cpu_frequency.support));
}

static void log_telemetry(power::PerformanceService& perf) {
    Telemetry t = perf.telemetry();
    char sfps[16], efps[16], kbps[16], cpu[16], rss[16], thr[16], isp[16], enc[16], cf[16];
    snprintf(sfps, sizeof sfps, t.effective_sensor_fps.available ? "%d" : "n/a", t.effective_sensor_fps.value);
    snprintf(efps, sizeof efps, t.measured_encoded_fps.available ? "%.1f" : "n/a", t.measured_encoded_fps.value);
    snprintf(kbps, sizeof kbps, t.measured_bitrate_kbps.available ? "%.0f" : "n/a", t.measured_bitrate_kbps.value);
    snprintf(cpu, sizeof cpu, t.cpu_percent.available ? "%.1f" : "n/a", t.cpu_percent.value);
    snprintf(rss, sizeof rss, t.rss_kb.available ? "%llu" : "n/a", (unsigned long long)t.rss_kb.value);
    snprintf(thr, sizeof thr, t.threads.available ? "%d" : "n/a", t.threads.value);
    snprintf(isp, sizeof isp, t.isp_clock_hz.available ? "%llu" : "n/a", (unsigned long long)(t.isp_clock_hz.value / 1000000));
    snprintf(enc, sizeof enc, t.encoder_clock_hz.available ? "%llu" : "n/a", (unsigned long long)(t.encoder_clock_hz.value / 1000000));
    snprintf(cf,  sizeof cf,  t.cpu_freq_khz.available ? "%llu" : "n/a", (unsigned long long)(t.cpu_freq_khz.value / 1000));
    LOGI("telemetry", "state=%s gen=%u profile=%s sensor_fps req=%d eff=%s%s stream_fps req=%d enc=%s bitrate req=%d meas=%skbps drops=%u cpu=%s%% rss=%skB thr=%s isp=%sMHz enc=%sMHz cpu=%sMHz",
         lifecycle::state_name(t.state), t.generation, power::profile_name(t.profile), t.requested_sensor_fps, sfps,
         t.sensor_fps_readback ? "(hw)" : "", t.requested_stream_fps, efps, t.requested_bitrate_kbps, kbps,
         t.dropped_frames, cpu, rss, thr, isp, enc, cf);
}

static const char* lc_lower(lifecycle::State s) {
    switch (s) {
        case lifecycle::State::ColdIdle: return "cold_idle"; case lifecycle::State::Starting: return "starting";
        case lifecycle::State::Active: return "active"; case lifecycle::State::GraceIdle: return "grace_idle";
        case lifecycle::State::Stopping: return "stopping"; case lifecycle::State::Failed: return "failed";
    }
    return "?";
}

int main(int argc, char** argv) {
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) { printf("machino %s\n", MACHINO_VERSION); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "--migrate-majestic")) return run_migration(argc, argv);

    const char* conf = "/etc/machino/machino.conf";   // canonical path (init, streamerctl, installer, manager all use it)
    bool verbose = false;
    int api_port_override = 0, api_upstream_override = -1;   // set by the boot script for front-door mode
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) conf = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = true;
        else if (!strcmp(argv[i], "--api-port") && i + 1 < argc) api_port_override = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--api-upstream-port") && i + 1 < argc) api_upstream_override = atoi(argv[++i]);
        else { usage(argv[0]); return 2; }
    }

    AppConfig cfg; std::string err;
    if (!load_config(conf, cfg, err)) { LOGE(MOD, "%s", err.c_str()); return 1; }
    // Front-door overrides win over the (possibly older, user-owned) config file,
    // so streamerctl/init can put Machino on port 80 relaying to busybox on :85
    // without ever editing machino.conf.
    if (api_port_override > 0) cfg.api.port = api_port_override;
    if (api_upstream_override >= 0) cfg.api.upstream_port = api_upstream_override;
    if (cfg.api.upstream_port > 0) {
        // Drop-in identity: the stock WebUI's CGIs use `pidof majestic`,
        // /proc/$pid/comm and `killall -HUP majestic`. Set the comm so those
        // hit Machino (SIGHUP already reloads the config). BusyBox pgrep -x
        // matches argv0 (verified on the T40NN for machino), so streamerctl's
        // pgrep -f /usr/bin/machino and pgrep -x majestic stay unambiguous.
        // PENDING hardware check: BusyBox pidof/killall find comm "majestic".
        prctl(PR_SET_NAME, "majestic", 0, 0, 0);
        LOGI(MOD, "majestic-compat: process comm set to 'majestic' (pidof/killall compatibility)");
    }
    log_set_level(verbose ? LogLevel::Debug : (LogLevel)cfg.log.level);
    log_set_syslog(cfg.log.syslog);
    LOGI(MOD, "machino %s starting (pid %d)", MACHINO_VERSION, (int)getpid());

    hw::Registry reg;
    profiles::register_builtin(reg);
    if (!cfg.board_profile_file.empty()) {
        hw::BoardProfile bp; std::string perr, warn;
        if (!hw::load_board_profile_file(cfg.board_profile_file.c_str(), bp, perr, &warn)) { LOGE(MOD, "board profile %s: %s", cfg.board_profile_file.c_str(), perr.c_str()); return 6; }
        if (!warn.empty()) LOGW(MOD, "board profile %s: %s", cfg.board_profile_file.c_str(), warn.c_str());
        reg.add_board(bp);
        if (cfg.hardware.board_id.empty()) cfg.hardware.board_id = bp.board_id;
    }
    std::string vendor;
    if (!cfg.hardware.platform.empty()) { if (const auto* p = reg.platform(cfg.hardware.platform)) vendor = p->vendor; }
    else if (const auto* b = reg.board(cfg.hardware.board_id)) { if (const auto* p = reg.platform(b->platform)) vendor = p->vendor; }

    hw::ResolvedHardware hwr;
    if (!hw::resolve_hardware(cfg.hardware, reg, platform_defaults_for(vendor), hwr, err)) {
        LOGE(MOD, "hardware resolution failed: %s", err.c_str());
        LOGE(MOD, "refusing to start: no guessing of buses or pins (set 'board = <profile>' or explicit sensor.* values)");
        return 7;
    }
    hw::log_resolved_hardware(hwr);
    EffectiveStream stream = effective_stream(cfg.video, hwr);
    LOGI(MOD, "Stream: %dx%d@%d gop=%d %d kbps profile=%d", stream.width, stream.height, stream.fps, stream.gop, stream.bitrate_kbps, stream.profile);

    sigset_t mask; sigemptyset(&mask);
    sigaddset(&mask, SIGINT); sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGPIPE);
    sigaddset(&mask, SIGUSR1); sigaddset(&mask, SIGUSR2); sigaddset(&mask, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

    int rc = 0;
    {
        std::unique_ptr<IPlatform> platform = make_platform(hwr);
        if (!platform) { LOGE(MOD, "no adapter for platform vendor '%s'", hwr.platform.vendor.c_str()); return 5; }

        app::LinuxGraceTimer timer;
        app::LinuxGraceTimer sub_timer, jpeg_timer;   // M8: per-unit grace
        app::LinuxSystemStats sysstats;
        StreamHub  hub;
        StreamHub  sub_hub;
        EventBus   bus;
        ConfigStore store(conf);
        if (!store.load(err)) LOGW(MOD, "config store: %s (API changes will not persist)", err.c_str());
        lifecycle::LifecycleConfig lc; lc.idle_grace_ms = cfg.pipeline.idle_grace_ms; lc.poll_timeout_ms = cfg.pipeline.poll_timeout_ms;
        lifecycle::PipelineManager pipeline(*platform, stream, lc, timer, hub);
        pipeline.set_state_listener([&bus](lifecycle::State from, lifecycle::State to) {
            Json j = Json::object(); j.set("from", Json::string(lc_lower(from))); j.set("to", Json::string(lc_lower(to)));
            bus.publish("lifecycle", j.dump());
        });
        // M8: substream (unit 1) - only when enabled and its geometry is valid;
        // geometry is never invented, so a bad config disables it with a warning.
        EffectiveStream sub_stream; std::string sub_err; bool sub_ok = false;
        if (cfg.video1.enabled) {
            sub_ok = effective_sub_stream(cfg.video1, stream, sub_stream, sub_err);
            if (sub_ok) { pipeline.configure_sub(sub_stream, sub_hub, &sub_timer);
                          LOGI(MOD, "substream ch1: %dx%d@%d %d kbps", sub_stream.width, sub_stream.height, sub_stream.fps, sub_stream.bitrate_kbps); }
            else        LOGW(MOD, "substream disabled: %s", sub_err.c_str());
        }
        // M8: hardware JPEG for snapshots - ephemeral, created on demand only.
        // Gated behind jpeg.enabled (default OFF): the live T40NN wedged whole-
        // daemon when the extra FS/encoder pair came up; opt-in until verified.
        if (!cfg.jpeg.enabled) {
            LOGI(MOD, "jpeg snapshots disabled (jpeg.enabled=false; MJPEG/snapshot report unsupported)");
        } else if (platform->capabilities().jpeg.supported == Cap::Supported) {
            JpegParams jp; jp.quality = cfg.jpeg.quality;
            pipeline.configure_jpeg(jp, cfg.snapshot.cache_ms, cfg.snapshot.grace_ms, &jpeg_timer);
            LOGI(MOD, "jpeg snapshots available (quality %d, cache %dms, grace %dms)", cfg.jpeg.quality, cfg.snapshot.cache_ms, cfg.snapshot.grace_ms);
        }
        power::PerformanceService perf(pipeline, *platform, sysstats, hwr, cfg.video);
        log_capabilities(perf.capabilities());
        perf.apply_config(cfg.performance, cfg.video);
        media::TuningService tuning(pipeline, *platform, hub, pipeline.stream(), cfg.image, cfg.latency);
        // M9: detection/AI. A consumer, not a media owner - it takes a base-only
        // demand when enabled. Construction brings the base up iff cfg.ai.enabled.
        detection::DetectionService detection(pipeline, *platform, bus, cfg.ai);
        if (cfg.ai.enabled) LOGI(MOD, "detection: %s (%s, %d fps) state=%s", cfg.ai.detector.c_str(),
                                 platform->capabilities().ai.motion == Cap::Supported ? "backend present" : "no backend",
                                 cfg.ai.inference_fps, detection::ai_state_name(detection.state()));
        // Constructed before the API so rtsp.enabled/rtsp.port can be applied
        // live (AP2) instead of only at the next daemon start.
        RtspServer rtsp(cfg.rtsp, pipeline, hub, sub_ok ? &sub_hub : nullptr);
        IStreamServer& server = rtsp;
        api::ApiService api(perf, tuning, pipeline, store, bus, hwr, cfg, &detection, &rtsp);
        http::ServerConfig hc; hc.bind = cfg.api.bind; hc.port = cfg.api.port;
        hc.upstream_host = cfg.api.upstream_host; hc.upstream_port = cfg.api.upstream_port;
        // Front-door: the Majestic drop-in login gates :80 exactly like
        // Majestic did - the WebUI login IS the camera's system login.
        hc.session_auth = cfg.api.upstream_port > 0 && cfg.api.auth;
        if (hc.session_auth) hc.auth_check = shadow_check;
        LOGI(MOD, "webui session auth: %s", hc.session_auth ? "on (system account)" : "off");
        // /ws/video and /ws/webrtc: hub consumers with their own demand;
        // stream=1 serves from the substream hub when it is configured.
        http::HttpServer httpd(hc, api, bus, &hub, &pipeline, sub_ok ? &sub_hub : nullptr);
        int tfd = -1;
        if (cfg.telemetry.log_interval_s > 0) {
            tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
            struct itimerspec its{}; its.it_interval.tv_sec = cfg.telemetry.log_interval_s; its.it_value.tv_sec = cfg.telemetry.log_interval_s;
            timerfd_settime(tfd, 0, &its, nullptr);
        }
        int ep = epoll_create1(EPOLL_CLOEXEC);
        epoll_event ev{}; ev.events = EPOLLIN;
        ev.data.fd = sfd;        epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev);
        ev.data.fd = timer.fd(); epoll_ctl(ep, EPOLL_CTL_ADD, timer.fd(), &ev);
        ev.data.fd = sub_timer.fd();  epoll_ctl(ep, EPOLL_CTL_ADD, sub_timer.fd(), &ev);
        ev.data.fd = jpeg_timer.fd(); epoll_ctl(ep, EPOLL_CTL_ADD, jpeg_timer.fd(), &ev);
        if (tfd >= 0) { ev.data.fd = tfd; epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev); }

        lifecycle::DemandHandle hold;
        if (cfg.pipeline.always_on) {
            Result r; hold = pipeline.acquire(ConsumerType::Manual, &r);
            if (!hold.active()) { LOGE(MOD, "always_on: pipeline bring-up failed"); rc = 3; }
        }
        if (rc == 0 && cfg.api.enabled && !httpd.start()) { LOGE(MOD, "API server start failed"); rc = 8; }
        if (rc == 0 && !server.start()) { LOGE(MOD, "stream server start failed"); rc = 4; }

        if (rc == 0) {
            LOGI(MOD, "running: rtsp://<ip>:%d%s api=http://<ip>:%d/api/v1 lifecycle=%s idle_grace=%dms profile=%s revision=%u",
                 cfg.rtsp.port, cfg.rtsp.path.c_str(), cfg.api.port, lifecycle::state_name(pipeline.state()),
                 cfg.pipeline.idle_grace_ms, power::profile_name(cfg.performance.profile), store.revision());
            bool run = true;
            while (run) {
                epoll_event out[8];
                int n = epoll_wait(ep, out, 8, -1);
                for (int i = 0; i < n; ++i) {
                    if (out[i].data.fd == sfd) {
                        signalfd_siginfo si;
                        while (read(sfd, &si, sizeof si) == (ssize_t)sizeof si) {
                            if (si.ssi_signo == SIGINT || si.ssi_signo == SIGTERM) { LOGI(MOD, "signal %d - shutting down", (int)si.ssi_signo); run = false; }
                            else if (si.ssi_signo == SIGUSR1) { LOGI(MOD, "SIGUSR1 -> release manual hold"); hold.release(); }
                            else if (si.ssi_signo == SIGUSR2) {
                                if (hold.active()) LOGI(MOD, "SIGUSR2 -> manual hold already held");
                                else { LOGI(MOD, "SIGUSR2 -> take manual hold"); Result r; hold = pipeline.acquire(ConsumerType::Manual, &r);
                                       if (!hold.active()) LOGE(MOD, "manual hold: pipeline start failed"); }
                            }
                            else if (si.ssi_signo == SIGHUP) {
                                AppConfig fresh; std::string e2;
                                if (!load_config(conf, fresh, e2)) { LOGW(MOD, "SIGHUP: reload failed: %s", e2.c_str()); continue; }
                                LOGI(MOD, "SIGHUP -> applying performance/stream configuration");
                                perf.apply_config(fresh.performance, fresh.video);
                                detection.apply_config(fresh.ai);
                                tuning.set_latency_profile(fresh.latency.profile);
                                if (fresh.latency.gop) tuning.set_gop(*fresh.latency.gop);
                                if (fresh.latency.framesource_buffers) tuning.set_framesource_buffers(*fresh.latency.framesource_buffers);
                                if (fresh.latency.encoder_buffers) tuning.set_encoder_buffers(*fresh.latency.encoder_buffers);
                                if (fresh.latency.consumer_queue_depth) tuning.set_queue_depth(*fresh.latency.consumer_queue_depth);
                                const media::ImageSettings& im = fresh.image;
                                auto image = [&](ImageControl c, const std::optional<int>& v) { if (v) tuning.set_image(c, *v); };
                                image(ImageControl::Brightness, im.brightness); image(ImageControl::Contrast, im.contrast);
                                image(ImageControl::Saturation, im.saturation); image(ImageControl::Sharpness, im.sharpness); image(ImageControl::Hue, im.hue);
                                image(ImageControl::HFlip, im.hflip); image(ImageControl::VFlip, im.vflip); image(ImageControl::AntiFlicker, im.anti_flicker);
                                image(ImageControl::AeCompensation, im.ae_compensation); image(ImageControl::HighlightDepress, im.highlight_depress);
                                image(ImageControl::BacklightComp, im.backlight_comp); image(ImageControl::WhiteBalanceMode, im.white_balance_mode);
                                image(ImageControl::RunningMode, im.running_mode); image(ImageControl::TemporalNr, im.temporal_nr);
                                image(ImageControl::SpatialNr, im.spatial_nr); image(ImageControl::Dpc, im.dpc); image(ImageControl::Defog, im.defog);
                                store.load(e2);
                                Json j = Json::object(); j.set("revision", Json::integer(store.revision())); j.set("source", Json::string("sighup"));
                                bus.publish("config_changed", j.dump());
                                log_telemetry(perf);
                            }
                        }
                    } else if (out[i].data.fd == timer.fd()) {
                        if (timer.consume()) pipeline.on_grace_timeout();
                    } else if (out[i].data.fd == sub_timer.fd()) {
                        if (sub_timer.consume()) pipeline.on_unit_grace(lifecycle::UNIT_SUB);
                    } else if (out[i].data.fd == jpeg_timer.fd()) {
                        if (jpeg_timer.consume()) pipeline.on_jpeg_grace();
                    } else if (tfd >= 0 && out[i].data.fd == tfd) {
                        uint64_t x; while (read(tfd, &x, sizeof x) > 0) {}
                        log_telemetry(perf);
                    }
                }
            }
        }
        httpd.stop();
        server.stop();
        detection.shutdown();   // stop the detector and release its base demand before the base goes down
        hold.release();
        pipeline.shutdown();
        lifecycle::Stats st = pipeline.stats();
        LOGI(MOD, "lifecycle summary: generations=%u starts=%u stops=%u restarts=%u failed=%u last_error='%s'",
             st.generation, st.start_count, st.stop_count, st.restart_count, st.failed_count, st.last_error.c_str());
        if (tfd >= 0) close(tfd);
        close(ep);
    }

    close(sfd);
    LOGI(MOD, "exit %d", rc);
    return rc;
}
