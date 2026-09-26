// AP-NNA3: the machinod half of the machino-nna helper. What matters here is
// the contract, not the model: the helper is spawned with the right arguments,
// "ready" gates inference, one frame line yields one parsed result, a dead or
// silent helper degrades to timeouts/errors and a respawn after backoff --
// and none of it ever blocks start() on a model load.
#include "core/detection/nna_detector.hpp"

#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace machino;
using namespace machino::detection;

extern int g_fail_ext, g_pass_ext;
#define NCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

struct FakeProcess : INnaProcess {
    bool spawn_ok = true;
    bool is_alive = false;
    int  spawns = 0;
    std::vector<std::string> last_argv;
    std::vector<std::string> written;
    std::deque<std::string>  lines;      // scripted stdout

    bool spawn(const std::vector<std::string>& argv) override {
        ++spawns;
        last_argv = argv;
        if (!spawn_ok) return false;
        is_alive = true;
        return true;
    }
    bool alive() override { return is_alive; }
    bool write_line(const std::string& l) override { if (!is_alive) return false; written.push_back(l); return true; }
    bool read_line(std::string& out, int) override {
        if (lines.empty()) return false;
        out = lines.front(); lines.pop_front();
        return true;
    }
    void terminate() override { is_alive = false; }
};

struct FakeSource : IAnalysisSource {
    int started = 0, stopped = 0, releases = 0;
    bool give_frames = true;
    std::vector<uint8_t> pixels = std::vector<uint8_t>(64 * 36 * 3 / 2, 0x5a);

    Result start() override { ++started; return Result::ok(); }
    Result stop()  override { ++stopped; return Result::ok(); }
    Result get(AnalysisFrame& out, int) override {
        if (!give_frames) return Result::timeout();
        out.data = pixels.data(); out.size = pixels.size();
        out.width = 64; out.height = 36; out.stride = 64; out.pts_us = 111222;
        return Result::ok();
    }
    void release() override { ++releases; }
    int width()  const override { return 64; }
    int height() const override { return 36; }
};

struct Rig {
    FakeProcess proc;
    FakeSource  src;
    int64_t clock = 1000;
    NnaDetectorConfig cfg;
    Rig() {
        cfg.helper_path = "/usr/sbin/machino-nna";
        cfg.model_path  = "/etc/machino/models/persondet.bin";
        cfg.frame_path  = "tests/tmp-nna-frame.bin";
        cfg.inference_fps = 5;
    }
    NnaDetector make() {
        return NnaDetector(proc, src, cfg, [this] { return clock; });
    }
};

void test_spawn_ready_and_one_detection() {
    Rig r;
    NnaDetector d = r.make();
    NCHECK(d.start());
    // start() spawned but did not wait: source running, helper loading.
    NCHECK(r.src.started == 1);
    NCHECK(r.proc.spawns == 1);
    NCHECK(r.proc.last_argv.size() == 7);
    NCHECK(r.proc.last_argv[0] == "/usr/sbin/machino-nna");
    NCHECK(r.proc.last_argv[2] == "/etc/machino/models/persondet.bin");
    NCHECK(r.proc.last_argv[4] == "64");

    DetectionResult out;
    // Still loading: quiet.
    NCHECK(d.poll(out, 10).status == Status::Timeout);
    // Helper reports ready; the ready-consuming poll is still resultless.
    r.proc.lines.push_back("ready venus-1.0\n");
    NCHECK(d.poll(out, 10).status == Status::Timeout);

    // One frame -> one result with one person.
    r.proc.lines.push_back("result 111222 1\n");
    r.proc.lines.push_back("det 0 87 0.25 0.30 0.20 0.40 person\n");
    Result pr = d.poll(out, 10);
    NCHECK(pr);
    NCHECK(out.frame_pts_us == 111222);
    NCHECK(out.detections.size() == 1);
    if (out.detections.size() == 1) {
        NCHECK(out.detections[0].label == "person");
        NCHECK(out.detections[0].confidence == 87);
        NCHECK(out.detections[0].class_id == 0);
        NCHECK(out.detections[0].box.w > 0.19f && out.detections[0].box.w < 0.21f);
    }
    // The IMP buffer went back, and the frame line named geometry and path.
    NCHECK(r.src.releases == 1);
    NCHECK(!r.proc.written.empty());
    NCHECK(r.proc.written.back().rfind("frame 111222 64 36 64 ", 0) == 0);
    NCHECK(r.proc.written.back().find("tests/tmp-nna-frame.bin") != std::string::npos);

    // The frame file carries the source's bytes.
    FILE* f = fopen(r.cfg.frame_path.c_str(), "rb");
    NCHECK(f != nullptr);
    if (f) {
        uint8_t b = 0;
        NCHECK(fread(&b, 1, 1, f) == 1 && b == 0x5a);
        fclose(f);
        remove(r.cfg.frame_path.c_str());
    }
    d.stop();
    NCHECK(r.src.stopped == 1);
    NCHECK(!r.proc.is_alive);
}

