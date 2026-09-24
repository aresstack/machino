#include "adapters/linux/at_transport.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

namespace machino { namespace linuxsys {

namespace {

uint64_t now_ms()
{
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

// Roher 8N1-Modus ohne Flusskontrolle und ohne jede Zeichenumsetzung.
//
// Die Baudrate ist an einem USB-CDC-Port Fassade -- es gibt keine Leitung, auf
// der sie etwas bedeutet -- aber termios will eine, und 115200 ist das, was in
// jeder Anleitung zu diesem Modem steht.
//
// Wichtig ist der Rest: ohne ~ICANON kommt jedes Byte sofort statt erst bei
// Zeilenende, und ohne ~ONLCR/~ICRNL bleibt ein CR ein CR. Ein AT-Kommando ist
// per Definition CR-terminiert; wuerde die Leitungsdisziplin daraus CRLF
// machen, antwortete das Modem auf ein Kommando, das wir nicht geschickt haben.
bool make_raw(int fd)
{
    struct termios t;
    if (::tcgetattr(fd, &t) != 0) return false;

    ::cfmakeraw(&t);
    ::cfsetispeed(&t, B115200);
    ::cfsetospeed(&t, B115200);

    t.c_cflag |= (CLOCAL | CREAD);          // kein Modemsteuersignal abwarten
    t.c_cflag &= (tcflag_t)~CRTSCTS;        // keine Hardware-Flusskontrolle
    t.c_cflag &= (tcflag_t)~CSTOPB;
    t.c_cflag &= (tcflag_t)~PARENB;
    t.c_cc[VMIN]  = 0;                      // nie blockieren; poll() entscheidet
    t.c_cc[VTIME] = 0;

    return ::tcsetattr(fd, TCSANOW, &t) == 0;
}

} // namespace

Result AtTransport::send(const std::string& cmd, AtReply& out, int timeout_ms)
{
    out = AtReply{};

    const int fd = ::open(device_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        // ENOENT/ENXIO heisst hier fast immer: das Modem ist weg. Das ist ein
        // Ergebnis, kein Fehler, auf den jemand warten muesste.
        out.device_gone = (errno == ENOENT || errno == ENXIO || errno == ENODEV);
        return Result::error(errno);
    }

    if (!make_raw(fd)) {
        const int e = errno;
        ::close(fd);
        return Result::error(e);
    }

    // Reste einer vorherigen Sitzung wegwerfen, sonst liest das erste Kommando
    // die Antwort des letzten.
    ::tcflush(fd, TCIOFLUSH);

    const std::string line = cmd + "\r";
    size_t written = 0;
    const uint64_t deadline = now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);

    while (written < line.size()) {
        const ssize_t n = ::write(fd, line.data() + written, line.size() - written);
        if (n > 0) { written += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            if (now_ms() >= deadline) { ::close(fd); out.timed_out = true; return Result::timeout(); }
            struct pollfd pfd = {fd, POLLOUT, 0};
            ::poll(&pfd, 1, 50);
            continue;
        }
        const int e = errno;
        out.device_gone = (e == EIO || e == ENODEV || e == ENXIO);
        ::close(fd);
        return Result::error(e);
    }

    for (;;) {
        const uint64_t now = now_ms();
        if (now >= deadline) { out.timed_out = true; break; }

        struct pollfd pfd = {fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, (int)(deadline - now));
        if (pr < 0) {
            if (errno == EINTR) continue;
            const int e = errno;
            ::close(fd);
            return Result::error(e);
        }
        if (pr == 0) { out.timed_out = true; break; }

        // POLLHUP/POLLERR ohne Daten: der Port ist verschwunden, waehrend wir
        // gewartet haben. Weiterzupollen wuerde bis zum Timeout dauern und
        // dann dieselbe Antwort geben, nur spaeter.
        if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) && !(pfd.revents & POLLIN)) {
            out.device_gone = true;
            break;
        }

        char buf[512];
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            out.raw.append(buf, (size_t)n);
            out.result = cellular::at_scan(out.raw);
            if (out.result != cellular::AtResult::Pending) break;
            continue;
        }
        if (n == 0) continue;                       // nichts da, poll erneut
        if (errno == EAGAIN || errno == EINTR) continue;
        out.device_gone = (errno == EIO || errno == ENODEV || errno == ENXIO);
        break;
    }

    ::close(fd);
    out.payload = cellular::at_payload(out.raw, cmd);

    if (out.device_gone) return Result::error(EIO);
    if (out.result == cellular::AtResult::Pending) return Result::timeout();
    return Result::ok();      // auch bei AtResult::Error: die ANTWORT kam an,
                              // was drinsteht, entscheidet der Aufrufer
}

}} // namespace machino::linuxsys
