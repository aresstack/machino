// FileStateStore durability.
//
// This is the file the network transaction's known-good configuration lives
// in. Losing it does not merely lose a setting: recover() then has nothing to
// roll back to and fails closed, which on a camera reached only over the
// network means someone has to drive to it. So the failure paths get tests,
// not just the happy one.
//
// Two of the cases below can only be provoked on POSIX with a filesystem that
// honours permission bits. Where they cannot, they SAY they were skipped
// rather than counting as passes -- a durability test that quietly does
// nothing is worse than none.
#include "core/state_store.hpp"

#include <cstdio>
#include <string>

#include <dirent.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace machino;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

const char* kDir = "tests/tmp-state-store";

// No system(3) here. Besides being a fork, its return value is
// warn_unused_result on glibc, so ignoring it is an error under -Werror --
// which is how this file first broke the cross-build it was written to
// protect.
void make_dir()
{
#if defined(_WIN32)
    ::_mkdir("tests");
    ::_mkdir(kDir);
#else
    ::mkdir("tests", 0755);
    ::mkdir(kDir, 0755);
#endif
}

void wipe()
{
    // Flat directory by construction: one file per key, plus any .tmp a failed
    // write might have left. Read it rather than listing names we think are
    // there -- a leftover we did not expect is exactly what these tests look
    // for, and it must not survive into the next case.
#if defined(_WIN32)
    ::_chmod(kDir, 0700);
#else
    ::chmod(kDir, 0755);            // an earlier case may have made it read-only
#endif
    if (DIR* d = ::opendir(kDir)) {
        while (struct dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            std::remove((std::string(kDir) + "/" + n).c_str());
        }
        ::closedir(d);
    }
#if defined(_WIN32)
    ::_rmdir(kDir);
#else
    ::rmdir(kDir);
#endif
}

