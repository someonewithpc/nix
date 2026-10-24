// Check every lexer classifier against the scalar one on random blocks
#include "../src/libexpr/lexer-simd.hh"

#include <cstdio>
#include <cstring>
#include <random>

using namespace nix::lexer::simd;

int main()
{
    auto all = allClassifiers();
    std::mt19937_64 rng(42);
    const char * interesting = " \t\r\n_'-.+$\"\\*az09AZ@[`{/:";
    unsigned char block[64];
    for (int iter = 0; iter < 2000000; iter++) {
        for (auto & b : block)
            b = rng() % 3 ? interesting[rng() % 26] : rng() % 256;
        Masks ref;
        all[0].second(block, ref);
        for (size_t i = 1; i < all.size(); i++) {
            Masks m;
            all[i].second(block, m);
            if (memcmp(&m, &ref, sizeof m)) {
                printf("%s differs from scalar at iteration %d\n", all[i].first, iter);
                return 1;
            }
        }
    }
    for (auto & [name, _] : all)
        printf("%s ", name);
    printf("agree on 2000000 random blocks\n");
}
