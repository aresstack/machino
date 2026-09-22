// core/random: the one source of cryptographic randomness.
//
// Session cookies and auth nonces are built from this, so the properties that
// matter are that it produces the requested amount, that it does not repeat,
// and above all that it reports failure instead of quietly substituting
// something weaker.
#include "core/random.hpp"
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace machino;

extern int g_fail_ext, g_pass_ext;
#define RCHK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

void run_random_tests() {
    // ---- shape --------------------------------------------------------------
    {
        const std::string h = secure_hex(16);
        RCHK(h.size() == 32);                       // 128 bits, the token size
        for (char c : h) RCHK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        RCHK(secure_hex(1).size() == 2);
        RCHK(secure_hex(64).size() == 128);
    }

    // ---- refusals, rather than a shorter or weaker answer --------------------
    {
        RCHK(secure_hex(0).empty());
        RCHK(secure_hex(65).empty());               // past the internal buffer
        RCHK(secure_hex(100000).empty());
        unsigned char one = 0;
        RCHK(!secure_random(nullptr, 1));           // a null sink is not "success"
        RCHK(secure_random(nullptr, 0));            // asking for nothing is fine
        RCHK(secure_random(&one, 1));
    }

    // ---- it does not repeat --------------------------------------------------
    {
        // A generator that returned the same token twice would hand two
        // sessions the same cookie. 64 draws is far too few to say anything
        // about quality, but a stuck or counter-like source fails it at once.
        std::set<std::string> seen;
        for (int i = 0; i < 64; ++i) seen.insert(secure_hex(16));
        RCHK(seen.size() == 64);
    }

    // ---- and it is not obviously stuck --------------------------------------
    {
        unsigned char buf[256];
        memset(buf, 0, sizeof buf);
        RCHK(secure_random(buf, sizeof buf));
        bool all_same = true;
        for (size_t i = 1; i < sizeof buf; ++i) if (buf[i] != buf[0]) { all_same = false; break; }
        RCHK(!all_same);
        // every byte value should not be missing entirely from 256 samples in a
        // way that says "this is a small counter"; a crude spread check is all
        // that is honest here
        std::set<unsigned char> values(buf, buf + sizeof buf);
        RCHK(values.size() > 64);
    }
}
