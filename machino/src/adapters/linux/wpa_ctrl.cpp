#include "adapters/linux/wpa_ctrl.hpp"
#include "ports/inetwork.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace machino { namespace linuxsys {

WpaCtrl::WpaCtrl(std::string iface_path) : iface_path_(std::move(iface_path)) {}

WpaCtrl::~WpaCtrl() { close(); }

bool WpaCtrl::available() const
{
    struct stat st;
    return ::stat(iface_path_.c_str(), &st) == 0;
}

Result WpaCtrl::ensure_open()
{
    if (fd_ >= 0) return Result::ok();
    if (!available()) return Result::unsupported();

    int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return Result::error(errno);

    // Our end needs a name so wpa_supplicant can reply. The pid alone is not
    // enough: two WpaCtrl objects in one process -- two radios, or a retry
    // while the first is still open -- would bind the same path and the second
    // bind would fail. A per-object counter makes the name unique.
    static int seq = 0;
    char ours[128];
    std::snprintf(ours, sizeof(ours), "/tmp/machino-wpa-%d-%d", (int)::getpid(), seq++);
    ::unlink(ours);

    struct sockaddr_un local;
    std::memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;
    std::snprintf(local.sun_path, sizeof(local.sun_path), "%s", ours);
    if (::bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
        int e = errno; ::close(fd); return Result::error(e);
    }

    struct sockaddr_un remote;
    std::memset(&remote, 0, sizeof(remote));
    remote.sun_family = AF_UNIX;
    std::snprintf(remote.sun_path, sizeof(remote.sun_path), "%s", iface_path_.c_str());
    if (::connect(fd, (struct sockaddr*)&remote, sizeof(remote)) < 0) {
        int e = errno; ::close(fd); ::unlink(ours); return Result::error(e);
    }

    fd_ = fd;
    local_path_ = ours;
    return Result::ok();
}

void WpaCtrl::close()
{
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    if (!local_path_.empty()) { ::unlink(local_path_.c_str()); local_path_.clear(); }
}

Result WpaCtrl::request(const std::string& cmd, std::string& reply, int timeout_ms)
{
    Result rc = ensure_open();
    if (!rc.is_ok()) return rc;

    if (::send(fd_, cmd.data(), cmd.size(), 0) < 0) {
        // errno first: close() calls ::close and ::unlink, either of which may
        // overwrite it, and we would then report the wrong reason.
        const int e = errno;
        close();                          // the supplicant probably restarted
        return Result::error(e);
    }

    // wpa_supplicant also pushes unsolicited events (lines starting with '<')
    // down this socket. Skip them until the actual answer arrives, otherwise a
    // scan notification would be mistaken for the scan results.
    const int deadline_slices = timeout_ms > 0 ? timeout_ms : 1;
    int waited = 0;
    while (waited < deadline_slices) {
        struct pollfd p;
        p.fd = fd_; p.events = POLLIN; p.revents = 0;
        const int slice = 100;
        int n = ::poll(&p, 1, slice);
        waited += slice;
        if (n < 0) { if (errno == EINTR) continue; return Result::error(errno); }
        if (n == 0) continue;

        char buf[4096];
        ssize_t got = ::recv(fd_, buf, sizeof(buf) - 1, 0);
        if (got <= 0) continue;
        buf[got] = 0;
        if (buf[0] == '<') continue;      // unsolicited event
        reply.assign(buf, (size_t)got);
        return Result::ok();
    }
    return Result::timeout();
}

bool WpaCtrl::ok_request(const std::string& cmd, int timeout_ms)
{
    std::string r;
    if (!request(cmd, r, timeout_ms).is_ok()) return false;
    while (!r.empty() && (r.back() == '\n' || r.back() == '\r')) r.pop_back();
    return r == "OK";
}

}} // namespace machino::linuxsys
