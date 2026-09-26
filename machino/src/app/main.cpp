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
#include "app/api/net_api.hpp"
#include "core/devices/aic8800_package.hpp"
#include "core/devices/device_package.hpp"
#include "adapters/linux/at_transport.hpp"
#include "adapters/linux/linux_ecm_backend.hpp"
#include "adapters/linux/linux_ppp_backend.hpp"
#include "adapters/linux/linux_ethernet_uplink.hpp"
#include "adapters/linux/linux_ipsec_backend.hpp"
#include "adapters/linux/linux_route_backend.hpp"
#include "adapters/linux/linux_serial_scan.hpp"
#include "adapters/linux/linux_usb_host.hpp"
#include "adapters/linux/sysfs_gpio.hpp"
#include "adapters/linux/wifi_station_uplink.hpp"
#include "adapters/linux/hostapd_ap.hpp"
#include "adapters/linux/wpa_supplicant_wifi.hpp"
#include "core/cellular/ppp_link.hpp"
#include "core/net/cellular_uplink.hpp"
#include "core/net/route_manager.hpp"
#include "core/net/route_plan.hpp"
#include "app/compat/majestic_migrate.hpp"
#include "app/http/http_server.hpp"
#include "app/onvif/discovery_server.hpp"
#include "app/onvif/onvif_service.hpp"
#include "app/log_reader.hpp"
#include "app/osd/osd_service.hpp"
#include "app/linux_grace_timer.hpp"
#include "app/linux_system_stats.hpp"
#include "app/linux_watchdog.hpp"
#include "app/watchdog.hpp"
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
#include "core/random.hpp"
#include "core/runtime_stats.hpp"
#include "core/stream_hub.hpp"
#include "profiles/builtin_profiles.hpp"
#include "profiles/usb_profiles.hpp"
#include "core/state_store.hpp"

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
#include <sys/wait.h>
#include <cerrno>
#include <ctime>
#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>

using namespace machino;
using lifecycle::ConsumerType;

static const char* MOD = "MAIN";

// Monotonic milliseconds for the watchdog. steady_clock, not the wall clock:
// ONVIF SetSystemDateAndTime can step the system time, and a feeder whose
// schedule jumps backwards would stop feeding until the clock caught up.
static int64_t now_ms() {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

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
        // An EMPTY hash means an UNCLAIMED camera - it must NEVER count as
        // "empty password accepted". Nobody logs in until the claim flow
        // (AP10, see claim_state/SetupGate) has set a real password; the same
        // emptiness is what that flow reads as its state.
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

// AP10: the claim state IS root's shadow entry - there is no second record,
// because SSH (`openipc-claim`) sets the same one and the two must never be
// able to disagree.
//
// Fail CLOSED: if the file cannot be read we report CLAIMED, so a camera that
// cannot answer the question never opens an unauthenticated page that sets the
// root password.
static http::ClaimState claim_state() {
    FILE* f = fopen("/etc/shadow", "r");
    if (!f) return http::ClaimState::Claimed;
    char line[512];
    http::ClaimState st = http::ClaimState::Claimed;
    while (fgets(line, sizeof line, f)) {
        char* c1 = strchr(line, ':');
        if (!c1) continue;
        *c1 = 0;
        if (strcmp(line, "root") != 0) continue;
        char* hash = c1 + 1;
        if (char* c2 = strchr(hash, ':')) *c2 = 0;
        if (hash[0] == 0) st = http::ClaimState::Unclaimed;   // empty hash = unclaimed
        break;
    }
    fclose(f);
    return st;
}

// `root:<password>` piped to chpasswd, which is what the stock flow does and
// what keeps the on-disk write (temp file + rename inside chpasswd) out of this
// process. The password never reaches a command line, a log or an environment
// variable - only that pipe. SIGPIPE is blocked process-wide, so a chpasswd
// that dies early surfaces as EPIPE rather than killing the daemon.
static bool set_root_password(const std::string& pw, std::string& err) {
    int fds[2];
    if (pipe(fds) != 0) { err = "The camera could not start the password helper."; return false; }
    const pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); err = "The camera could not start the password helper."; return false; }
    if (pid == 0) {
        close(fds[1]);
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        execlp("chpasswd", "chpasswd", (char*)nullptr);
        _exit(127);
    }
    close(fds[0]);
    const std::string line = "root:" + pw + "\n";
    size_t off = 0;
    bool wrote = true;
    while (off < line.size()) {
        const ssize_t n = write(fds[1], line.data() + off, line.size() - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        wrote = false;
        break;
    }
    close(fds[1]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (!wrote) { err = "The password could not be handed to the system."; return false; }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        err = WIFEXITED(st) && WEXITSTATUS(st) == 127
                  ? "This image has no chpasswd, so the password cannot be set here."
                  : "The system refused the password.";
        return false;
    }
    return true;
}

