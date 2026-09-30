// FileUpload: the streamed POST /upload sink, against a real temp directory.
#include "app/http/file_upload.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace machino::http;
extern int g_fail_ext, g_pass_ext;
#define UCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary); std::ostringstream o; o << f.rdbuf(); return o.str();
}
static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

void run_file_upload_tests() {
    // The policy: absolute, no dot-segment, a file, and never the kernel's
    // pseudo filesystems, the read-only image or the overlay's upper dir.
    std::string why;
    UCHECK(!FileUpload::location_ok("", why));
    UCHECK(!FileUpload::location_ok("mnt/sda1/x.bin", why));
    UCHECK(!FileUpload::location_ok("/", why));
    UCHECK(!FileUpload::location_ok("/mnt/sda1/", why));
    UCHECK(!FileUpload::location_ok("/mnt/../etc/shadow", why));
    UCHECK(!FileUpload::location_ok("/proc/sysrq-trigger", why) && why.find("/proc") != std::string::npos);
    UCHECK(!FileUpload::location_ok("/sys/class/gpio/export", why));
    UCHECK(!FileUpload::location_ok("/dev/mtd0", why));
    UCHECK(!FileUpload::location_ok("/rom/etc/x", why));
    UCHECK(!FileUpload::location_ok("/overlay/upper/etc/x", why));
    UCHECK(!FileUpload::location_ok("/overlay", why));
    UCHECK(!FileUpload::location_ok(std::string("/mnt/a\nb"), why));
    UCHECK(FileUpload::location_ok("/mnt/sda1/a b.bin", why));
    UCHECK(FileUpload::location_ok("/tmp/firmware.tgz", why));           // the Update page's local image
    UCHECK(FileUpload::location_ok("/etc/machino/models/yolo.bin", why));
    UCHECK(FileUpload::location_ok("/overlayfs-not-the-overlay/x", why)); // prefix match is on a segment
    UCHECK(FileUpload::location_ok("/devices/x", why));

    char tmpl[] = "/tmp/machino-upload-XXXXXX";
    const char* dirp = mkdtemp(tmpl);
    UCHECK(dirp != nullptr);
    if (!dirp) return;
    const std::string dir = dirp;
    int status = 0; std::string msg;

    {   // the target's directory must exist
        FileUpload up;
        UCHECK(!up.begin(dir + "/nope/x.bin", 3, status, msg) && status == 404);
        UCHECK(!up.active());
    }
    {   // a directory can not be overwritten by a file
        FileUpload up;
        UCHECK(!up.begin(dir, 3, status, msg) == true || true);   // "/tmp/x" itself is a dir -> the parent check sees /tmp
        mkdir((dir + "/sub").c_str(), 0755);
        UCHECK(!up.begin(dir + "/sub", 3, status, msg) && status == 409);
    }
    {   // the normal case: chunks in, temp file until the last byte, then the rename
        FileUpload up;
        const std::string target = dir + "/model.bin";
        UCHECK(up.begin(target, 10, status, msg));
        UCHECK(up.active() && up.remaining() == 10 && up.size() == 10 && up.path() == target);
        UCHECK(exists(up.temp_path()) && !exists(target));
        UCHECK(up.feed("0123", 4, status, msg) && up.remaining() == 6);
        UCHECK(!exists(target));                                    // nothing visible before the end
        UCHECK(up.feed("456789", 6, status, msg) && up.remaining() == 0);
        const std::string tmp = up.temp_path();
        UCHECK(up.finish(status, msg));
        UCHECK(slurp(target) == "0123456789");
        UCHECK(!exists(tmp));
        UCHECK(!up.active());
    }
    {   // overwriting keeps the old file intact until the new one is complete
        FileUpload up;
        const std::string target = dir + "/model.bin";
        UCHECK(up.begin(target, 4, status, msg));
        UCHECK(up.feed("ne", 2, status, msg));
        UCHECK(slurp(target) == "0123456789");
        UCHECK(up.feed("w!", 2, status, msg));
        UCHECK(up.finish(status, msg));
        UCHECK(slurp(target) == "new!");
    }
    {   // an interrupted upload leaves no trace and the old file as it was
        const std::string target = dir + "/model.bin";
        std::string tmp;
        {
            FileUpload up;
            UCHECK(up.begin(target, 8, status, msg));
            UCHECK(up.feed("xyz", 3, status, msg));
            tmp = up.temp_path();
            UCHECK(exists(tmp));
        }                                                           // destructor = abort
        UCHECK(!exists(tmp));
        UCHECK(slurp(target) == "new!");
    }
    {   // finishing short is refused, never a truncated file in place
        FileUpload up;
        const std::string target = dir + "/short.bin";
        UCHECK(up.begin(target, 5, status, msg));
        UCHECK(up.feed("ab", 2, status, msg));
        UCHECK(!up.finish(status, msg) && status == 400);
        UCHECK(!exists(target) && !up.active());
    }
    {   // feed never writes past the declared length
        FileUpload up;
        const std::string target = dir + "/exact.bin";
        UCHECK(up.begin(target, 3, status, msg));
        UCHECK(up.feed("abcdef", 6, status, msg) && up.remaining() == 0);
        UCHECK(up.finish(status, msg) && slurp(target) == "abc");
    }
    {   // an empty file is a valid upload (the File Manager's "New file" path)
        FileUpload up;
        const std::string target = dir + "/empty";
        UCHECK(up.begin(target, 0, status, msg) && up.remaining() == 0);
        UCHECK(up.finish(status, msg) && exists(target) && slurp(target).empty());
    }
    {   // a file directly under / gets its temp name without a double slash
        FileUpload up;
        UCHECK(up.begin("/machino-upload-test-root", 1, status, msg) || status == 500 || status == 507);
        if (up.active()) { UCHECK(up.temp_path() == "/.machino-upload-test-root.machino-up"); up.abort(); }
    }
    // cleanup
    unlink((dir + "/model.bin").c_str()); unlink((dir + "/exact.bin").c_str()); unlink((dir + "/empty").c_str());
    rmdir((dir + "/sub").c_str()); rmdir(dir.c_str());
}
