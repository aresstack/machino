#include "app/http/file_upload.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace machino { namespace http {

bool FileUpload::location_ok(const std::string& loc, std::string& why) {
    if (loc.empty() || loc[0] != '/') { why = "File-Location must be an absolute path"; return false; }
    if (loc.size() > 4096) { why = "File-Location is too long"; return false; }
    if (loc.find("..") != std::string::npos) { why = "File-Location must not contain .."; return false; }
    for (unsigned char ch : loc)
        if (ch < 0x20 || ch == 0x7f) { why = "File-Location contains a control character"; return false; }
    if (loc.size() == 1 || loc.back() == '/') { why = "File-Location must name a file, not a directory"; return false; }
    static const char* const refused[] = { "/proc", "/sys", "/dev", "/rom", "/overlay" };
    for (const char* r : refused) {
        const size_t n = strlen(r);
        if (loc.compare(0, n, r) == 0 && (loc.size() == n || loc[n] == '/')) {
            why = std::string("no uploads under ") + r;
            return false;
        }
    }
    return true;
}

bool FileUpload::begin(const std::string& location, size_t length, int& status, std::string& msg) {
    abort();
    std::string why;
    if (!location_ok(location, why)) { status = 400; msg = why; return false; }
    const size_t slash = location.rfind('/');
    const std::string parent = slash == 0 ? std::string("/") : location.substr(0, slash);
    const std::string base = location.substr(slash + 1);
    struct stat st;
    if (stat(parent.c_str(), &st) != 0) { status = 404; msg = "no such directory: " + parent; return false; }
    if (!S_ISDIR(st.st_mode)) { status = 404; msg = "not a directory: " + parent; return false; }
    if (stat(location.c_str(), &st) == 0 && !S_ISREG(st.st_mode)) {
        status = 409; msg = "not a regular file: " + location; return false;
    }
    struct statvfs vf;
    if (statvfs(parent.c_str(), &vf) == 0) {
        const unsigned long long avail = (unsigned long long)vf.f_bavail * vf.f_frsize;
        const unsigned long long need = (unsigned long long)length + 64 * 1024;
        if (avail < need) {
            status = 507;
            msg = "not enough space on " + parent + ": need " + std::to_string(need / 1024) + " kB, free " +
                  std::to_string(avail / 1024) + " kB";
            return false;
        }
    }
    tmp_ = (parent == "/" ? std::string() : parent) + "/." + base + ".machino-up";
    unlink(tmp_.c_str());                       // a stale one from an interrupted run
    fd_ = open(tmp_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        status = 500; msg = "cannot create " + tmp_ + ": " + strerror(errno);
        tmp_.clear();
        return false;
    }
    path_ = location;
    left_ = total_ = length;
    return true;
}

bool FileUpload::feed(const char* p, size_t n, int& status, std::string& msg) {
    if (fd_ < 0) { status = 500; msg = "upload is not open"; return false; }
    if (n > left_) n = left_;
    while (n > 0) {
        const ssize_t w = write(fd_, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            const int e = errno;
            status = (e == ENOSPC) ? 507 : 500;
            msg = std::string(e == ENOSPC ? "no space left on the target while writing " : "write failed for ") +
                  path_ + ": " + strerror(e);
            abort();
            return false;
        }
        p += w; n -= (size_t)w; left_ -= (size_t)w;
    }
    return true;
}

bool FileUpload::finish(int& status, std::string& msg) {
    if (fd_ < 0) { status = 500; msg = "upload is not open"; return false; }
    if (left_ != 0) { status = 400; msg = "upload ended " + std::to_string(left_) + " bytes short"; abort(); return false; }
    close(fd_); fd_ = -1;
    if (rename(tmp_.c_str(), path_.c_str()) != 0) {
        const int e = errno;
        unlink(tmp_.c_str()); tmp_.clear();
        status = 500; msg = "cannot move the upload onto " + path_ + ": " + strerror(e);
        return false;
    }
    tmp_.clear();
    return true;
}

void FileUpload::abort() {
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    if (!tmp_.empty()) { unlink(tmp_.c_str()); tmp_.clear(); }
    left_ = 0;
}

}} // namespace machino::http
