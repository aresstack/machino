// How a logical pin name becomes a platform pin number.
//
// This exists because the previous version buried an Ingenic convention in
// what claimed to be a generic Linux GPIO backend: "PB18" -> bank 1 * 32 + 18.
// That mapping is a property of one SoC family's pinctrl, not of Linux and not
// of sysfs. A board with a numeric scheme, a gpiochip/line pair or a vendor
// naming of its own has to be able to say so without touching the backend.
//
// The resolver is therefore chosen by the board profile and injected. The GPIO
// backend only ever asks "what number is this name".
#pragma once
#include <string>

namespace machino { namespace hw {

class IPinResolver {
public:
    virtual ~IPinResolver() = default;

    // Name the scheme so the API can report what a board speaks, and so a
    // config written for one board is not silently reinterpreted on another.
    virtual const char* scheme() const = 0;

    virtual bool resolve(const std::string& name, int& number_out) const = 0;

    // The canonical spelling of a number, for status output. Empty when the
    // scheme cannot express it.
    virtual std::string name_of(int number) const = 0;
};

// "P<letter><index>", number = (letter - 'A') * bank_size + index.
// The Ingenic/xburst convention, but expressed as a parameter rather than a
// built-in truth: other SoCs use the same shape with a different bank size or
// a different number of banks.
class BankPinResolver : public IPinResolver {
public:
    explicit BankPinResolver(int bank_size = 32, int banks = 6)
        : bank_size_(bank_size), banks_(banks) {}

    const char* scheme() const override { return "bank-letter"; }
    bool        resolve(const std::string& name, int& number_out) const override;
    std::string name_of(int number) const override;

private:
    int bank_size_;
    int banks_;
};

// Plain numbers, optionally with a "gpio" prefix: "50", "gpio50".
// For boards whose documentation simply names the global number.
class NumericPinResolver : public IPinResolver {
public:
    const char* scheme() const override { return "numeric"; }
    bool        resolve(const std::string& name, int& number_out) const override;
    std::string name_of(int number) const override;
};

}} // namespace machino::hw