// Upstream enforces EULA acceptance "whenever the document is on the image".
// setup.html fetches /eula.<lang>.txt from the web root, so the presence of any
// of them is the same question.
static bool eula_present() {
    static const char* LANGS[] = {"en", "ru", "zh-CN"};
    for (const char* l : LANGS) {
        const std::string p = std::string("/var/www/eula.") + l + ".txt";
        if (access(p.c_str(), R_OK) == 0) return true;
    }
    return false;
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
    // In drop-in mode the system log is the WebUI log viewer: it streams
    // logread and filters its "majestic" source on that exact program name,
    // which is also the name this process already answers to for
    // pidof/killall. Outside drop-in mode the honest ident is our own.
    const bool dropin = cfg.api.upstream_port > 0;
    log_set_syslog(cfg.log.syslog || dropin, dropin ? "majestic" : "machino");
    LOGI(MOD, "machino %s starting (pid %d)", MACHINO_VERSION, (int)getpid());

    // The only fork() on the STREAMING path, and it happens HERE - before the
    // hardware registry, before any platform adapter exists, and long before
    // IMP can have been initialised. Forking later, while IMP is live, leaves
    // the process permanently unable to re-initialise it after the next
    // teardown (measured; docs/incident-2026-09-22-oom.md). /ws/logs from here
    // on only adds and removes subscribers.
    //
    // AP27 correction: this used to say "THE ONLY fork() in the daemon's
    // steady-state life", and that is not true - set_root_password() forks a
    // chpasswd. What makes that one safe is a different argument, and it was
    // nowhere written down: it can only run from the SetupGate, which only
    // exists while the camera is UNCLAIMED, and an unclaimed camera has no
    // consumer that could have started IMP (HTTP redirects everything to
    // /setup, RTSP answers 401). So IMP is not live when it runs. That is a
    // property of the claim flow, not of the fork - if /setup ever becomes
    // reachable on a claimed camera, this stops holding.
    //
    // Failure is not fatal: the camera streams fine without a log viewer.
    LogReader log_reader;
    if (!log_reader.start())
        LOGW(MOD, "no log reader - /ws/logs will accept and close");

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
        // The credential is the system account, exactly as the stock UI states
        // for these endpoints ("root with the same password you use for this
        // WebUI") - the same validator the HTTP session gate uses.
        RtspServer rtsp(cfg.rtsp, pipeline, hub, sub_ok ? &sub_hub : nullptr, shadow_check,
                        [] { return claim_state() == http::ClaimState::Claimed; },
                        cfg.system.unsafe);
        IStreamServer& server = rtsp;
        api::ApiService api(perf, tuning, pipeline, store, bus, hwr, cfg, &detection, &rtsp);
        // AP-NNA5: der Availability-Vertrag aus der Plattform in die API --
        // dieselbe Bewertung, die auch die Detector-Fabrik gated.
        // static_cast, nicht dynamic_cast: das Binary baut mit -fno-rtti, und
        // der Vendor-Check ist exakt die Bedingung, unter der make_platform
        // den Ingenic-Typ erzeugt hat.
        if (hwr.platform.vendor == "ingenic") {
            auto* ing = static_cast<ingenic::IngenicPlatform*>(platform.get());
            api.set_detector_status_provider(
                [ing](const std::string& mp) { return ing->detector_status(mp); });
        }

        // AP3 (Feature 2): IPsec/VPN ueber die Prozessgrenze -- machinod
        // linkt KEIN WeirdIKE; Lifecycle laeuft ueber S99weirdike, Status
        // ueber den ctl-Socket. Fehler hier beruehren das Video nie.
        ipsec::LinuxIpsecBackend ipsec_backend;
        ipsec::IpsecService ipsec_service(ipsec_backend,
                                          "/etc/machino/ipsec.conf",
                                          "/etc/weirdike/weirdike.conf");
        api.set_ipsec_service(&ipsec_service);

        // AP35/AP36: the USB host and the connectivity layer.
        //
        // Constructed HERE, before the HTTP server, so they outlive it: the
        // server holds a pointer to the API object below and a client may
        // still be mid-request while the stack unwinds.
        //
        // None of this is part of the media lifecycle. A USB device appearing
        // or a WiFi change must not be able to touch IMP, the pipeline or
        // WebRTC -- the camera keeps streaming whatever the port does.
        UsbPowerCapability usb_power;
        if (!profiles::usb_power_for_board(hwr.board_id, usb_power))
            LOGI(MOD, "usb: no power profile for board '%s' - the port is reported as not switchable",
                 hwr.board_id.c_str());
        linuxsys::SysfsGpio usb_gpio(profiles::ingenic_pin_resolver());
        linuxsys::LinuxUsbHostBackend usb_backend(usb_gpio, usb_power);
        usb::UsbHostService usb_service(usb_backend);

        // Settings are read through the key list the writer produces, so the
        // names cannot drift apart. An absent key keeps the built-in default.
        auto read_settings = [&store](const KeyValues& keys) {
            KeyValues present;
            for (const auto& kv : keys) {
                const std::string v = store.get(kv.first);
                if (!v.empty()) present.emplace_back(kv.first, v);
            }
            return present;
        };

        {
            usb::UsbConfig uc;
            KeyValues keys; api::usb_config_to_settings(uc, keys);
            std::string e;
            if (!api::usb_config_from_settings(read_settings(keys), uc, e))
                LOGW(MOD, "usb: %s - using defaults", e.c_str());
            bool applied = false;
            // Adopted, not applied: whether the port comes up is
            // enable_at_boot's decision, and apply() here would override it.
            if (!usb_service.load_config(uc, e))
                LOGW(MOD, "usb: the stored configuration was rejected (%s) - leaving the port alone", e.c_str());
            else if (!usb_service.apply_at_boot(e, &applied).is_ok())
                LOGW(MOD, "usb: boot-time power-up failed: %s", e.c_str());
            else if (applied)
                LOGI(MOD, "usb: port power enabled at boot");
        }

        // Was der Boot-Helfer WIRKLICH gestartet hat.
        //
        // Nicht dasselbe wie usb.mode in der Konfiguration: dazwischen liegt
        // der Neustart. Die Marke schreibt machino-usb-helper beim Start, und
        // sie liegt unter /var/run -- sie ueberlebt den Neustart absichtlich
        // nicht, denn genau danach stimmen die beiden Werte wieder ueberein.
        //
        // Nicht lesbar heisst "off". Das ist die vorsichtige Richtung: es
        // meldet einen noetigen Neustart zu viel, nie einen zu wenig.
        usb::UsbFunction boot_usb_function = usb::UsbFunction::Off;
        {
            std::ifstream f("/var/run/machino-usb-mode");
            std::string t;
            if (f && std::getline(f, t)) {
                while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' '))
                    t.pop_back();
                if (!t.empty() && !usb::usb_function_parse(t, boot_usb_function)) {
                    LOGW(MOD, "usb: the boot helper recorded an unknown mode - assuming off");
                    boot_usb_function = usb::UsbFunction::Off;
                }
            }
            usb_service.set_boot_function(boot_usb_function);
            LOGI(MOD, "usb: boot helper started '%s', configuration says '%s'",
                 usb::usb_function_name(boot_usb_function),
                 usb::usb_function_name(usb_service.config().function));
        }

        linuxsys::LinuxEthernetUplink eth_uplink("eth0");
        linuxsys::WpaSupplicantWifi   wifi("wlan0");
        // hostapd is started by the init script, before this process and
        // therefore before IMP. Machino only writes its config and
        // reconfigures it over the control socket -- fork+exec while the media
        // pipeline is live is the documented trigger of an out-of-memory
        // incident on this camera.
        linuxsys::HostapdAp           wifi_ap("wlan0");
        wifi.set_ap(&wifi_ap);
        linuxsys::WifiStationUplink   wifi_uplink(wifi, "wlan0");

        net::ConnectivityManager conn;
        conn.add(&eth_uplink);

        // The WiFi uplink is registered UNCONDITIONALLY, and that is the
        // point. An earlier version added it only when a radio was present at
        // start-up, which looked reasonable and was wrong on this camera: the
        // AIC8800 hangs off USB, its modules are loaded by hand, and the port
        // power is a GPIO. The radio therefore appears MINUTES after boot, and
        // a start-up snapshot had already decided there was none.
        //
        // The cost of getting that wrong is not cosmetic. The only way to
        // re-evaluate it would be restarting the daemon, and a warm restart is
        // this camera's documented hardlock trigger -- so the WiFi surface
        // would have stayed dead until the next power cycle.
        //
        // Nothing is claimed by registering it: capabilities() is a live
        // query, WifiStationUplink::state() answers Absent while there is no
        // interface, and the connectivity manager skips an uplink that is not
        // usable. The honest answer arrives when the hardware does.
        conn.add(&wifi_uplink);

        // AP-M5: cellular, the third uplink.
        //
        // Registered unconditionally for exactly the reason the WiFi one is.
        // The modem hangs off the same USB port, its power is a GPIO and its
        // kernel modules are loaded by hand, so it appears minutes after boot
        // if it appears at all. A snapshot taken here would decide there is no
        // modem, and the only way to revisit that would be restarting the
        // daemon -- this camera's documented hardlock trigger.
        linuxsys::AtTransport     modem_at("");
        linuxsys::LinuxEcmBackend ecm_backend;
        linuxsys::LinuxPppBackend ppp_backend;
        cellular::CellularService cell_service(modem_at);
        cellular::EcmLink         cell_ecm(modem_at, ecm_backend);
        cellular::PppLink         cell_ppp(modem_at, ppp_backend);
        cell_service.set_clock([] { return (uint64_t)now_ms(); });
        cell_ecm.set_clock([] { return (uint64_t)now_ms(); });
        cell_ppp.set_clock([] { return (uint64_t)now_ms(); });

        // EINER von beiden, und die Wahl faellt HIER -- einmal, beim Start.
        //
        // Beide Objekte existieren; das kostet nichts, weil ein Konstruktor
        // hier nichts tut. Was NICHT passiert, ist beide zu betreiben: nur der
        // gewaehlte bekommt tick(), also spricht nur einer mit dem Modem. Zwei
        // Zustandsmaschinen auf einem AT-Port waeren zwei Sprecher, und die
        // Antwort der einen landete in der anderen.
        //
        // Der Wert kommt aus der Datei und nicht aus einer spaeteren
        // Konfiguration: ECM und PPP brauchen verschiedene Kernelmodule, und
        // die hat der Boot-Helfer schon geladen. Ein Wechsel ist ein Neustart.
        const bool want_ppp = [&store] {
            const std::string v = store.get("cellular.data_link");
            return v == "ppp";
        }();
        cellular::ICellularDataLink& cell_link =
            want_ppp ? static_cast<cellular::ICellularDataLink&>(cell_ppp)
                     : static_cast<cellular::ICellularDataLink&>(cell_ecm);
        LOGI(MOD, "cellular: data link is %s", want_ppp ? "ppp" : "ecm");

        net::CellularUplink cell_uplink(cell_service, cell_link);

        // The stored cellular configuration is the SOURCE OF TRUTH.
        //
        // A staged change deviates from it live; the store is written only
        // when the change is confirmed, and every apply below re-reads it
        // first. That is what makes a rollback actually restore something
        // instead of just removing the pending record.
        auto stored_cellular = [&read_settings, boot_usb_function] {
            cellular::CellularConfig cc;
            KeyValues keys; api::cellular_config_to_settings(cc, keys);
            std::string e;
            if (!api::cellular_config_from_settings(read_settings(keys), cc, e))
                LOGW(MOD, "cellular: %s - using defaults", e.c_str());
            // Ob Mobilfunk laeuft, steht NICHT in der Mobilfunkkonfiguration.
            // Es haengt daran, was der Boot-Helfer geladen hat -- und zwar am
            // GESTARTETEN Modus, nicht am gespeicherten. Wer gerade auf
            // cellular umgestellt hat, hat noch keine Modem-Treiber im Kernel;
            // hier trotzdem loszulaufen hiesse, einen AT-Port zu suchen, den
            // niemand angelegt hat, und "kein Modem" zu melden statt "Neustart
            // erforderlich".
            cc.enabled = (boot_usb_function == usb::UsbFunction::Cellular);
            return cc;
        };
        {
            const cellular::CellularConfig cc = stored_cellular();
            cell_uplink.set_config(cc);
            // The APN is printed; the PIN and the password are not, here or
            // anywhere else.
            const std::string apn_note =
                (cc.enabled && !cc.apn.empty()) ? (", apn " + cc.apn) : std::string();
            LOGI(MOD, "cellular: %s%s", cc.enabled ? "enabled" : "off", apn_note.c_str());
        }
        conn.add(&cell_uplink);

        // The AT port is found, not configured.
        //
        // ttyUSB numbering is not stable: it depends on the order the kernel
        // enumerated the interfaces, and the modem re-enumerates on its own
        // whenever its USB composition changes. Pinning /dev/ttyUSB3 in a
        // config file works until the first time it does not, and then it
        // looks like a dead modem. Rediscovery is cheap and only runs while
        // the port we have is gone.
        auto rediscover_modem_port = [&modem_at, &cell_ppp, want_ppp] {
            if (modem_at.available()) return;
            const cellular::ModemPorts p =
                cellular::map_modem_ports(linuxsys::scan_usb_serial_ports());
            if (p.at.empty() || p.at == modem_at.device()) return;
            LOGI(MOD, "cellular: AT port is %s", p.at.c_str());
            modem_at.set_device(p.at);
            // Der DATEN-Port ist ein anderer als der AT-Port: MI_03 spricht AT,
            // MI_04 traegt PPP. Beide aus derselben Zuordnung, beide nicht
            // hartkodiert -- nach einer Re-Enumeration heissen sie anders, und
            // ein festes /dev/ttyUSB4 waere genau so lange richtig, bis es das
            // nicht mehr ist.
            if (want_ppp) {
                if (p.modem.empty())
                    LOGW(MOD, "cellular: no modem port found - PPP has nothing to dial on");
                else
                    LOGI(MOD, "cellular: modem port is %s", p.modem.c_str());
                cell_ppp.set_modem_port(p.modem);
            }
        };

        const net::WifiCapabilities wcaps = wifi.capabilities();
        LOGI(MOD, "wifi at start-up: %s (re-evaluated on every request)",
             wcaps.present
                 ? (wcaps.station_usable() ? "radio present, station mode usable"
                                           : "radio present, no wpa_supplicant control socket")
                 : "no radio yet");

        {
            net::UplinkPolicy p;
            KeyValues keys; api::policy_to_settings(p, keys);
            std::string e;
            if (!api::policy_from_settings(read_settings(keys), p, e))
                LOGW(MOD, "network: %s - using the default order", e.c_str());
            conn.set_policy(p);
        }

        // The damping needs a clock. Without one the manager switches on the
        // first evaluation that disagrees, and with three uplinks in the list
        // a marginal link would keep telling every live RTSP and WebRTC
        // session that the path changed.
        conn.set_clock([] { return (uint32_t)now_ms(); });

        // Routing and DNS, decided in one place.
        //
        // Before AP-M5 three shell scripts decided it independently: the boot
        // script gave eth0 metric 0, udhcpc-wlan 200, udhcpc-cellular 300.
        // That is a preference order hard-coded across three files, and it
        // contradicted the uplink policy -- setting the policy to "cellular
        // first" changed the status page and nothing about where packets went.
        // The scripts keep their metrics as a bootstrap for the window before
        // this has converged; from here on the plan wins.
        linuxsys::LinuxRouteBackend route_backend;
        net::RouteManager route_manager(route_backend);
        auto apply_routes = [&conn, &route_manager] {
            const net::RoutePlan plan =
                net::plan_routes(conn.status(), conn.policy(), conn.active_id());
            const net::ReconcileReport r = route_manager.reconcile(plan);
            if (!r.error.empty())
                LOGW(MOD, "network: %s", r.error.c_str());
            else if (r.changed())
                LOGI(MOD, "network: %d default route(s) installed, %d removed%s",
                     r.added, r.removed, r.dns_written ? ", resolvers updated" : "");
        };

        // The staged-change record lives next to the config, on the same
        // overlay, and IStateStore writes it 0600 -- a WiFi candidate carries
        // a passphrase.
        FileStateStore net_state("/etc/machino/state");
        net::NetworkTxn net_txn(net_state, [&wifi, &cell_uplink, &stored_cellular]
                                           (const std::string& blob) -> Result {
            Json j; std::string e;
            if (!Json::parse(blob, j, e)) return Result::error();
            const Json* kind = j.get("kind");
            const Json* conf = j.get("config");
            if (!kind || !kind->is_string()) return Result::error();

            // EVERY apply starts by putting the cellular configuration back to
            // what is stored, whatever kind of change is being applied.
            //
            // That single rule is what makes a cellular rollback work. There
            // is one confirmed record for the whole network, so the record a
            // cellular change rolls back TO may well be a WiFi one -- and
            // without this line the modem would simply keep the configuration
            // nobody confirmed. Applying the stored configuration is a no-op
            // in every other case, because the store is only written on
            // confirm.
            cell_uplink.set_config(stored_cellular());

            // The baseline. "Whatever the boot scripts set" is a real
            // configuration and restoring it means changing nothing -- the
            // alternative, inventing one, is how a rollback strands a camera.
            if (kind->as_string() == "boot") return Result::ok();

            if (!conf) return Result::error();
            if (kind->as_string() == "cellular") {
                cellular::CellularConfig cc = cell_uplink.config();
                if (!api::cellular_config_from_json(*conf, cc, e)) return Result::error();
                cell_uplink.set_config(cc);
                return Result::ok();
            }
            if (kind->as_string() == "wifi-station") {
                net::WifiStationConfig sc;
                if (!api::wifi_station_from_json(*conf, sc, e)) return Result::error();
                return wifi.start_station(sc);
            }
            if (kind->as_string() == "wifi-ap") {
                net::WifiApConfig ac;
                if (!api::wifi_ap_from_json(*conf, ac, e)) return Result::error();
                return wifi.start_ap(ac);
            }
            return Result::unsupported();
        });
        {
            std::string e;
            // The three outcomes are handled as three, not folded into a
            // bool. An earlier version treated "could not write it" the same
            // as "already there", with a comment claiming the latter was the
            // normal case -- and on the camera the former was true, so every
            // staged network change was refused for a whole release.
            switch (net_txn.seed_confirmed("{\"kind\":\"boot\"}", e)) {
                case net::SeedOutcome::Seeded:
                    LOGI(MOD, "network: known-good baseline seeded");
                    break;
                case net::SeedOutcome::AlreadyPresent:
                    break;                      // the ordinary case on any later boot
                case net::SeedOutcome::Failed:
                    LOGE(MOD, "network: no known-good baseline could be written (%s) - "
                              "staged changes will be REFUSED and rollback is unavailable",
                         e.c_str());
                    break;
            }
            switch (net_txn.recover(e)) {
                case net::RecoverOutcome::RolledBack:
                    LOGW(MOD, "network: %s", e.c_str());
                    break;
                case net::RecoverOutcome::ConfirmedUnusable:
                    LOGE(MOD, "network: %s", e.c_str());
                    break;
                case net::RecoverOutcome::Nothing:
                    break;
            }
        }

        // Der Geraetemanager. Er haelt nur Wissen: alles, was er sagt, liest er
        // im Dateisystem nach (Manifest, /lib/modules, /etc/wireless/usb), und
        // alles, was er aendert, legt er als Absichtsdatei ab. Kein depmod,
        // kein modprobe, kein fork -- siehe die Begruendung an InstallPending.
        //
        // Die Lebensdauer ist Absicht: beide Objekte leben so lange wie der
        // HTTP-Server, der auf sie zeigt. DeviceManager::add nimmt kein
        // Eigentum, deshalb stehen sie hier und nicht in einem Block.
        devices::Aic8800Package aic8800_pkg;
        devices::DeviceManager device_manager;
        device_manager.add(&aic8800_pkg);
        {
            const devices::DeviceStatus st = aic8800_pkg.status();
            LOGI(MOD, "devices: %s = %s%s%s", st.id.c_str(),
                 devices::install_state_name(st.state),
                 st.detail.empty() ? "" : " - ", st.detail.c_str());
        }

        api::NetApiService::Deps nd;
        nd.usb  = &usb_service;
        nd.conn = &conn;
        // Always wired, for the same reason the uplink above is. A null here
        // makes every /api/v1/network/wifi route answer "WiFi is not available
        // on this build" -- which is a statement about the BUILD, and would
        // have been a lie on a camera whose radio simply had not been powered
        // up yet.
        nd.wifi = &wifi;
        nd.devices = &device_manager;
        nd.txn  = &net_txn;
        nd.now_ms = [] { return (uint32_t)now_ms(); };
        nd.save_usb = [&store](const usb::UsbConfig& c, std::string& e) {
            KeyValues kv; api::usb_config_to_settings(c, kv);
            return store.commit(kv, e);
        };
        nd.save_policy = [&store](const net::UplinkPolicy& p, std::string& e) {
            KeyValues kv; api::policy_to_settings(p, kv);
            return store.commit(kv, e);
        };
        nd.cellular = &cell_uplink;
        // A confirmed change is the only thing that writes the store. Writing
        // it when the change was APPLIED would leave the rollback with nothing
        // to go back to -- see the note on ConfirmedFn.
        nd.on_confirmed = [&store, &cell_uplink](const std::string& candidate) {
            Json j; std::string e;
            if (!Json::parse(candidate, j, e)) return;
            const Json* kind = j.get("kind");
            if (!kind || !kind->is_string() || kind->as_string() != "cellular") return;
            KeyValues kv; api::cellular_config_to_settings(cell_uplink.config(), kv);
            if (!store.commit(kv, e))
                LOGE(MOD, "cellular: the confirmed configuration could not be saved (%s) - "
                          "it is live now and will be gone after a reboot", e.c_str());
        };
        api::NetApiService net_api(nd);

        // Transports hear about an uplink switch; the media pipeline never
        // does. The source address and the NAT path change underneath a live
        // socket, so this is a transport concern and not an encoder one.
        conn.subscribe_path_change([](const std::string& from, const std::string& to) {
            LOGI(MOD, "network path: %s -> %s",
                 from.empty() ? "(none)" : from.c_str(), to.empty() ? "(none)" : to.c_str());
        });
        conn.evaluate();
        apply_routes();

        http::ServerConfig hc; hc.bind = cfg.api.bind; hc.port = cfg.api.port;
        hc.upstream_host = cfg.api.upstream_host; hc.upstream_port = cfg.api.upstream_port;
        // Front-door: the Majestic drop-in login gates :80 exactly like
        // Majestic did - the WebUI login IS the camera's system login.
        hc.unsafe = cfg.system.unsafe;
        hc.session_auth = cfg.api.upstream_port > 0 && cfg.api.auth;
        if (hc.session_auth) hc.auth_check = shadow_check;
        LOGI(MOD, "webui session auth: %s", hc.session_auth ? "on (system account)" : "off");
        // /ws/video and /ws/webrtc: hub consumers with their own demand;
        // stream=1 serves from the substream hub when it is configured.
        http::HttpServer httpd(hc, api, bus, &hub, &pipeline, sub_ok ? &sub_hub : nullptr);

        // AP9 OSD. The backend is the software one: it knows the placement
        // arithmetic but cannot draw, so available() is false and
        // /api/v1/osd answers 404 - the honest answer for a build with no
        // overlay path, and the one the stock settings page is written to
        // handle. The image store is fully live regardless, because a logo
        // uploaded now must survive until the drawing backend arrives.
        SoftOsdBackend osd_backend;
        osd::OsdService osd_service(osd_backend, cfg.osd.image_dir);
        osd_service.set_config(cfg.osd);
        {
            std::vector<osd::StreamGeometry> geo;
            geo.push_back({0, stream.width, stream.height, cfg.video.osd});
            if (sub_ok) geo.push_back({1, sub_stream.width, sub_stream.height, cfg.video1.osd});
            osd_service.set_streams(geo);
        }
        httpd.set_osd(&osd_service);

        // AP10: the unclaimed / first-run gate. No key installer is wired, so a
        // key offered during setup is reported as "claimed, key not installed"
        // rather than silently dropped.
        http::SetupGate setup_gate(claim_state, set_root_password, shadow_check, eula_present);
        httpd.set_setup(&setup_gate);
        httpd.set_log_reader(&log_reader);
        httpd.set_net_api(&net_api);

        // AP11 ONVIF. Off by default until it has met a real client; the
        // profiles it advertises are the ones the pipeline actually has, so
        // a client is never handed a stream that does not exist.
        // The HTTP Digest nonce is keyed with this, never with the password:
        // the challenge goes to any unauthenticated caller. No entropy means
        // no Digest, which is the fail-closed answer.
        const std::string onvif_nonce_secret = secure_hex(32);
        if (cfg.onvif.enabled && onvif_nonce_secret.empty())
            LOGW(MOD, "onvif: no entropy for the digest nonce - HTTP Digest stays off");
        onvif::OnvifService onvif_service(cfg.onvif, shadow_check, onvif_nonce_secret);
        {
            onvif::DeviceInfo di;
            di.firmware = MACHINO_VERSION;
            di.model = hwr.platform.model.empty() ? std::string("Machino") : hwr.platform.model;
            di.hardware_id = hwr.board_id;
            onvif_service.set_device(di);
            std::vector<onvif::MediaProfile> mp;
            onvif::MediaProfile main;
            main.token = "main"; main.name = "Main stream";
            main.width = stream.width; main.height = stream.height; main.fps = stream.fps;
            main.bitrate_kbps = stream.bitrate_kbps; main.rtsp_path = cfg.rtsp.path;
            mp.push_back(main);
            if (sub_ok) {
                onvif::MediaProfile sub;
                sub.token = "sub"; sub.name = "Sub stream";
                sub.width = sub_stream.width; sub.height = sub_stream.height; sub.fps = sub_stream.fps;
                sub.bitrate_kbps = sub_stream.bitrate_kbps; sub.rtsp_path = cfg.rtsp.sub_path;
                mp.push_back(sub);
            }
            onvif_service.set_profiles(mp);
            onvif_service.set_ports(cfg.api.port, cfg.rtsp.port);
            // AP12: this camera has no RTC, and without a default route ntpd
            // never reaches a peer - the clock then keeps whatever
            // fake-hwclock restored, which is the time of the last orderly
            // shutdown. SetSystemDateAndTime is the standard way an NVR fixes
            // that; OpenIPC's own answer for a browser is the "Set from
            // browser" button, which is untouched.
            //
            // Plausibility and the no-op-when-already-right rule live in the
            // service; this only performs the step and logs it, because a
            // stepped clock makes every uptime figure before it meaningless
            // and the log must say when that happened.
            onvif_service.set_clock_setter([](int64_t epoch) {
                struct timespec ts;
                ts.tv_sec  = (time_t)epoch;
                ts.tv_nsec = 0;
                const int64_t before = (int64_t)time(nullptr);
                if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
                    LOGW(MOD, "onvif: clock_settime failed (%s)", strerror(errno));
                    return false;
                }
                LOGI(MOD, "onvif: system clock stepped by %lld s (was %lld, now %lld)",
                     (long long)(epoch - before), (long long)before, (long long)epoch);
                return true;
            });
        }
        std::unique_ptr<onvif::DiscoveryServer> wsd;
        if (cfg.onvif.enabled) {
            httpd.set_onvif(&onvif_service);
            // WS-Discovery is what lets a client FIND the camera instead of
            // being told its address. Own socket and own thread, so the HTTP
            // poll loop that carries the live media path is untouched. The
            // endpoint uuid is derived from durable facts, not stored.
            const std::string seed = hwr.board_id + ":" + hwr.platform.model;
            wsd = std::make_unique<onvif::DiscoveryServer>(
                seed,
                "onvif://www.onvif.org/Profile/Streaming onvif://www.onvif.org/type/video_encoder",
                cfg.api.port);
            if (!wsd->start()) LOGW(MOD, "ws-discovery unavailable; clients must be given the address");
        }
        LOGI(MOD, "onvif: %s", cfg.onvif.enabled ? "on" : "off");
        LOGI(MOD, "claim state: %s", setup_gate.unclaimed() ? "UNCLAIMED (serving only the setup flow)" : "claimed");
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

        // The connectivity poll. Its own timer rather than a piggyback on the
        // telemetry one: telemetry can be switched off, and the rollback of an
        // unconfirmed network change must not depend on that. Two seconds is
        // fast enough that the confirmation countdown on the web page does not
        // visibly lag, and slow enough that reading sysfs costs nothing.
        const int net_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (net_tfd >= 0) {
            struct itimerspec its{};
            its.it_interval.tv_sec = 2; its.it_value.tv_sec = 2;
            timerfd_settime(net_tfd, 0, &its, nullptr);
            ev.data.fd = net_tfd; epoll_ctl(ep, EPOLL_CTL_ADD, net_tfd, &ev);
        } else {
            LOGW(MOD, "network: no poll timer - failover and change rollback will not run");
        }

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
            // AP4: the hardware watchdog. majestic armed this and Machino had
            // silently dropped it, so every hang so far needed a human with a
            // power plug. The FEEDER is a separate thread on purpose and it
            // does NOT feed on its own schedule - it feeds only when this loop
            // has advanced since the last feed. A feeder that ignores that
            // guarantees the camera will never recover from a wedged loop.
            LinuxWatchdog wdt_dev;
            WatchdogService wdt(wdt_dev, cfg.watchdog.timeout_s);
            bool wdt_on = false;
            if (cfg.watchdog.enabled) wdt_on = (bool)wdt.start(now_ms());
            else LOGI(MOD, "watchdog: disabled by configuration");
            std::atomic<bool> wdt_quit{false};
            std::thread wdt_feeder;
            if (wdt_on) {
                wdt_feeder = std::thread([&] {
                    // Wake far more often than the feed interval: the tick
                    // itself decides whether anything is due, and a short sleep
                    // keeps shutdown prompt.
                    while (!wdt_quit.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        const int64_t t = now_ms();
                        wdt.tick(t);
                        const WatchdogStats s = wdt.stats(t);
                        RuntimeStats::get().watchdog(s.available, s.enabled, s.timeout_s, s.feeds,
                                                     s.skipped, s.feed_errors, s.health_epoch,
                                                     s.last_feed_age_ms);
                    }
                });
            }
            // Bound the wait so an idle camera still proves it is alive. An
            // idle camera is a healthy camera: nothing here is coupled to
            // frames, sessions or encoders.
            const int loop_wait_ms = wdt_on ? 1000 : -1;

            bool run = true;
            while (run) {
                wdt.heartbeat();                 // THIS is what makes a feed legitimate
                epoll_event out[8];
                int n = epoll_wait(ep, out, 8, loop_wait_ms);
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
                                // AP9: the substream too. mj-settings.js lists
                                // exactly four sections - image, sensor, video0,
                                // video1 - so upstream EXPECTS video1 to be
                                // settable, and the stock Apply is a SIGHUP.
                                // Without this the page would save a sub-stream
                                // change and the Apply it offers would not
                                // deliver it.
                                {
                                    EffectiveStream ns; std::string se;
                                    const bool want = fresh.video1.enabled &&
                                                      effective_sub_stream(fresh.video1, stream, ns, se);
                                    if (fresh.video1.enabled && !want)
                                        LOGW(MOD, "SIGHUP: substream stays off: %s", se.c_str());
                                    std::string ue;
                                    if (want) pipeline.configure_sub(ns, sub_hub, &sub_timer);
                                    const Result ur = pipeline.update_sub_stream(ns, want, ue);
                                    if (!ur) LOGW(MOD, "SIGHUP: substream update failed: %s", ue.c_str());
                                    else     LOGI(MOD, "SIGHUP: substream %s", want ? "applied" : "disabled");
                                }
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
                    } else if (net_tfd >= 0 && out[i].data.fd == net_tfd) {
                        uint64_t x; while (read(net_tfd, &x, sizeof x) > 0) {}
                        // Rollback first. If an unconfirmed change has run out
                        // of time, the selection that follows should see the
                        // restored configuration, not the one being undone.
                        if (net_api.tick())
                            LOGW(MOD, "network: an unconfirmed change was rolled back");
                        // The modem before the selection: tick() is what makes
                        // the ECM link advance, and a selection run on last
                        // tick's state would be one round behind for the whole
                        // bring-up.
                        if (cell_uplink.enabled()) rediscover_modem_port();
                        cell_uplink.tick();
                        conn.evaluate();
                        // Routes AFTER the selection, always -- not only when
                        // evaluate() reported a change. An uplink can get a new
                        // gateway from a DHCP renewal without the ACTIVE uplink
                        // changing at all, and that new gateway still has to
                        // reach the kernel.
                        apply_routes();
                    }
                }
            }
            // Disarm BEFORE the long teardown below. Stopping the encoder and
            // the pipeline can take seconds, and this loop stops heartbeating
            // the moment it exits - without this the watchdog would reset the
            // camera in the middle of an orderly shutdown, which is exactly
            // the reset a restart must not produce.
            wdt_quit.store(true, std::memory_order_release);
            if (wdt_feeder.joinable()) wdt_feeder.join();
            wdt.stop();
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
        if (net_tfd >= 0) close(net_tfd);
        close(ep);
    }

    close(sfd);
    LOGI(MOD, "exit %d", rc);
    return rc;
}
