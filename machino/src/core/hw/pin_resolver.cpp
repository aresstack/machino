#include "core/hw/pin_resolver.hpp"

#include <cstdio>

namespace machino { namespace hw {

namespace {

// Strict: a malformed name must not silently become pin 0, which is a real
// pin. Also refuses leading zeros, so "PB018" cannot quietly mean PB18 --
// two spellings for one pin would defeat an allowlist that compares strings.
bool parse_index(const std::string& s, size_t from, int limit, int& out)
{
    if (from >= s.size()) return false;
    if (s.size() - from > 1 && s[from] == '0') return false;
    int v = 0;
    for (size_t i = from; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
        if (v > limit) return false;
    }
    out = v;
    return true;
}

} // namespace

bool BankPinResolver::resolve(const std::string& name, int& number_out) const
{
    if (name.size() < 3) return false;
    if (name[0] != 'P' && name[0] != 'p') return false;

    char letter = name[1];
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    const int bank = letter - 'A';
    if (bank < 0 || bank >= banks_) return false;

    int idx = 0;
    if (!parse_index(name, 2, bank_size_ - 1, idx)) return false;

    number_out = bank * bank_size_ + idx;
    return true;
}

std::string BankPinResolver::name_of(int number) const
{
    if (number < 0 || number >= banks_ * bank_size_) return std::string();
    char buf[16];
    std::snprintf(buf, sizeof(buf), "P%c%d", (char)('A' + number / bank_size_), number % bank_size_);
    return buf;
}

bool NumericPinResolver::resolve(const std::string& name, int& number_out) const
{
    size_t from = 0;
    if (name.size() > 4 && (name.compare(0, 4, "gpio") == 0 || name.compare(0, 4, "GPIO") == 0)) from = 4;
    return parse_index(name, from, 1 << 20, number_out);
}

std::string NumericPinResolver::name_of(int number) const
{
    if (number < 0) return std::string();
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", number);
    return buf;
}

}} // namespace machino::hw
