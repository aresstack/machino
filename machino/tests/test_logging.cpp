// AP5: the log file sink must be usable for diagnostics AND bounded, because
// it lives on a small overlay. This exercises the shipped sink (the same
// translation unit the daemon links), not a test double.
#include "core/log.hpp"
#include "core/log_file.hpp"
#include "core/runtime_stats.hpp"
#include <cstdio>
#include <string>

using namespace machino;
extern int g_fail_ext, g_pass_ext;
#define LCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

const char* PATH = "tests/tmp_machino_log.txt";

long file_size(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fclose(f);
    return n;
}

std::string read_all(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return "";
    std::string out; char buf[1024]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

void cleanup() {
    log_file_close();
    remove(PATH);
    remove((std::string(PATH) + ".1").c_str());
}

} // namespace

void run_logging_tests() {
    cleanup();

    // no path configured: writing must be a silent no-op, never a crash and
    // never a stray file (a daemon with no log target still has to run)
    log_file_open("", 0);
    log_file_write("nothing should happen\n");
    LCHECK(file_size(PATH) == -1);

    // lines land in the file, in order, and the size is tracked
    log_file_open(PATH, 0);
    log_file_write("first line\n");
    log_file_write("second line\n");
    const std::string body = read_all(PATH);
    LCHECK(body == "first line\nsecond line\n");
    LCHECK(log_file_bytes() == body.size());

    // rotation: with a small cap the live file must stay bounded and exactly
    // one previous generation must survive - this is what stops a long-running
    // camera from filling its overlay
    cleanup();
    const size_t CAP = 512;
    log_file_open(PATH, CAP);
    std::string line(64, 'x');
    line += "\n";
    for (int i = 0; i < 200; ++i) log_file_write(line.c_str());
    const long live = file_size(PATH);
    const long prev = file_size(std::string(PATH) + ".1");
    LCHECK(live >= 0);
    LCHECK(prev >= 0);                                   // a generation was kept
    LCHECK((size_t)live < CAP + line.size());            // live file bounded by the cap
    LCHECK((size_t)prev < CAP + line.size());            // and so is the kept one
    // 200 * 65 bytes were written; without rotation the file would be ~13 KB
    LCHECK(live + prev < (long)(2 * (CAP + line.size())));

    // the newest line is in the LIVE file, so a reader tailing it sees current
    // events rather than history
    log_file_write("newest marker\n");
    LCHECK(read_all(PATH).find("newest marker") != std::string::npos);

    // reopening starts a fresh accounting but keeps what is on disk
    log_file_close();
    log_file_open(PATH, 0);
    log_file_write("after reopen\n");
    const std::string after = read_all(PATH);
    LCHECK(after.find("newest marker") != std::string::npos);
    LCHECK(after.find("after reopen") != std::string::npos);

    cleanup();

    // runtime counters: gauges go up and down, counters only up. The
    // diagnostics must never be the reason a number looks wrong.
    RuntimeStats& rs = RuntimeStats::get();
    const int rtsp0 = rs.rtsp_sessions.load();
    RuntimeStats::inc(rs.rtsp_sessions);
    RuntimeStats::inc(rs.rtsp_sessions);
    LCHECK(rs.rtsp_sessions.load() == rtsp0 + 2);
    RuntimeStats::dec(rs.rtsp_sessions);
    LCHECK(rs.rtsp_sessions.load() == rtsp0 + 1);
    RuntimeStats::dec(rs.rtsp_sessions);
    LCHECK(rs.rtsp_sessions.load() == rtsp0);

    const uint64_t pkts0 = rs.webrtc_rtp_packets.load();
    RuntimeStats::inc(rs.webrtc_rtp_packets);
    RuntimeStats::inc(rs.webrtc_rtp_bytes, 1400);
    LCHECK(rs.webrtc_rtp_packets.load() == pkts0 + 1);
    LCHECK(rs.webrtc_rtp_bytes.load() >= 1400);

    // the singleton really is one instance
    LCHECK(&RuntimeStats::get() == &rs);
}
