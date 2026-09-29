// machinoctl -- die Linux-Seite: Loopback-Socket zu machinod, stdin/stdout,
// verdeckte Eingabe ueber termios. Nur im Cross-Build (SRC), nicht in den
// Hosttests: dort steht der Fake-Client an dieser Stelle.
#include "app/ctl/machinoctl.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

namespace machino { namespace ctl {

namespace {

// Eine Antwort darf gross sein (config.schema.json), aber nicht unbegrenzt:
// die Kamera hat ~20 MB frei, und dieser Prozess laeuft NEBEN dem Daemon.
constexpr size_t kMaxReply = 4u * 1024u * 1024u;

bool send_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        off += (size_t)n;
    }
    return true;
}

} // namespace

bool LoopbackHttpClient::request(const std::string& method, const std::string& path,
                                 const std::string& body, HttpReply& out, std::string& err)
{
    std::string last;
    for (int port : ports_) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { err = std::string("socket: ") + strerror(errno); return false; }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (const sockaddr*)&a, sizeof a) != 0) {
            last = std::to_string(port) + ": " + strerror(errno);
            close(fd);
            continue;
        }
        // connect() im Daemon loest DNS auf und startet weirdiked -- das darf
        // Sekunden dauern. Danach ist Schluss, ein haengender Daemon blockiert
        // die Konsole nicht ewig.
        timeval tv{}; tv.tv_sec = 30;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        tv.tv_sec = 10;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        std::string req = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                          "User-Agent: machinoctl\r\nAccept: application/json\r\nConnection: close\r\n";
        // Auch ein GET mit Body (`api get <pfad> <json>`) traegt Content-Length:
        // ohne ihn staenden die Body-Bytes als Muell hinter dem Request.
        if (method != "GET" || !body.empty()) {
            req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
        }
        req += "\r\n";
        req += body;
        if (!send_all(fd, req)) { err = std::string("senden: ") + strerror(errno); close(fd); return false; }

        std::string raw;
        char buf[8192];
        int last_err = 0;
        for (;;) {
            const ssize_t n = recv(fd, buf, sizeof buf, 0);
            if (n < 0) { if (errno == EINTR) continue; last_err = errno; break; }
            if (n == 0) break;
            raw.append(buf, (size_t)n);
            if (raw.size() > kMaxReply) { err = "Antwort groesser als " + std::to_string(kMaxReply) + " Bytes"; close(fd); return false; }
        }
        close(fd);
        if (raw.empty()) {
            err = (last_err == EAGAIN || last_err == EWOULDBLOCK)
                    ? "machinod antwortet nicht (Zeitueberschreitung auf 127.0.0.1:" + std::to_string(port) + ")"
                    : "leere Antwort von 127.0.0.1:" + std::to_string(port) + (last_err ? std::string(" (") + strerror(last_err) + ")" : "");
            return false;
        }
        used_ = port;
        return parse_http_reply(raw, out, err);
    }
    std::string ports;
    for (size_t i = 0; i < ports_.size(); ++i) { if (i) ports += "/"; ports += std::to_string(ports_[i]); }
    err = "machinod nicht erreichbar auf 127.0.0.1:" + ports + " (" + last + ") -- laeuft Machino? (streamerctl status)";
    return false;
}

namespace {

bool read_stdin_line(std::string& out) {
    out.clear();
    char buf[512];
    bool any = false;
    while (fgets(buf, sizeof buf, stdin)) {
        any = true;
        out += buf;
        if (!out.empty() && out.back() == '\n') break;
    }
    if (!any) return false;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return true;
}

// Verdeckte Eingabe: Echo aus, solange stdin ein Terminal ist (UART, SSH);
// aus einer Pipe wird die Zeile einfach gelesen.
std::string read_secret(const std::string& prompt) {
    termios old{};
    const bool tty = isatty(0) && tcgetattr(0, &old) == 0;
    // Der Prompt gehoert zum Terminal; eine Pipe bekommt keinen (er stuende
    // sonst ohne Zeilenende vor der naechsten Meldung).
    if (tty) { fputs(prompt.c_str(), stderr); fflush(stderr); }
    if (tty) { termios t = old; t.c_lflag &= ~(tcflag_t)ECHO; tcsetattr(0, TCSAFLUSH, &t); }
    std::string s;
    const bool ok = read_stdin_line(s);
    if (tty) { tcsetattr(0, TCSAFLUSH, &old); fputs("\n", stderr); }
    return ok ? s : std::string();
}

} // namespace

int run_ctl(int argc, char** argv, int first)
{
    std::vector<int> ports;
    bool raw = false;
    std::vector<std::string> args;
    for (int i = first; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) {
            const int p = atoi(argv[++i]);
            if (p < 1 || p > 65535) { fprintf(stderr, "machinoctl: --port 1..65535\n"); return 2; }
            ports = {p};
        } else if (a == "--json") {
            raw = true;
        } else if ((a == "--help" || a == "-h") && args.empty()) {
            fputs(help_text(), stdout);
            return 0;
        } else {
            args.push_back(a);
        }
    }
    if (ports.empty()) {
        const char* e = getenv("MACHINOCTL_PORT");
        const int p = (e && *e) ? atoi(e) : 0;
        if (p >= 1 && p <= 65535) ports = {p};
        else ports = {80, 8080};   // Front-Door (Machino aktiv), sonst api.port
    }
    LoopbackHttpClient http(ports);
    Console c{
        http,
        [](const std::string& s) { fputs(s.c_str(), stdout); fflush(stdout); },
        [](const std::string& s) { fputs(s.c_str(), stderr); fflush(stderr); },
        read_stdin_line,
        read_secret,
        raw,
        isatty(0) != 0,
    };
    if (args.empty()) return run_repl(c);
    return run_command(c, args);
}

}} // namespace machino::ctl
