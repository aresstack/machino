// Cryptographic randomness, in one place.
//
// This exists because the tree had three answers to the same question: a
// mt19937_64 for session tokens, another for RTSP nonces, and an ad-hoc
// /dev/urandom read in main for the ONVIF nonce secret. Mersenne Twister is
// not a CSPRNG - its full internal state is recoverable from its output - and
// a session token is a bearer credential. The exposure was narrow (tokens are
// only minted after a credential check, and the RTSP generator is thread_local
// so a connection sees at most one or two draws from it), but "narrow" is an
// invariant of today's call sites, not a property of the primitive.
//
// There is deliberately NO weak fallback. If the platform cannot provide real
// randomness the caller is told so and fails closed, because a predictable
// token is worse than a refused login.
#pragma once
#include <cstddef>
#include <string>

namespace machino {

// Fill `out` with `n` cryptographically random bytes. False means the platform
// would not give them - never that weaker bytes were substituted.
bool secure_random(void* out, size_t n);

// `bytes` random bytes as lowercase hex, or "" when unavailable.
std::string secure_hex(size_t bytes);

} // namespace machino
