// map-bench <trace> [passes] [only]: replay the parser's attribute set
// inserts through ordered-map candidates. Per pass: build every set the way
// addAttr does (a lookup, then an insert on a miss), iterate every set once
// (bindVars), then look every name up once (eval)
//
// The trace is 16-byte records of the set's address, the symbol id and
// whether the lookup hit, with a zero address where each file starts. It
// came from a call in each `addAttr` and `addInherit` lookup, not on the
// branch. Builds with g++ -std=c++23 -O2 against boost, abseil and
// github.com/Thermadiag/seq
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <flat_map>
#include <map>
#include <memory_resource>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/container/flat_map.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <absl/container/btree_map.h>
#include <seq/flat_map.hpp>
#include <seq/radix_map.hpp>

struct Def
{
    uint8_t kind;
    void * e;
    uint32_t pos;
    uint32_t displ;
};
static_assert(sizeof(Def) == 24);

struct Op
{
    uint32_t set, sym, hit;
};

struct File
{
    uint32_t nsets;
    std::vector<Op> ops;
};

std::vector<File> load(const char * path)
{
    FILE * f = fopen(path, "rb");
    struct R
    {
        uint64_t set;
        uint32_t sym, hit;
    } r;
    std::vector<File> files;
    std::unordered_map<uint64_t, uint32_t> ids;
    while (fread(&r, sizeof r, 1, f) == 1) {
        if (!r.set) {
            files.emplace_back();
            ids.clear();
            continue;
        }
        auto [it, fresh] = ids.try_emplace(r.set, uint32_t(ids.size()));
        files.back().ops.push_back({it->second, r.sym, r.hit});
        files.back().nsets = ids.size();
    }
    return files;
}

struct Counting : std::pmr::memory_resource
{
    std::pmr::memory_resource * next;
    size_t bytes = 0;
    Counting(std::pmr::memory_resource * next)
        : next(next)
    {
    }
    void * do_allocate(size_t n, size_t a) override
    {
        bytes += n;
        return next->allocate(n, a);
    }
    void do_deallocate(void * p, size_t n, size_t a) override
    {
        next->deallocate(p, n, a);
    }
    bool do_is_equal(const memory_resource & o) const noexcept override
    {
        return this == &o;
    }
};

using Clock = std::chrono::steady_clock;
volatile uint64_t sink;

struct Result
{
    double build = 0, iter = 0, find = 0;
    size_t bytes = 0;
};

/* M: map type; make(alloc) builds one; ins(m, sym) does the lower_bound and
   emplace; fin(m) finishes the set (a no-op except for the build-then-sort
   variants); lookup(m, sym) returns a Def* */
template<class M, class Make, class Ins, class Fin, class Iter, class Find>
Result run(const std::vector<File> & files, Make make, Ins ins, Fin fin, Iter iter, Find find)
{
    Result res;
    static char * buf = [] {
        auto p = new char[size_t(1) << 30];
        memset(p, 1, size_t(1) << 30);
        return p;
    }();
    std::pmr::monotonic_buffer_resource arena(buf, size_t(1) << 30, std::pmr::null_memory_resource());
    Counting up(&arena);
    std::pmr::polymorphic_allocator<char> alloc(&up);
    std::vector<M> sets;
    uint64_t acc = 0;
    for (auto & f : files) {
        sets.clear();
        sets.reserve(f.nsets);
        auto t0 = Clock::now();
        for (uint32_t i = 0; i < f.nsets; i++)
            sets.push_back(make(alloc));
        for (auto & op : f.ops)
            ins(sets[op.set], op.sym);
        for (auto & m : sets)
            fin(m);
        auto t1 = Clock::now();
        for (auto & m : sets)
            acc += iter(m);
        auto t2 = Clock::now();
        for (auto & op : f.ops)
            acc += uintptr_t(find(sets[op.set], op.sym));
        auto t3 = Clock::now();
        res.build += std::chrono::duration<double>(t1 - t0).count();
        res.iter += std::chrono::duration<double>(t2 - t1).count();
        res.find += std::chrono::duration<double>(t3 - t2).count();
    }
    sink = acc;
    res.bytes = up.bytes;
    return res;
}

template<class M>
auto sumIter(M & m)
{
    uint64_t s = 0;
    for (auto && [k, v] : m)
        s += k + v.pos;
    return s;
}

