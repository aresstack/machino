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

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace machino;

extern int g_fail_ext, g_pass_ext;
#define TCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

const char* kDir = "tests/tmp-state-store";

void make_dir()
{
#if defined(_WIN32)
    ::system("mkdir tests\\tmp-state-store 2>NUL");
#else
    ::mkdir("tests", 0755);
    ::mkdir(kDir, 0755);
#endif
}

void wipe()
{
#if defined(_WIN32)
    ::system("rmdir /s /q tests\\tmp-state-store 2>NUL");
#else
    ::system("rm -rf tests/tmp-state-store");
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
