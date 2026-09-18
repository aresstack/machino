// Machino entry point: config -> registry -> resolved hardware -> platform
// adapter -> PipelineManager -> RTSP. Single process, event loop on
// signalfd + the lifecycle grace timerfd (epoll); no polling.
//
// An open Machino daemon is not an active camera pipeline: the media chain
// is brought up by demand (RTSP PLAY, manual hold) and torn down after the
// grace period when the last consumer left.
//   SIGUSR1 = drop the manual hold    SIGUSR2 = take a manual hold
#include "adapters/ingenic/ingenic_platform.hpp"
#include "app/linux_grace_timer.hpp"
#include "app/rtsp/rtsp_server.hpp"
#include "core/capabilities.hpp"
#include "core/config.hpp"
#include "core/hw/board_profile_parser.hpp"
#include "core/hw/registry.hpp"
#include "core/hw/resolve.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/log.hpp"
#include "core/stream_hub.hpp"
#include "profiles/builtin_profiles.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/epoll.h>
#include <sys/signalfd.h>
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
    LOGI(MOD, "capabilities: h264=%s h265=%s max_streams=%d isp=%s hw-encoder=%s sensor.fps=%s power(isp=%s enc=%s cpu=%s) ai=%s",
         cap_name(c.video.h264), cap_name(c.video.h265), c.video.max_streams, cap_name(c.isp.available),
         cap_name(c.encoder.hardware), cap_name(c.sensor.configurable_fps), cap_name(c.power.isp_clock_control),
         cap_name(c.power.encoder_clock_control), cap_name(c.power.cpu_frequency_control), cap_name(c.ai.available));
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
        LOGI(MOD, "board profile file %s registered as '%s'", cfg.board_profile_file.c_str(), bp.board_id.c_str());
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
    LOGI(MOD, "Stream: %dx%d@%d gop=%d %d kbps profile=%d", stream.width, stream.height, stream.fps,
         stream.gop, stream.bitrate_kbps, stream.profile);

    // Block the handled signals process-wide before any thread exists.
    sigset_t mask; sigemptyset(&mask);
    sigaddset(&mask, SIGINT); sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGPIPE);
    sigaddset(&mask, SIGUSR1); sigaddset(&mask, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

    int rc = 0;
    {
        std::unique_ptr<IPlatform> platform = make_platform(hwr);
        if (!platform) { LOGE(MOD, "no adapter for platform vendor '%s'", hwr.platform.vendor.c_str()); return 5; }
        log_capabilities(platform->capabilities());

        app::LinuxGraceTimer timer;
        StreamHub  hub;
        lifecycle::LifecycleConfig lc; lc.idle_grace_ms = cfg.pipeline.idle_grace_ms; lc.poll_timeout_ms = cfg.pipeline.poll_timeout_ms;
        lifecycle::PipelineManager pipeline(*platform, stream, lc, timer, hub);
        RtspServer rtsp(cfg.rtsp, pipeline, hub);
        IStreamServer& server = rtsp;

        int ep = epoll_create1(EPOLL_CLOEXEC);
        epoll_event ev{}; ev.events = EPOLLIN;
        ev.data.fd = sfd;        epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev);
        ev.data.fd = timer.fd(); epoll_ctl(ep, EPOLL_CTL_ADD, timer.fd(), &ev);

        lifecycle::DemandHandle hold;                       // manual demand (always_on / SIGUSR2)
        if (cfg.pipeline.always_on) {
            Result r; hold = pipeline.acquire(ConsumerType::Manual, &r);
            if (!hold.active()) { LOGE(MOD, "always_on: pipeline bring-up failed"); rc = 3; }
        }
        if (rc == 0 && !server.start()) { LOGE(MOD, "stream server start failed"); rc = 4; }

        if (rc == 0) {
            LOGI(MOD, "running: rtsp://<ip>:%d%s lifecycle=%s idle_grace=%dms", cfg.rtsp.port, cfg.rtsp.path.c_str(),
                 lifecycle::state_name(pipeline.state()), cfg.pipeline.idle_grace_ms);
            bool run = true;
            while (run) {
                epoll_event out[4];
                int n = epoll_wait(ep, out, 4, -1);          // purely event-driven
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
                        }
                    } else if (out[i].data.fd == timer.fd()) {
                        if (timer.consume()) pipeline.on_grace_timeout();
                    }
                }
            }
        }
        // Reverse order: stop serving (sessions release their demand), drop the hold, then the manager.
        server.stop();
        hold.release();
        pipeline.shutdown();
        lifecycle::Stats st = pipeline.stats();
        LOGI(MOD, "lifecycle summary: generations=%u starts=%u stops=%u failed=%u last_error='%s'",
             st.generation, st.start_count, st.stop_count, st.failed_count, st.last_error.c_str());
        close(ep);
    }   // platform dtor: tear_down() (idempotent)

    close(sfd);
    LOGI(MOD, "exit %d", rc);
    return rc;
}
