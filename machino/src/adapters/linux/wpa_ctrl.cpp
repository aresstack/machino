#include "adapters/linux/wpa_ctrl.hpp"
#include "ports/inetwork.hpp"

#include <cstdio>
#include <cstdlib>
#include <cerrno>
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
    struct sockaddr_un local;
    std::memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;

    // Straight into sun_path, and CHECKED. sun_path is 108 bytes on Linux; an
    // earlier version formatted into a 128-byte buffer first and copied that
    // in, so a long path was silently truncated. A truncated unix socket
    // address is not a clean failure -- it names a different socket, and the
    // bind or connect then succeeds against the wrong thing or fails with an
    // error that points nowhere near the cause.
    static int seq = 0;
    const int n = std::snprintf(local.sun_path, sizeof(local.sun_path),
                                "/tmp/machino-wpa-%d-%d", (int)::getpid(), seq++);
    if (n < 0 || (size_t)n >= sizeof(local.sun_path)) {
        ::close(fd);
        return Result::error(ENAMETOOLONG);
    }
    const std::string ours = local.sun_path;
    ::unlink(ours.c_str());

    if (::bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
        int e = errno; ::close(fd); return Result::error(e);
    }

    struct sockaddr_un remote;
    std::memset(&remote, 0, sizeof(remote));
    remote.sun_family = AF_UNIX;
    // Same check for the far end. A control directory deeper than 107 bytes is
    // unusual but perfectly legal, and connecting to a truncated path would
    // reach whatever happens to be at the shorter name.
    if (iface_path_.size() >= sizeof(remote.sun_path)) {
        ::close(fd); ::unlink(ours.c_str());
        return Result::error(ENAMETOOLONG);
    }
    std::memcpy(remote.sun_path, iface_path_.data(), iface_path_.size());
    if (::connect(fd, (struct sockaddr*)&remote, sizeof(remote)) < 0) {
        int e = errno; ::close(fd); ::unlink(ours.c_str()); return Result::error(e);
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