bool exists(const std::string& p)
{
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

void test_round_trip_and_no_litter()
{
    wipe(); make_dir();
    FileStateStore s(kDir);

    std::string out;
    TCHECK(!s.load("absent", out));

    TCHECK(s.save("k", "value one"));
    TCHECK(s.load("k", out) && out == "value one");

    TCHECK(s.save("k", "value two"));
    TCHECK(s.load("k", out) && out == "value two");

    // A leftover .tmp means a write path returned without cleaning up, and the
    // next reader of the directory has to reason about which file is real.
    TCHECK(!exists(std::string(kDir) + "/k.tmp"));

    s.clear("k");
    TCHECK(!s.load("k", out));
    wipe();
}

void test_the_store_creates_its_own_directory()
{
    // THE bug this test exists for. On the camera /etc/machino/state did not
    // exist, FileStateStore never created it, every save() failed at open(),
    // and NetworkTxn therefore had no known-good baseline -- so every staged
    // network change was refused with "there is no confirmed configuration to
    // fall back to". The entire rollback safety net was inert, and the host
    // tests could not see it because they all use MemStore or a directory the
    // test made first.
    wipe();
#if defined(_WIN32)
    ::_mkdir("tests");
#else
    ::mkdir("tests", 0755);
#endif
    // Deliberately NOT creating kDir here.
    FileStateStore s(kDir);
    TCHECK(s.save("k", "value"));
    std::string out;
    TCHECK(s.load("k", out) && out == "value");
    wipe();
}

void test_a_nested_directory_is_created_too()
{
    // /etc/machino/state is two levels below something that may not exist on
    // a first boot either.
    const std::string nested = std::string(kDir) + "/a/b";
    FileStateStore s(nested);
    TCHECK(s.save("k", "deep"));
    std::string out;
    TCHECK(s.load("k", out) && out == "deep");

    std::remove((nested + "/k.state").c_str());
#if defined(_WIN32)
    ::_rmdir(nested.c_str()); ::_rmdir((std::string(kDir) + "/a").c_str());
#else
    ::rmdir(nested.c_str()); ::rmdir((std::string(kDir) + "/a").c_str());
#endif
    wipe();
}

void test_a_file_in_the_way_is_not_mistaken_for_a_directory()
{
    // mkdir returns EEXIST for a regular file too. Treating that as success
    // would have the store carry on and write into a path that can never hold
    // it -- and the failure would surface much later, as a refused network
    // change with no obvious cause.
    wipe();
    make_dir();
    const std::string blocked = std::string(kDir) + "/blocker";
    FILE* f = std::fopen(blocked.c_str(), "wb");
    TCHECK(f != nullptr);
    if (f) { std::fputs("not a directory", f); std::fclose(f); }

    FileStateStore s(blocked);
    TCHECK(!s.save("k", "value"));          // refuses rather than pretending
    std::string out;
    TCHECK(!s.load("k", out));

    std::remove(blocked.c_str());
    wipe();
}

void test_an_empty_value_is_not_the_same_as_an_absent_key()
{
    // network-pending is read for PRESENCE: "a change was in flight". An empty
    // record that reads back as absent would silently skip the rollback.
    wipe(); make_dir();
    FileStateStore s(kDir);
    TCHECK(s.save("e", ""));
    std::string out = "dirty";
    TCHECK(s.load("e", out));
    TCHECK(out.empty());
    wipe();
}

void test_a_value_with_newlines_and_nuls_survives()
{
    wipe(); make_dir();
    FileStateStore s(kDir);
    const std::string blob = std::string("a\nb\r\nc\0d", 8);
    TCHECK(s.save("b", blob));
    std::string out;
    TCHECK(s.load("b", out) && out == blob);
    wipe();
}

#if !defined(_WIN32)
void test_the_file_is_not_world_readable()
{
    // A WiFi candidate carries a passphrase, and this sits on the overlay
    // where anything on the box could read it otherwise.
    wipe(); make_dir();
    FileStateStore s(kDir);
    TCHECK(s.save("secret", "psk"));
    struct stat st;
    if (::stat((std::string(kDir) + "/secret").c_str(), &st) == 0) {
        TCHECK((st.st_mode & 0077) == 0);
    } else {
        fprintf(stderr, "SKIP: could not stat the state file\n");
    }
    wipe();
}

void test_a_failing_write_does_not_destroy_the_previous_value()
{
    // THE regression this file exists for. An earlier version deleted the
    // target and renamed again whenever rename() failed, on every platform.
    // Any rename failure -- ENOSPC, EROFS, EIO, a full jffs2 overlay -- then
    // destroyed the last known-good record on the way to failing anyway, and
    // for network-confirmed that is what makes a rollback impossible.
    wipe(); make_dir();
    FileStateStore s(kDir);
    TCHECK(s.save("k", "known good"));

    if (::geteuid() == 0) {
        fprintf(stderr, "SKIP: running as root, a read-only directory cannot be provoked\n");
        wipe();
        return;
    }
    if (::chmod(kDir, 0500) != 0) {
        fprintf(stderr, "SKIP: this filesystem does not honour directory permissions\n");
        wipe();
        return;
    }

    const bool wrote = s.save("k", "should not land");
    ::chmod(kDir, 0755);

    if (wrote) {
        // Some filesystems let the owner write regardless. Then the case was
        // not provoked and asserting anything about it would be noise.
        fprintf(stderr, "SKIP: the write succeeded, a read-only directory was not enforced\n");
        wipe();
        return;
    }

    std::string out;
    TCHECK(s.load("k", out));
    TCHECK(out == "known good");        // still there, which is the whole point
    wipe();
}
#endif

} // namespace

void run_state_store_tests()
{
    test_the_store_creates_its_own_directory();
    test_a_nested_directory_is_created_too();
    test_a_file_in_the_way_is_not_mistaken_for_a_directory();
    test_round_trip_and_no_litter();
    test_an_empty_value_is_not_the_same_as_an_absent_key();
    test_a_value_with_newlines_and_nuls_survives();
#if !defined(_WIN32)
    test_the_file_is_not_world_readable();
    test_a_failing_write_does_not_destroy_the_previous_value();
#else
    fprintf(stderr, "SKIP: POSIX durability cases (file mode, rename failure) - not this platform\n");
#endif
}
