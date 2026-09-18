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
#include "app/http/http_server.hpp"
#include "app/linux_grace_timer.hpp"
#include "app/linux_system_stats.hpp"
#include "app/rtsp/rtsp_server.hpp"
#include "core/capabilities.hpp"
#include "core/config.hpp"
#include "core/config_store.hpp"
#include "core/events.hpp"
#include "core/hw/board_profile_parser.hpp"
#include "core/hw/registry.hpp"
#include "core/hw/resolve.hpp"
#include "core/json.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/log.hpp"
#include "core/power/performance_service.hpp"
#include "core/stream_hub.hpp"
#include "profiles/builtin_profiles.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

using namespace machino;
using lifecycle::ConsumerType;

static const char* MOD = "MAIN";

static void usage(const char* argv0) { fprintf(stderr, "usage: %s [-c machino.conf] [-v]\n", argv0); }

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
    const char* conf = "/etc/machino.conf";
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) conf = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = true;
        else { usage(argv[0]); return 2; }
    }

    AppConfig cfg; std::string err;
    if (!load_config(conf, cfg, err)) { LOGE(MOD, "%s", err.c_str()); return 1; }
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
        app::LinuxSystemStats sysstats;
        StreamHub  hub;
        EventBus   bus;
        ConfigStore store(conf);
        if (!store.load(err)) LOGW(MOD, "config store: %s (API changes will not persist)", err.c_str());
        lifecycle::LifecycleConfig lc; lc.idle_grace_ms = cfg.pipeline.idle_grace_ms; lc.poll_timeout_ms = cfg.pipeline.poll_timeout_ms;
        lifecycle::PipelineManager pipeline(*platform, stream, lc, timer, hub);
        pipeline.set_state_listener([&bus](lifecycle::State from, lifecycle::State to) {
            Json j = Json::object(); j.set("from", Json::string(lc_lower(from))); j.set("to", Json::string(lc_lower(to)));
            bus.publish("lifecycle", j.dump());
        });
        power::PerformanceService perf(pipeline, *platform, sysstats, hwr, cfg.video);
        log_capabilities(perf.capabilities());
        perf.apply_config(cfg.performance, cfg.video);
        api::ApiService api(perf, pipeline, store, bus, hwr, cfg);
        http::ServerConfig hc; hc.bind = cfg.api.bind; hc.port = cfg.api.port;
        http::HttpServer httpd(hc, api, bus);
        RtspServer rtsp(cfg.rtsp, pipeline, hub);
        IStreamServer& server = rtsp;

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
                epoll_event out[4];
                int n = epoll_wait(ep, out, 4, -1);
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
                                store.load(e2);
                                Json j = Json::object(); j.set("revision", Json::integer(store.revision())); j.set("source", Json::string("sighup"));
                                bus.publish("config_changed", j.dump());
                                log_telemetry(perf);
                            }
                        }
                    } else if (out[i].data.fd == timer.fd()) {
                        if (timer.consume()) pipeline.on_grace_timeout();
                    } else if (tfd >= 0 && out[i].data.fd == tfd) {
                        uint64_t x; while (read(tfd, &x, sizeof x) > 0) {}
                        log_telemetry(perf);
                    }
                }
            }
        }
        httpd.stop();
        server.stop();
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
