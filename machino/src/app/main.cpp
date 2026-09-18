// Machino entry point: config -> registry -> resolved hardware -> platform
// adapter -> pipeline -> RTSP. Single process, event loop on signalfd +
// timerfd; deterministic teardown on SIGINT/SIGTERM in reverse order.
//   SIGUSR1 = stop_pipeline()   SIGUSR2 = start_pipeline()
// Exactly one deterministically selected profile -> one media init. No
// probing of buses, pins or sensors.
#include "adapters/ingenic/ingenic_platform.hpp"
#include "app/rtsp/rtsp_server.hpp"
#include "core/capabilities.hpp"
#include "core/config.hpp"
#include "core/hw/board_profile_parser.hpp"
#include "core/hw/registry.hpp"
#include "core/hw/resolve.hpp"
#include "core/log.hpp"
#include "core/pipeline.hpp"
#include "core/stream_hub.hpp"
#include "profiles/builtin_profiles.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

using namespace machino;

static const char* MOD = "MAIN";

static int64_t now_ms() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void usage(const char* argv0) {
    fprintf(stderr, "usage: %s [-c machino.conf] [-v]\n", argv0);
}

// The only place that maps a platform vendor to a concrete adapter.
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

    // ---- hardware description: registry -> resolver (user > board > default > fail closed)
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
    // vendor for platform defaults: from the user platform or the selected board
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

    sigset_t mask; sigemptyset(&mask);
    sigaddset(&mask, SIGINT); sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGPIPE);
    sigaddset(&mask, SIGUSR1); sigaddset(&mask, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    struct itimerspec its{}; its.it_interval.tv_nsec = 250 * 1000000L; its.it_value.tv_nsec = 250 * 1000000L;
    timerfd_settime(tfd, 0, &its, nullptr);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    epoll_event ev{}; ev.events = EPOLLIN;
    ev.data.fd = sfd; epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev);
    ev.data.fd = tfd; epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);

    int rc = 0;
    {
        std::unique_ptr<IPlatform> platform = make_platform(hwr);
        if (!platform) { LOGE(MOD, "no adapter for platform vendor '%s'", hwr.platform.vendor.c_str()); return 5; }
        log_capabilities(platform->capabilities());
        StreamHub  hub;
        Pipeline   pipeline(*platform, stream, cfg.pipeline, hub);
        RtspServer rtsp(cfg.rtsp, pipeline, hub);
        IStreamServer& server = rtsp;

        if (cfg.pipeline.always_on && !pipeline.start_pipeline()) { LOGE(MOD, "pipeline bring-up failed"); rc = 3; }
        if (rc == 0 && !server.start()) { LOGE(MOD, "stream server start failed"); rc = 4; }

        if (rc == 0) {
            LOGI(MOD, "running: rtsp://<ip>:%d%s (%s)", cfg.rtsp.port, cfg.rtsp.path.c_str(),
                 cfg.pipeline.always_on ? "always-on" : "on-demand");
            bool run = true;
            while (run) {
                epoll_event out[4];
                int n = epoll_wait(ep, out, 4, 1000);
                for (int i = 0; i < n; ++i) {
                    if (out[i].data.fd == sfd) {
                        signalfd_siginfo si;
                        while (read(sfd, &si, sizeof si) == (ssize_t)sizeof si) {
                            if (si.ssi_signo == SIGINT || si.ssi_signo == SIGTERM) { LOGI(MOD, "signal %d - shutting down", (int)si.ssi_signo); run = false; }
                            else if (si.ssi_signo == SIGUSR1) { LOGI(MOD, "SIGUSR1 -> stop_pipeline"); pipeline.stop_pipeline(); }
                            else if (si.ssi_signo == SIGUSR2) { LOGI(MOD, "SIGUSR2 -> start_pipeline"); if (!pipeline.start_pipeline()) LOGE(MOD, "start_pipeline failed"); }
                        }
                    } else if (out[i].data.fd == tfd) {
                        uint64_t x; while (read(tfd, &x, sizeof x) > 0) {}
                        pipeline.tick(now_ms());
                    }
                }
            }
        }
        server.stop();
        pipeline.stop_pipeline();
    }

    close(ep); close(tfd); close(sfd);
    LOGI(MOD, "exit %d", rc);
    return rc;
}
