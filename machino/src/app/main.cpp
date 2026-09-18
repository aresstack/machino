// Machino M2 entry point: config -> platform adapter -> pipeline -> RTSP.
// Single process, event loop on signalfd + timerfd; deterministic teardown
// on SIGINT/SIGTERM in reverse order of construction.
//   SIGUSR1 = stop_pipeline()   SIGUSR2 = start_pipeline()   (in-process lifecycle)
#include "adapters/ingenic/ingenic_platform.hpp"
#include "app/rtsp/rtsp_server.hpp"
#include "core/config.hpp"
#include "core/log.hpp"
#include "core/pipeline.hpp"
#include "core/stream_hub.hpp"

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

// Adapter selection is the only place that knows concrete platform names.
static std::unique_ptr<IPlatform> make_platform(const AppConfig& cfg) {
    if (cfg.platform == "ingenic-t40nn" || cfg.platform == "ingenic-t40")
        return std::make_unique<ingenic::IngenicPlatform>(cfg.sensor, ingenic::wiring_from_config(cfg.bus));
    return nullptr;
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
    LOGI(MOD, "machino %s starting (pid %d, platform %s)", MACHINO_VERSION, (int)getpid(), cfg.platform.c_str());

    // Block the handled signals process-wide before any thread exists so they
    // are delivered only through the signalfd in this loop.
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
        std::unique_ptr<IPlatform> platform = make_platform(cfg);
        if (!platform) { LOGE(MOD, "unknown platform '%s' (supported: ingenic-t40nn, ingenic-t40)", cfg.platform.c_str()); return 5; }
        StreamHub  hub;
        Pipeline   pipeline(*platform, cfg, hub);
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
        // Reverse order: stop serving (releases consumers), then the pipeline, then the platform (dtor).
        server.stop();
        pipeline.stop_pipeline();
    }

    close(ep); close(tfd); close(sfd);
    LOGI(MOD, "exit %d", rc);
    return rc;
}
