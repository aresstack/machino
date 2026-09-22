#include "core/random.hpp"

#include <cstdio>

#if defined(_WIN32)
#  include <windows.h>
#  include <bcrypt.h>
#endif

namespace machino {

bool secure_random(void* out, size_t n) {
    if (!out || n == 0) return n == 0;

#if defined(_WIN32)
    // The host build only. bcrypt is already linked for mbedTLS, so this costs
    // nothing and keeps the host tests exercising the real code path rather
    // than a stub.
    return BCryptGenRandom(nullptr, (PUCHAR)out, (ULONG)n,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    // /dev/urandom rather than getrandom(2): this kernel is 4.4 and the libc
    // is musl, and urandom is the interface both have always had. It never
    // blocks after early boot, and the daemon starts long after that.
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    const size_t got = fread(out, 1, n, f);
    fclose(f);
    return got == n;
#endif
}

std::string secure_hex(size_t bytes) {
    if (bytes == 0 || bytes > 64) return "";
    unsigned char buf[64];
    if (!secure_random(buf, bytes)) return "";
    static const char* HEX = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        out += HEX[(buf[i] >> 4) & 0x0f];
        out += HEX[buf[i] & 0x0f];
    }
    return out;
}

} // namespace machino
