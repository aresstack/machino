#include "core/state_store.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#  include <io.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace machino {

FileStateStore::FileStateStore(std::string dir) : dir_(std::move(dir)) {}

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
void fsync_directory(const std::string& dir)
{
    const int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd < 0) return;            // best effort: not all filesystems allow it
    ::fsync(dfd);
    ::close(dfd);
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
    if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        if (std::remove(final_path.c_str()) != 0) return false;
        if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) return false;
    }

#if !defined(_WIN32)
    fsync_directory(dir_);
#endif
    return true;
}

void FileStateStore::clear(const std::string& key)
{
    std::remove(path_for(key).c_str());
#if !defined(_WIN32)
    fsync_directory(dir_);          // the deletion has to stick too
#endif
}

} // namespace machino
