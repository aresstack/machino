// POST /upload, majestic-native: the WebUI's File Manager and the Update
// page's local .tgz both send the file as the raw request body, with its
// destination in the File-Location header. majestic writes it straight to
// that path. Machino stands in majestic's place as the front door, so it has
// to as well - relayed to busybox httpd the request just vanished (the upload
// XHR ignores its status), measured on the card reader 2026-09-30.
//
// Streamed, not buffered: every chunk goes to a temp file next to the target
// as it arrives, so the 48 MB camera never holds the body, a model or a
// firmware image is not bounded by the working buffer, and an interrupted
// upload leaves the target untouched (the temp file is dropped). The rename
// at the end is the only moment the target changes.
//
// No sockets in here: the poll loop feeds bytes in, this class owns the file.
// That keeps it host-testable (tests/test_file_upload.cpp) - http_server.cpp
// itself does not build on the host.
#pragma once
#include <cstddef>
#include <string>

namespace machino { namespace http {

class FileUpload {
public:
    FileUpload() = default;
    ~FileUpload() { abort(); }
    FileUpload(const FileUpload&) = delete;
    FileUpload& operator=(const FileUpload&) = delete;

    // Where an upload may NOT go, whatever the login: the kernel's pseudo
    // filesystems (a write into /proc or /sys is a command, not a file), the
    // read-only image (/rom) and the overlay's upper directory itself (a
    // write there behind overlayfs' back corrupts the merged view). Also no
    // relative path, no dot-segment, no directory. Everything else is the
    // operator's camera, exactly as on stock majestic - the login gate in
    // front of this is the same one that guards the rest of the WebUI.
    static bool location_ok(const std::string& loc, std::string& why);

    // Validate the location, the target's directory and the free space
    // there, then open the temp file. false: `status` (400/404/409/500/507)
    // and `msg` say why, nothing was created.
    bool begin(const std::string& location, size_t length, int& status, std::string& msg);
    // Write n bytes (n <= remaining()). false: the upload is aborted, the
    // temp file gone, status/msg set (507 when the disk ran out).
    bool feed(const char* p, size_t n, int& status, std::string& msg);
    // All bytes in: close and rename the temp file onto the target.
    bool finish(int& status, std::string& msg);
    // Drop the temp file (client went away, or an error). Idempotent.
    void abort();

    size_t remaining() const { return left_; }
    size_t size() const { return total_; }
    const std::string& path() const { return path_; }
    const std::string& temp_path() const { return tmp_; }
    bool active() const { return fd_ >= 0; }

private:
    int         fd_ = -1;
    size_t      left_ = 0;
    size_t      total_ = 0;
    std::string path_;
    std::string tmp_;
};

}} // namespace machino::http
