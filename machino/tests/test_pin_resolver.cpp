#include "core/hw/pin_resolver.hpp"
#include <cstdio>
using namespace machino::hw;
extern int g_fail_ext, g_pass_ext;
#define TCHECK(c) do { if (c) ++g_pass_ext; else { ++g_fail_ext; fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);} } while(0)
void run_pin_tests() {
    BankPinResolver b; int n = -1;
    TCHECK(b.resolve("PB18", n) && n == 50);
    TCHECK(b.resolve("pb18", n) && n == 50);
    TCHECK(b.resolve("PA0", n)  && n == 0);
    TCHECK(b.resolve("PF31", n) && n == 191);
    TCHECK(!b.resolve("PB018", n));   // keine zwei Schreibweisen fuer denselben Pin
    TCHECK(!b.resolve("PB32", n));
    TCHECK(!b.resolve("PG0", n));
    TCHECK(!b.resolve("P", n));
    TCHECK(!b.resolve("", n));
    TCHECK(!b.resolve("50", n));
    TCHECK(b.name_of(50) == "PB18");
    BankPinResolver b16(16, 4);
    TCHECK(b16.resolve("PB2", n) && n == 18);
    TCHECK(!b16.resolve("PB16", n));
    NumericPinResolver num;
    TCHECK(num.resolve("50", n) && n == 50);
    TCHECK(num.resolve("gpio50", n) && n == 50);
    TCHECK(!num.resolve("PB18", n));
    TCHECK(!num.resolve("050", n));
    TCHECK(num.name_of(50) == "50");
}
