// Direct regression for the comparator used by map insertion and lookup.
// From a configured source root:
// c++ -std=c++17 -ffunction-sections -fdata-sections -Isrc/include -Isrc/dependencies -Ibuild \
//   src/utils.cc test/test_numeric_key_compare.cc -Wl,--gc-sections -o build/test_numeric_key_compare
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>
#include "structures.h"
#include "utils.h"

// Unreachable for the supported scalar key types tested here.
void panic_moo(const char *) { std::abort(); }

static int sign(int value) { return (value > 0) - (value < 0); }

int main()
{
    std::vector<Var> keys;
    for (Num n : {MININT, (Num)-1, (Num)0, (Num)1, MAXINT}) {
        Var key; key.type = TYPE_INT; key.v.num = n; keys.push_back(key);
    }
    for (double n : {-1.0e300, (double)MININT, -1.0, -0.0, 0.0, 1.0, (double)MAXINT, 1.0e300}) {
        Var key; key.type = TYPE_FLOAT; key.v.fnum = n; keys.push_back(key);
    }
    if (sizeof(Num) == 8) {
        for (Num n : {(Num)9007199254740992LL, (Num)9007199254740993LL}) {
            Var key; key.type = TYPE_INT; key.v.num = n; keys.push_back(key);
        }
    }
    Var object; object.type = TYPE_OBJ; object.v.obj = 0; keys.push_back(object);
    Var error; error.type = TYPE_ERR; error.v.err = E_NONE; keys.push_back(error);
    Var string; string.type = TYPE_STR; string.v.str = "x"; keys.push_back(string);
    for (bool truth : {false, true}) {
        Var key; key.type = TYPE_BOOL; key.v.truth = truth; keys.push_back(key);
    }
    Waif waifs[3] = {};
    for (Waif &waif : waifs) {
        Var key; key.type = TYPE_WAIF; key.v.waif = &waif; keys.push_back(key);
        key.type = TYPE_ANON; key.v.anon = reinterpret_cast<Object *>(&waif); keys.push_back(key);
    }
    for (size_t a = 0; a < keys.size(); ++a) {
        if (compare(keys[a], keys[a], 0) != 0) return 1;
        for (size_t b = 0; b < keys.size(); ++b) {
            int ab = compare(keys[a], keys[b], 0);
            if (sign(ab) != -sign(compare(keys[b], keys[a], 0))) {
                std::fprintf(stderr, "antisymmetry failed for keys %zu and %zu\n", a, b);
                return 1;
            }
            if (keys[a].type != keys[b].type && ab == 0) return 1;
            for (size_t c = 0; c < keys.size(); ++c) {
                if (ab <= 0 && compare(keys[b], keys[c], 0) <= 0 && compare(keys[a], keys[c], 0) > 0) {
                    std::fprintf(stderr, "transitivity failed for keys %zu, %zu and %zu\n", a, b, c);
                    return 1;
                }
            }
        }
    }
    return 0;
}