void test_pacing_holds_the_inference_rate() {
    Rig r;
    r.cfg.inference_fps = 5;   // 200 ms period
    NnaDetector d = r.make();
    d.start();
    r.proc.lines.push_back("ready\n");
    DetectionResult out;
    d.poll(out, 10);                                     // consumes ready
    r.proc.lines.push_back("result 1 0\n");
    NCHECK(d.poll(out, 10));                             // first inference at t
    // 100 ms later: not due -- no frame is taken, no line written.
    const size_t frames_before = r.proc.written.size();
    r.clock += 100;
    NCHECK(d.poll(out, 10).status == Status::Timeout);
    NCHECK(r.proc.written.size() == frames_before);
    // 200 ms after the first: due again.
    r.clock += 100;
    r.proc.lines.push_back("result 2 0\n");
    NCHECK(d.poll(out, 10));
    NCHECK(d.skipped() == 0);                            // im Takt: nichts ausgelassen
    NCHECK(out.infer_duration_ms == 0);                  // gemessen (gefrorene Testuhr)

    // Weit hinter dem Takt (5 Perioden): newest wins, kein Burst -- die
    // verpassten Perioden werden als skipped GEZAEHLT, nicht nachgeholt.
    r.clock += 1000;
    r.proc.lines.push_back("result 3 0\n");
    NCHECK(d.poll(out, 10));
    NCHECK(d.skipped() >= 4);
    const size_t frames_after = r.proc.written.size();
    r.proc.lines.push_back("result 4 0\n");
    NCHECK(d.poll(out, 10).status == Status::Timeout);   // und direkt danach: nicht faellig
    NCHECK(r.proc.written.size() == frames_after);
}

void test_dead_helper_respawns_after_backoff_video_never_involved() {
    Rig r;
    NnaDetector d = r.make();
    d.start();
    r.proc.lines.push_back("ready\n");
    DetectionResult out;
    d.poll(out, 10);

    // Helper dies mid-inference: this cycle fails, nothing crashes.
    r.proc.is_alive = false;
    NCHECK(!d.poll(out, 10));
    // Within the backoff: quiet timeouts, no respawn hammering.
    r.clock += 1000;
    NCHECK(d.poll(out, 10).status == Status::Timeout);
    NCHECK(r.proc.spawns == 1);
    // After the backoff: respawn, load, ready, working again.
    r.clock += 5000;
    NCHECK(d.poll(out, 10).status == Status::Timeout);   // respawned, loading
    NCHECK(r.proc.spawns == 2);
    r.proc.lines.push_back("ready\n");
    d.poll(out, 10);
    r.clock += 1000;
    r.proc.lines.push_back("result 3 0\n");
    NCHECK(d.poll(out, 10));
}

void test_load_error_and_ready_timeout_fail_closed() {
    Rig r;
    NnaDetector d = r.make();
    d.start();
    DetectionResult out;
    // The helper says why it cannot: that is an error, and the helper is down.
    r.proc.lines.push_back("error model file unreadable\n");
    NCHECK(!d.poll(out, 10));
    NCHECK(!r.proc.is_alive);

    // Respawn after backoff, then never says ready: bounded, then error.
    r.clock += 6000;
    d.poll(out, 10);                                     // respawn -> loading
    r.clock += r.cfg.ready_timeout_ms + 1;
    NCHECK(!d.poll(out, 10));
    NCHECK(!r.proc.is_alive);
}

void test_malformed_lines_never_crash() {
    Rig r;
    NnaDetector d = r.make();
    d.start();
    r.proc.lines.push_back("ready\n");
    DetectionResult out;
    d.poll(out, 10);

    // Garbage instead of a result line: a failed cycle, nothing more.
    r.proc.lines.push_back("resu!t nonsense\n");
    NCHECK(!d.poll(out, 10));

    // A malformed det among valid ones is skipped; the count keeps us in sync.
    r.clock += 1000;
    r.proc.lines.push_back("result 9 3\n");
    r.proc.lines.push_back("det 0 87 0.1 0.1 0.2 0.2 person\n");
    r.proc.lines.push_back("det 0 999 0.0 0.0 0.0 0.0\n");           // bad conf, bad box
    r.proc.lines.push_back("det 1 55 0.5 0.5 0.3 0.3 bicycle\n");
    NCHECK(d.poll(out, 10));
    NCHECK(out.detections.size() == 2);

    Detection dd;
    NCHECK(!parse_det_line("det x y z\n", dd));
    NCHECK(!parse_det_line("det 0 50 1.5 0.1 0.2 0.2 offscreen\n", dd));
    NCHECK(parse_det_line("det 3 42 0.0 0.0 1.0 1.0 whole frame label\n", dd));
    NCHECK(dd.label == "whole frame label");
}

void test_missing_helper_is_not_installed_not_a_crash() {
    Rig r;
    r.proc.spawn_ok = false;
    NnaDetector d = r.make();
    NCHECK(d.start());                                   // source runs; helper absent
    DetectionResult out;
    NCHECK(!d.poll(out, 10));                            // says so once
    r.clock += 1000;
    NCHECK(d.poll(out, 10).status == Status::Timeout);   // then waits out the backoff
    // Payload installed later, no machinod restart: next window just works.
    r.clock += 6000;
    r.proc.spawn_ok = true;
    NCHECK(d.poll(out, 10).status == Status::Timeout);   // spawns, loading
    NCHECK(r.proc.spawns >= 2);
}

} // namespace

void run_nna_detector_tests() {
    test_spawn_ready_and_one_detection();
    test_pacing_holds_the_inference_rate();
    test_dead_helper_respawns_after_backoff_video_never_involved();
    test_load_error_and_ready_timeout_fail_closed();
    test_malformed_lines_never_crash();
    test_missing_helper_is_not_installed_not_a_crash();
}
