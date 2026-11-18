#include <gtest/gtest.h>

#include <map>
#include <numeric>
#include <random>

#include "nix/expr/tests/libexpr.hh"

namespace nix {

class BindingsTest : public LibExprTest
{
protected:
    std::vector<Symbol> names;
    std::vector<Value *> values;

    BindingsTest()
    {
        for (int i = 0; i < 64; ++i) {
            names.push_back(state.symbols.create("a" + std::to_string(i)));
            values.push_back(state.allocValue());
        }
    }

    size_t layerOf(const Attr & attr)
    {
        return std::ranges::find(values, attr.value) - values.begin();
    }

    /* Layers are given topmost first, as indices into `names`. Each layer
       points its attributes at its own value so the winning layer shows.
       An empty layer gets capacity 1, since the shared empty set can't be
       layered */
    const Bindings * buildStack(const std::vector<std::vector<size_t>> & layers)
    {
        const Bindings * base = nullptr;
        for (size_t l = layers.size(); l-- > 0;) {
            std::vector<Symbol> syms;
            for (auto i : layers[l])
                syms.push_back(names[i]);
            std::ranges::sort(syms);
            auto bb = state.buildBindings(std::max<size_t>(syms.size(), 1));
            if (base)
                bb.layerOnTopOf(*base);
            for (auto s : syms)
                bb.insert(s, values[l]);
            base = bb.alreadySorted();
        }
        return base;
    }
};

TEST_F(BindingsTest, iterationMatchesModelOverRandomLayerStacks)
{
    std::mt19937 rng(42);
    for (int round = 0; round < 5000; ++round) {
        size_t numLayers = std::uniform_int_distribution<size_t>(1, 8)(rng);
        size_t universe = std::uniform_int_distribution<size_t>(1, names.size())(rng);
        std::vector<std::vector<size_t>> layers(numLayers);
        std::map<Symbol, size_t> model;
        for (size_t l = 0; l < numLayers; ++l) {
            size_t count = std::uniform_int_distribution<size_t>(0, universe)(rng);
            std::vector<size_t> pool(universe);
            std::iota(pool.begin(), pool.end(), 0);
            std::shuffle(pool.begin(), pool.end(), rng);
            pool.resize(count);
            layers[l] = pool;
            for (auto i : pool)
                model.try_emplace(names[i], l);
        }

        auto & bindings = *buildStack(layers);
        ASSERT_EQ(bindings.size(), model.size());

        std::vector<std::pair<Symbol, size_t>> walked;
        for (auto & attr : bindings)
            walked.emplace_back(attr.name, layerOf(attr));
        std::vector<std::pair<Symbol, size_t>> expected(model.begin(), model.end());
        ASSERT_EQ(walked, expected);

        for (auto & [name, layer] : model) {
            auto attr = bindings.get(name);
            ASSERT_NE(attr, nullptr);
            ASSERT_EQ(layerOf(*attr), layer);
        }
    }
}

TEST_F(BindingsTest, iteratorCopiesAreIndependent)
{
    auto & bindings = *buildStack({{1, 3, 5}, {0, 1, 2}, {2, 4, 6}});
    auto it = bindings.begin();
    ++it;
    auto copy = it;
    ++it;
    ++it;
    ASSERT_EQ(copy->name, names[1]);
    ASSERT_EQ(it->name, names[3]);
    ++copy;
    ASSERT_EQ(copy->name, names[2]);
    ASSERT_EQ(std::distance(copy, bindings.end()), 5);
}

TEST_F(BindingsTest, emptyLayers)
{
    auto & bindings = *buildStack({{}, {2}, {}, {}, {1, 2}, {}});
    std::vector<Symbol> walked;
    for (auto & attr : bindings)
        walked.push_back(attr.name);
    ASSERT_EQ(walked, (std::vector<Symbol>{names[1], names[2]}));
    ASSERT_EQ(layerOf(*bindings.get(names[2])), 1u);

    auto & allEmpty = *buildStack({{}, {}});
    ASSERT_EQ(allEmpty.begin(), allEmpty.end());
}

} // namespace nix