/* An unsorted vector that is sorted once the set is finished. Lookups
   while building scan the keys linearly up to `indexAt` entries, and go
   through a hash index from there */
struct BuildThenSort
{
    std::pmr::vector<std::pair<uint32_t, Def>> v;
    boost::unordered_flat_map<uint32_t, uint32_t> * index = nullptr;
    static constexpr size_t indexAt = 32;

    BuildThenSort(std::pmr::polymorphic_allocator<char> a)
        : v(a)
    {
    }
    BuildThenSort(BuildThenSort && o) noexcept
        : v(std::move(o.v))
        , index(std::exchange(o.index, nullptr))
    {
    }
    ~BuildThenSort()
    {
        delete index;
    }

    void insert(uint32_t sym)
    {
        if (!index) {
            for (auto & [k, d] : v)
                if (k == sym)
                    return;
            v.push_back({sym, Def{0, nullptr, sym, 0}});
            if (v.size() == indexAt) {
                index = new boost::unordered_flat_map<uint32_t, uint32_t>;
                for (uint32_t i = 0; i < v.size(); i++)
                    index->emplace(v[i].first, i);
            }
            return;
        }
        if (index->try_emplace(sym, uint32_t(v.size())).second)
            v.push_back({sym, Def{0, nullptr, sym, 0}});
    }

    void finish()
    {
        delete std::exchange(index, nullptr);
        if (!std::is_sorted(v.begin(), v.end(), [](auto & a, auto & b) { return a.first < b.first; }))
            std::sort(v.begin(), v.end(), [](auto & a, auto & b) { return a.first < b.first; });
    }

    Def * find(uint32_t sym)
    {
        auto it = std::lower_bound(v.begin(), v.end(), sym, [](auto & a, uint32_t s) { return a.first < s; });
        return it != v.end() && it->first == sym ? &it->second : nullptr;
    }
};

/* The same, keeping keys and values in separate arrays, so the scans and
   the binary search only touch the 4-byte keys */
struct BuildThenSortSoA
{
    std::pmr::vector<uint32_t> keys;
    std::pmr::vector<Def> vals;
    boost::unordered_flat_map<uint32_t, uint32_t> * index = nullptr;
    static constexpr size_t indexAt = 32;

    BuildThenSortSoA(std::pmr::polymorphic_allocator<char> a)
        : keys(a)
        , vals(a)
    {
    }
    BuildThenSortSoA(BuildThenSortSoA && o) noexcept
        : keys(std::move(o.keys))
        , vals(std::move(o.vals))
        , index(std::exchange(o.index, nullptr))
    {
    }
    ~BuildThenSortSoA()
    {
        delete index;
    }

    void insert(uint32_t sym)
    {
        if (!index) {
            if (std::find(keys.begin(), keys.end(), sym) != keys.end())
                return;
            keys.push_back(sym);
            vals.push_back(Def{0, nullptr, sym, 0});
            if (keys.size() == indexAt) {
                index = new boost::unordered_flat_map<uint32_t, uint32_t>;
                for (uint32_t i = 0; i < keys.size(); i++)
                    index->emplace(keys[i], i);
            }
            return;
        }
        if (index->try_emplace(sym, uint32_t(keys.size())).second) {
            keys.push_back(sym);
            vals.push_back(Def{0, nullptr, sym, 0});
        }
    }

    void finish()
    {
        delete std::exchange(index, nullptr);
        if (std::is_sorted(keys.begin(), keys.end()))
            return;
        std::vector<uint32_t> perm(keys.size());
        for (uint32_t i = 0; i < perm.size(); i++)
            perm[i] = i;
        std::sort(perm.begin(), perm.end(), [&](uint32_t a, uint32_t b) { return keys[a] < keys[b]; });
        std::pmr::vector<uint32_t> k2(keys.get_allocator());
        std::pmr::vector<Def> v2(vals.get_allocator());
        k2.reserve(perm.size());
        v2.reserve(perm.size());
        for (auto i : perm) {
            k2.push_back(keys[i]);
            v2.push_back(vals[i]);
        }
        keys = std::move(k2);
        vals = std::move(v2);
    }

    Def * find(uint32_t sym)
    {
        auto it = std::lower_bound(keys.begin(), keys.end(), sym);
        return it != keys.end() && *it == sym ? &vals[it - keys.begin()] : nullptr;
    }
};

