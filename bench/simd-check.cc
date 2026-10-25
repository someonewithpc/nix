// Check every lexer classifier against the scalar one on random blocks
#include "../src/libexpr/lexer-simd.hh"

#include <chrono>
#include <cstdio>
#include <vector>
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

    /* 64 MiB of source-like bytes, classified block by block */
    std::vector<unsigned char> buf(64 << 20);
    for (auto & b : buf)
        b = interesting[rng() % 26];
    for (auto & [name, classify] : all) {
        uint64_t sink = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < buf.size(); i += 64) {
            Masks m;
            classify(buf.data() + i, m);
            sink += m.space ^ m.ident ^ m.strStop;
        }
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("%-7s %6.2f GB/s (%llu)\n", name, buf.size() / s / 1e9, (unsigned long long) (sink & 1));
    }
}
