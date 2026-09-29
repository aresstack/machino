// AP11: der Algorithmen-Katalog (siehe Header). `implemented` spiegelt
// vendor/weirdike/ike_suite.c des gepinnten Builds:
//   ENCR   AES-CBC 128/192/256                      (aes_cbc_bits_ok)
//   PRF    HMAC-SHA1 / SHA2-256 / SHA2-384 / SHA2-512
//   INTEG  HMAC-SHA1-96 / SHA2-256-128 / SHA2-384-192 / SHA2-512-256
//   D-H    14 15 16 19 20 21 28 29 30 31            (weirdike_ike_dh_supported)
// Nicht implementiert: AES-GCM (Primitive vorhanden, nicht aushandelbar),
// 3DES, ChaCha20-Poly1305, MD5, NULL, DH2, DH5, DH32 (Curve448).
#include "core/net/ipsec_algos.hpp"

namespace machino { namespace ipsec {

namespace {

//                id           label                            iana iana2 bits  impl  lancom
const AlgoInfo kDh[] = {
    {"dh2",       "DH2 - MODP-1024",                  2,  0,   0, false, false},
    {"dh5",       "DH5 - MODP-1536",                  5,  0,   0, false, false},
    {"dh14",      "DH14 - MODP-2048",                14,  0,   0, true,  true },
    {"dh15",      "DH15 - MODP-3072",                15,  0,   0, true,  false},
    {"dh16",      "DH16 - MODP-4096",                16,  0,   0, true,  false},
    {"dh19",      "DH19 - ECP-256",                  19,  0,   0, true,  false},
    {"dh20",      "DH20 - ECP-384",                  20,  0,   0, true,  false},
    {"dh21",      "DH21 - ECP-521",                  21,  0,   0, true,  false},
    {"dh28",      "DH28 - Brainpool-256",            28,  0,   0, true,  false},
    {"dh29",      "DH29 - Brainpool-384",            29,  0,   0, true,  false},
    {"dh30",      "DH30 - Brainpool-512",            30,  0,   0, true,  false},
    {"dh31",      "DH31 - Curve25519",               31,  0,   0, true,  false},
    {"dh32",      "DH32 - Curve448",                 32,  0,   0, false, false},
};
const AlgoInfo kIkeEnc[] = {
    {"aes128cbc", "AES-CBC-128",                     12,  0, 128, true,  false},
    {"aes192cbc", "AES-CBC-192",                     12,  0, 192, true,  false},
    {"aes256cbc", "AES-CBC-256",                     12,  0, 256, true,  true },
    {"aes128gcm", "AES-GCM-128",                     20,  0, 128, false, false},
    {"aes192gcm", "AES-GCM-192",                     20,  0, 192, false, false},
    {"aes256gcm", "AES-GCM-256",                     20,  0, 256, false, false},
    {"3des",      "3DES",                             3,  0,   0, false, false},
    {"chacha20",  "ChaCha20-Poly1305",               28,  0,   0, false, false},
};
const AlgoInfo kIkeHash[] = {   // iana = PRF, iana2 = INTEG
    {"sha1",      "SHA-1",                            2,  2,   0, true,  true },
    {"sha256",    "SHA-256",                          5, 12,   0, true,  true },
    {"sha384",    "SHA-384",                          6, 13,   0, true,  false},
    {"sha512",    "SHA-512",                          7, 14,   0, true,  false},
    {"md5",       "MD5",                              1,  1,   0, false, false},
};
const AlgoInfo kEspEnc[] = {
    {"aes128cbc", "AES-CBC-128",                     12,  0, 128, true,  false},
    {"aes192cbc", "AES-CBC-192",                     12,  0, 192, true,  false},
    {"aes256cbc", "AES-CBC-256",                     12,  0, 256, true,  true },
    {"aes128gcm", "AES-GCM-128",                     20,  0, 128, false, false},
    {"aes192gcm", "AES-GCM-192",                     20,  0, 192, false, false},
    {"aes256gcm", "AES-GCM-256",                     20,  0, 256, false, false},
    {"3des",      "3DES",                             3,  0,   0, false, false},
    {"chacha20",  "ChaCha20-Poly1305",               28,  0,   0, false, false},
    {"null",      "NULL (keine Verschluesselung)",   11,  0,   0, false, false},
};
const AlgoInfo kEspHash[] = {   // iana = INTEG
    {"sha1",      "SHA-1",                            2,  0,   0, true,  true },
    {"sha256",    "SHA-256",                         12,  0,   0, true,  true },
    {"sha384",    "SHA-384",                         13,  0,   0, true,  false},
    {"sha512",    "SHA-512",                         14,  0,   0, true,  false},
    {"md5",       "MD5",                              1,  0,   0, false, false},
    {"null",      "NULL (nur mit AEAD, z.B. AES-GCM)", 0, 0,   0, false, false},
};

} // namespace

const char* algo_group_field(AlgoGroup g)
{
    switch (g) {
        case AlgoGroup::Dh:      return "ikeDh";
        case AlgoGroup::IkeEnc:  return "ikeEnc";
        case AlgoGroup::IkeHash: return "ikeHash";
        case AlgoGroup::EspEnc:  return "espEnc";
        case AlgoGroup::EspHash: return "espHash";
    }
    return "?";
}

const char* algo_group_title(AlgoGroup g)
{
    switch (g) {
        case AlgoGroup::Dh:      return "DH-Gruppen (IKE_SA_INIT, PFS)";
        case AlgoGroup::IkeEnc:  return "IKE-SA-Verschluesselung";
        case AlgoGroup::IkeHash: return "IKE-SA-Hash (PRF + Integritaet)";
        case AlgoGroup::EspEnc:  return "Child-SA-Verschluesselung (ESP)";
        case AlgoGroup::EspHash: return "Child-SA-Hash (ESP-Integritaet)";
    }
    return "?";
}

const AlgoInfo* algo_table(AlgoGroup g, size_t& n)
{
    switch (g) {
        case AlgoGroup::Dh:      n = sizeof(kDh) / sizeof(kDh[0]);           return kDh;
        case AlgoGroup::IkeEnc:  n = sizeof(kIkeEnc) / sizeof(kIkeEnc[0]);   return kIkeEnc;
        case AlgoGroup::IkeHash: n = sizeof(kIkeHash) / sizeof(kIkeHash[0]); return kIkeHash;
        case AlgoGroup::EspEnc:  n = sizeof(kEspEnc) / sizeof(kEspEnc[0]);   return kEspEnc;
        case AlgoGroup::EspHash: n = sizeof(kEspHash) / sizeof(kEspHash[0]); return kEspHash;
    }
    n = 0;
    return nullptr;
}

const AlgoInfo* algo_find(AlgoGroup g, const std::string& id)
{
    size_t n = 0;
    const AlgoInfo* t = algo_table(g, n);
    for (size_t i = 0; i < n; ++i) if (id == t[i].id) return &t[i];
    return nullptr;
}

std::string algo_list_check(AlgoGroup g, const std::vector<std::string>& ids)
{
    const std::string field = algo_group_field(g);
    if (ids.empty()) return field + ": leere Liste (mindestens ein Algorithmus)";
    for (const auto& id : ids) {
        const AlgoInfo* a = algo_find(g, id);
        if (!a) return field + ": unbekannt: '" + id + "' ('ipsec algos' zeigt den Katalog)";
        if (!a->implemented)
            return field + ": " + a->label + " ('" + id + "') ist in diesem Build nicht implementiert";
    }
    return {};
}

uint16_t algo_smallest_dh(const std::vector<std::string>& ids)
{
    uint16_t best = 0;
    for (const auto& id : ids) {
        const AlgoInfo* a = algo_find(AlgoGroup::Dh, id);
        if (!a) continue;
        if (best == 0 || a->iana < best) best = a->iana;
    }
    return best;
}

}} // namespace machino::ipsec