void report(const char * name, Result r, int passes)
{
    printf(
        "| %-34s | %8.2f | %7.2f | %7.2f | %8.2f | %7.1f |\n",
        name,
        r.build / passes * 1e3,
        r.iter / passes * 1e3,
        r.find / passes * 1e3,
        (r.build + r.iter + r.find) / passes * 1e3,
        r.bytes / double(passes) / 1e6);
}

template<class F>
Result best(int passes, F f)
{
    Result b;
    b.build = 1e9;
    for (int i = 0; i < passes; i++) {
        auto r = f();
        if (r.build + r.iter + r.find < b.build + b.iter + b.find)
            b = r;
    }
    return b;
}

int main(int argc, char ** argv)
{
    auto files = load(argv[1]);
    int passes = argc > 2 ? atoi(argv[2]) : 5;
    std::string only = argc > 3 ? argv[3] : "";
    auto want = [&](const char * n) { return only.empty() || only == n; };
    auto def = [](uint32_t s) { return Def{0, nullptr, s, 0}; };
    printf("| map | build ms | iterate ms | find ms | total ms | MB allocated |\n|---|---|---|---|---|---|\n");


    if (want("null")) {
        using M = std::pmr::vector<std::pair<uint32_t, Def>>;
        report("nothing (the replay's own cost)", best(passes, [&] {
                   return run<M>(
                       files,
                       [](auto a) { return M(a); },
                       [&](M & m, uint32_t s) { sink = s + m.size(); },
                       [](M &) {},
                       [](M & m) { return sumIter(m); },
                       [](M & m, uint32_t s) -> Def * { sink = s + m.size(); return nullptr; });
               }), 1);
    }

    if (want("boostsmall")) {
        using V = boost::container::small_vector<std::pair<uint32_t, Def>, 4, std::pmr::polymorphic_allocator<std::pair<uint32_t, Def>>>;
        using M = boost::container::flat_map<uint32_t, Def, std::less<uint32_t>, V>;
        report("boost flat_map, 4 inline", best(passes, [&] {
                   return run<M>(
                       files,
                       [](auto a) { return M(typename M::allocator_type(std::pmr::polymorphic_allocator<std::pair<uint32_t, Def>>(a))); },
                       [&](M & m, uint32_t s) {
                           auto j = m.lower_bound(s);
                           if (j == m.end() || j->first != s)
                               m.emplace_hint(j, s, def(s));
                       },
                       [](M &) {},
                       [](M & m) { return sumIter(m); },
                       [](M & m, uint32_t s) { auto i = m.find(s); return i == m.end() ? nullptr : &i->second; });
               }), 1);
    }
    if (want("std::pmr::map")) {
        using M = std::pmr::map<uint32_t, Def>;
        report("std::pmr::map (now)", best(passes, [&] {
                   return run<M>(
                       files,
                       [](auto a) { return M(a); },
                       [&](M & m, uint32_t s) {
                           auto j = m.lower_bound(s);
                           if (j == m.end() || j->first != s)
                               m.emplace_hint(j, s, def(s));
                       },
                       [](M &) {},
                       [](M & m) { return sumIter(m); },
                       [](M & m, uint32_t s) { auto i = m.find(s); return i == m.end() ? nullptr : &i->second; });
               }), 1);
    }

    if (want("boost")) {
        using M = boost::container::flat_map<uint32_t, Def, std::less<uint32_t>, std::pmr::vector<std::pair<uint32_t, Def>>>;
        report("boost::container::flat_map", best(passes, [&] {
                   return run<M>(
                       files,
                       [](auto a) { return M(a); },
                       [&](M & m, uint32_t s) {
                           auto j = m.lower_bound(s);
                           if (j == m.end() || j->first != s)
                               m.emplace_hint(j, s, def(s));
                       },
                       [](M &) {},
                       [](M & m) { return sumIter(m); },
                       [](M & m, uint32_t s) { auto i = m.find(s); return i == m.end() ? nullptr : &i->second; });
               }), 1);
    }

    if (want("std::flat_map")) {
        using M = std::flat_map<uint32_t, Def, std::less<uint32_t>, std::pmr::vector<uint32_t>, std::pmr::vector<Def>>;
        report("std::flat_map (C++23, keys apart)", best(passes, [&] {
                   return run<M>(
                       files,
                       [](auto a) { return M(std::pmr::polymorphic_allocator<Def>(a)); },
                       [&](M & m, uint32_t s) {
                           auto j = m.lower_bound(s);
                           if (j == m.end() || j->first != s)
                               m.emplace_hint(j, s, def(s));
                       },
                       [](M &) {},
                       [](M & m) { return sumIter(m); },
                       [](M & m, uint32_t s) { auto i = m.find(s); return i == m.end() ? nullptr : &i->second; });
               }), 1);
    }

    if (want("absl")) {
        using M = absl::btree_map<uint32_t, Def, std::less<uint32_t>, std::pmr::polymorphic_allocator<std::pair<const uint32_t, Def>>>;
        report("absl::btree_map", best(passes, [&] {
                   return run<M>(
                       files,
                       [](auto a) { return M(a); },
                       [&](M & m, uint32_t s) {
                           auto j = m.lower_bound(s);
                           if (j == m.end() || j->first != s)
                               m.emplace_hint(j, s, def(s));
                       },
                       [](M &) {},
                       [](M & m) { return sumIter(m); },
                       [](M & m, uint32_t s) { auto i = m.find(s); return i == m.end() ? nullptr : &i->second; });
               }), 1);
    }

    if (want("seq")) {
        using M = seq::flat_map<uint32_t, Def, std::less<>, std::pmr::polymorphic_allocator<std::pair<uint32_t, Def>>>;
        report("seq::flat_map (tiered vector)", best(passes, [&] {
                   return run<M>(
                       files,
                       [](auto a) { return M(a); },
                       [&](M & m, uint32_t s) { m.emplace(s, def(s)); },
                       [](M &) {},
                       [](M & m) { return sumIter(m); },
                       [](M & m, uint32_t s) { auto i = m.find(s); return i == m.end() ? nullptr : &i->second; });
               }), 1);
    }

    if (want("radix")) {
        using M = seq::radix_map<uint32_t, Def, seq::default_key, std::pmr::polymorphic_allocator<std::pair<const uint32_t, Def>>>;
        report("seq::radix_map", best(passes, [&] {
                   return run<M>(
                       files,
                       [](auto a) { return M(a); },
                       [&](M & m, uint32_t s) { m.emplace(s, def(s)); },
                       [](M &) {},
                       [](M & m) { return sumIter(m); },
                       [](M & m, uint32_t s) { auto i = m.find(s); return i == m.end() ? nullptr : &i->second; });
               }), 1);
    }

    if (want("vector")) {
        using M = std::pmr::vector<std::pair<uint32_t, Def>>;
        report("plain vector, linear find", best(passes, [&] {
                   return run<M>(
                       files,
                       [](auto a) { return M(a); },
                       [&](M & m, uint32_t s) {
                           for (auto & [k, d] : m)
                               if (k == s)
                                   return;
                           m.push_back({s, def(s)});
                       },
                       [](M &) {},
                       [](M & m) { return sumIter(m); },
                       [](M & m, uint32_t s) -> Def * {
                           for (auto & [k, d] : m)
                               if (k == s)
                                   return &d;
                           return nullptr;
                       });
               }), 1);
    }

    if (want("bts")) {
        using M = BuildThenSort;
        report("vector, sorted when the set closes", best(passes, [&] {
                   return run<M>(
                       files,
                       [](auto a) { return M(a); },
                       [](M & m, uint32_t s) { m.insert(s); },
                       [](M & m) { m.finish(); },
                       [](M & m) { return sumIter(m.v); },
                       [](M & m, uint32_t s) { return m.find(s); });
               }), 1);
    }

    if (want("btssoa")) {
        using M = BuildThenSortSoA;
        report("same, keys apart", best(passes, [&] {
                   return run<M>(
                       files,
                       [](auto a) { return M(a); },
                       [](M & m, uint32_t s) { m.insert(s); },
                       [](M & m) { m.finish(); },
                       [](M & m) {
                           uint64_t s = 0;
                           for (size_t i = 0; i < m.keys.size(); i++)
                               s += m.keys[i] + m.vals[i].pos;
                           return s;
                       },
                       [](M & m, uint32_t s) { return m.find(s); });
               }), 1);
    }
}
