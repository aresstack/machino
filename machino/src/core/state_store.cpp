#include "core/state_store.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#  include <cerrno>
#  include <direct.h>
#  include <io.h>
#  include <sys/stat.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace machino {

namespace {

// mkdir -p, 0700. The records here can carry a WiFi passphrase, so the
// directory is no more readable than the files in it.
//
// This exists because it was missing. On the camera /etc/machino/state was
// never created, every save() failed at open(), and the network transaction
// therefore had no known-good baseline -- which made it refuse every staged
// change. Fail-closed, but the whole rollback safety net was inert.
bool is_dir(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

bool make_one(const std::string& path)
{
#if defined(_WIN32)
    if (::_mkdir(path.c_str()) == 0) return true;
#else
    if (::mkdir(path.c_str(), 0700) == 0) return true;
#endif
    // EEXIST alone is not success: a regular FILE at this path also gives
    // EEXIST, and carrying on would then write state into a path that can
    // never hold it. Only an existing DIRECTORY counts.
    return errno == EEXIST && is_dir(path);
}

bool ensure_dir(const std::string& dir)
{
    if (dir.empty()) return false;
    if (is_dir(dir)) return true;

    // Walk the path: /etc/machino/state is two levels below something that
    // may not exist on a first boot either.
    std::string acc;
    size_t i = 0;
    if (dir[0] == '/') { acc = "/"; i = 1; }
    while (i <= dir.size()) {
        const size_t slash = dir.find('/', i);
        const std::string part = dir.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        if (!part.empty()) {
            if (!acc.empty() && acc != "/") acc += "/";
            acc += part;
            if (!make_one(acc)) return false;
        }
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    return is_dir(dir);
}

} // namespace

FileStateStore::FileStateStore(std::string dir) : dir_(std::move(dir))
{
    // Created here rather than lazily in save(): a store whose directory
    // cannot be made is worth knowing about at start-up, not at the first
    // network change a user tries to make.
    ensure_dir(dir_);
}

std::string FileStateStore::path_for(const std::string& key) const
{
    // Keys are internal constants, never user input, but a key with a slash
    // would silently write outside the directory -- refuse by flattening.
    std::string safe;
    safe.reserve(key.size());
    for (char c : key) safe += (c == '/' || c == '\\' || c == '.') ? '_' : c;
    return dir_ + "/" + safe + ".state";
}

bool FileStateStore::load(const std::string& key, std::string& out) const
{
    std::ifstream f(path_for(key).c_str(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

#if !defined(_WIN32)
namespace {

// Writing the bytes and calling flush() only pushes them into the kernel's
// page cache. A power cut before the cache is written back loses them, and the
// rename may well land first -- leaving a file that exists and is empty. For a
// store whose entire purpose is surviving a power cut, that is the difference
// between a promise and a claim.
bool write_durably(const std::string& path, const std::string& value)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;

    size_t off = 0;
    while (off < value.size()) {
        ssize_t n = ::write(fd, value.data() + off, value.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        off += (size_t)n;
    }
    if (::fsync(fd) != 0) { ::close(fd); return false; }
    return ::close(fd) == 0;
}

// After the rename, the DIRECTORY entry itself is still only in the cache.
// Without this the file can survive while the name pointing at it does not.
// Returns false when the rename is NOT known to have reached the medium.
//
// A rename is a directory operation: without this, the new name can still be
// lost to a power cut even though the data was fsync'd. So a failure here is
// reported, not swallowed -- "we wrote it" has to mean it.
//
// The one exception is a filesystem that refuses to open a directory at all.
// That is a property of the filesystem rather than a failure of this write,
// and treating it as an error would make the store unusable there.
bool fsync_directory(const std::string& dir)
{
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (dfd < 0) return errno == EACCES || errno == EPERM || errno == EINVAL;
    int rc;
    do { rc = ::fsync(dfd); } while (rc != 0 && errno == EINTR);
    // EINVAL on a directory means this filesystem does not support it, which
    // is not this write failing either.
    const bool ok = (rc == 0) || (errno == EINVAL);
    ::close(dfd);
    return ok;
}

} // namespace
#endif

bool FileStateStore::save(const std::string& key, const std::string& value)
{
    const std::string final_path = path_for(key);
    const std::string tmp_path = final_path + ".tmp";

#if defined(_WIN32)
    {
        std::ofstream f(tmp_path.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << value;
        f.flush();
        if (!f.good()) return false;
    }
#else
    // 0600: a network configuration can carry a pre-shared key, and this file
    // sits on the overlay where anything on the box could read it otherwise.
    if (!write_durably(tmp_path, value)) return false;
#endif

    // rename over the old file: a reader sees one or the other, never a
    // fragment. On POSIX this replaces the target atomically; an earlier
    // version called remove() first "because Windows needs it", which opened a
    // window where the file did not exist at all.
#if defined(_WIN32)
    // Windows rename(2) fails if the target exists, so the delete really is
    // needed here -- and this platform is the host test environment, not the
    // camera. The window it opens is accepted for that reason and for no
    // other; it must not exist on the target.
    if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        if (std::remove(final_path.c_str()) != 0) { std::remove(tmp_path.c_str()); return false; }
        if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) { std::remove(tmp_path.c_str()); return false; }
    }
    return true;
#else
    // NOT on POSIX. Deleting the target and renaming again would mean that any
    // rename failure -- ENOSPC, EROFS, EIO, a full jffs2 overlay -- destroys
    // the last known-good record on the way to failing anyway. That is exactly
    // the class of bug this file was rewritten to remove: for
    // network-confirmed, losing it is what makes a rollback impossible.
    if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        ::unlink(tmp_path.c_str());     // leave no half-written litter behind
        return false;
    }

    // The data is on the medium and the name now points at it -- but the name
    // itself is a directory entry, and that has to be durable too.
    if (!fsync_directory(dir_)) return false;
    return true;
#endif
}

void FileStateStore::clear(const std::string& key)
{
    std::remove(path_for(key).c_str());
#if !defined(_WIN32)
    fsync_directory(dir_);          // the deletion has to stick too
#endif
}

} // namespace machino
