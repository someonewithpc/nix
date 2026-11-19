#include <nlohmann/json.hpp>
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "nix/expr/tests/value/context.hh"
#include "nix/store/store-dir-config.hh"

namespace nix {

// Test a few cases of invalid string context elements.

TEST(NixStringContextElemTest, empty_invalid)
{
    EXPECT_THROW(NixStringContextElem::parse(""), BadNixStringContextElem);
}

TEST(NixStringContextElemTest, single_bang_invalid)
{
    EXPECT_THROW(NixStringContextElem::parse("!"), BadNixStringContextElem);
}

TEST(NixStringContextElemTest, double_bang_invalid)
{
    EXPECT_THROW(NixStringContextElem::parse("!!/"), BadStorePath);
}

TEST(NixStringContextElemTest, eq_slash_invalid)
{
    EXPECT_THROW(NixStringContextElem::parse("=/"), BadStorePath);
}

TEST(NixStringContextElemTest, slash_invalid)
{
    EXPECT_THROW(NixStringContextElem::parse("/"), BadStorePath);
}

/**
 * Round trip (string <-> data structure) test for
 * `NixStringContextElem::Opaque`.
 */
TEST(NixStringContextElemTest, opaque)
{
    std::string_view opaque = "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-x";
    auto elem = NixStringContextElem::parse(opaque);
    auto * p = std::get_if<NixStringContextElem::Opaque>(&elem.raw);
    ASSERT_TRUE(p);
    ASSERT_EQ(p->path, StorePath{opaque});
    ASSERT_EQ(elem.to_string(), opaque);
}

/**
 * Round trip (string <-> data structure) test for
 * `NixStringContextElem::DrvDeep`.
 */
TEST(NixStringContextElemTest, drvDeep)
{
    std::string_view drvDeep = "=g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-x.drv";
    auto elem = NixStringContextElem::parse(drvDeep);
    auto * p = std::get_if<NixStringContextElem::DrvDeep>(&elem.raw);
    ASSERT_TRUE(p);
    ASSERT_EQ(p->drvPath, StorePath{drvDeep.substr(1)});
    ASSERT_EQ(elem.to_string(), drvDeep);
}

/**
 * Round trip (string <-> data structure) test for a simpler
 * `NixStringContextElem::Built`.
 */
TEST(NixStringContextElemTest, built_opaque)
{
    std::string_view built = "!foo!g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-x.drv";
    auto elem = NixStringContextElem::parse(built);
    auto * p = std::get_if<NixStringContextElem::Built>(&elem.raw);
    ASSERT_TRUE(p);
    ASSERT_EQ(p->output, "foo");
    ASSERT_EQ(
        *p->drvPath,
        ((SingleDerivedPath) SingleDerivedPath::Opaque{
            .path = StorePath{built.substr(5)},
        }));
    ASSERT_EQ(elem.to_string(), built);
}

/**
 * Round trip (string <-> data structure) test for a more complex,
 * inductive `NixStringContextElem::Built`.
 */
TEST(NixStringContextElemTest, built_built)
{
    /**
     * We set these in tests rather than the regular globals so we don't have
     * to worry about race conditions if the tests run concurrently.
     */
    ExperimentalFeatureSettings mockXpSettings;
    mockXpSettings.set("experimental-features", "dynamic-derivations ca-derivations");

    std::string_view built = "!foo!bar!g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-x.drv";
    auto elem = NixStringContextElem::parse(built, mockXpSettings);
    auto * p = std::get_if<NixStringContextElem::Built>(&elem.raw);
    ASSERT_TRUE(p);
    ASSERT_EQ(p->output, "foo");
    auto * drvPath = std::get_if<SingleDerivedPath::Built>(&*p->drvPath);
    ASSERT_TRUE(drvPath);
    ASSERT_EQ(drvPath->output, "bar");
    ASSERT_EQ(
        *drvPath->drvPath,
        ((SingleDerivedPath) SingleDerivedPath::Opaque{
            .path = StorePath{built.substr(9)},
        }));
    ASSERT_EQ(elem.to_string(), built);
}

/**
 * Without the right experimental features enabled, we cannot parse a
 * complex inductive string context element.
 */
TEST(NixStringContextElemTest, built_built_xp)
{
    ASSERT_THROW(
        NixStringContextElem::parse("!foo!bar!g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-x.drv"), MissingExperimentalFeature);
}

RC_GTEST_PROP(NixStringContextElemTest, prop_round_rip, (const NixStringContextElem & o))
{
    ExperimentalFeatureSettings xpSettings;
    xpSettings.set("experimental-features", "dynamic-derivations");
    RC_ASSERT(o == NixStringContextElem::parse(o.to_string(), xpSettings));
}

RC_GTEST_PROP(NixStringContextElemTest, prop_parse_valid, (const NixStringContextElem & o))
{
    RC_ASSERT(o == NixStringContextElem::parseValid(o.to_string()));
}

static int sign(std::strong_ordering o)
{
    return o < 0 ? -1 : o > 0 ? 1 : 0;
}

TEST(NixStringContextElemTest, encoded_order)
{
    std::vector<SingleDerivedPath> paths;
    for (auto base :
         {"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-x",
          "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-x.drv",
          "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-x-y.drv",
          "00w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-z.drv"})
        paths.push_back(SingleDerivedPath::Opaque{StorePath{base}});
    for (int depth = 0; depth < 2; ++depth) {
        auto n = paths.size();
        for (size_t i = 0; i < n; ++i)
            for (auto output : {"", "out", "dev", "out2", "o"})
                paths.push_back(
                    SingleDerivedPath::Built{
                        .drvPath = make_ref<SingleDerivedPath>(paths[i]),
                        .output = output,
                    });
    }

    std::vector<NixStringContextElem> elems;
    for (auto & p : paths) {
        std::visit([&](auto & x) { elems.push_back(NixStringContextElem{x}); }, p.raw());
        if (auto * o = std::get_if<SingleDerivedPath::Opaque>(&p.raw()))
            elems.push_back(NixStringContextElem::DrvDeep{.drvPath = o->path});
    }

    for (auto & a : elems)
        for (auto & b : elems)
            ASSERT_EQ(sign(compareEncodedContextElems(a.to_string(), b.to_string())), sign(a <=> b))
                << a.to_string() << " " << b.to_string();
}

RC_GTEST_PROP(
    NixStringContextElemTest, prop_encoded_order, (const NixStringContextElem & a, const NixStringContextElem & b))
{
    RC_ASSERT(sign(compareEncodedContextElems(a.to_string(), b.to_string())) == sign(a <=> b));
}

} // namespace nix
