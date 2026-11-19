#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/terminal.hh"

#include <gtest/gtest.h>

#include <random>

namespace nix {

static ref<AllowListSourceAccessor>
makeAllowList(const std::set<CanonPath> & prefixes, const std::unordered_set<CanonPath> & paths = {})
{
    return AllowListSourceAccessor::create(
        make_ref<MemorySourceAccessor>(), prefixes, paths, [](const CanonPath & path) -> RestrictedPathError {
            return RestrictedPathError("access to absolute path '%1%' is forbidden in restricted mode", path);
        });
}

TEST(AllowListSourceAccessor, matchesCanonPathIsAllowed)
{
    std::set<CanonPath> allowed{
        CanonPath("/foo/bar"),
        CanonPath("/foo/quux"),
        CanonPath("/a/b/c"),
        CanonPath("/nix/store/aaa-x"),
        CanonPath("/nix/store/aaa-x-y/z"),
    };
    auto accessor = makeAllowList(allowed);

    std::vector<std::string> names{"a", "b", "c", "d", "foo", "bar", "quux", "nix", "store", "aaa-x", "aaa-x-y", "z", "aaa", "fo"};
    std::mt19937 rng(42);
    auto check = [&](const CanonPath & path) {
        EXPECT_EQ(accessor->isAllowed(path), path.isAllowed(allowed)) << path;
    };
    check(CanonPath::root);
    for (int n = 0; n < 20000; ++n) {
        CanonPath path = CanonPath::root;
        for (auto depth = rng() % 6; depth > 0; --depth)
            path.push(names[rng() % names.size()]);
        check(path);
        if (n % 1000 == 999) {
            allowed.insert(path);
            accessor->allowPrefix(path);
        }
    }
}

TEST(AllowListSourceAccessor, cases)
{
    auto accessor = makeAllowList({CanonPath("/foo/bar"), CanonPath("/a/b/c")});

    ASSERT_TRUE(accessor->isAllowed(CanonPath("/")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/foo")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/foo/bar")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/foo/bar/bla")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/a/b")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/a/b/c/d/e")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/foo/ba")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/foo/barx")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/foo/baz/bar")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/a/b/d")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/bar")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/fo")));
}

TEST(AllowListSourceAccessor, empty)
{
    auto accessor = makeAllowList({});
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/foo")));
}

TEST(AllowListSourceAccessor, root)
{
    auto accessor = makeAllowList({CanonPath::root});
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/foo/bar")));
}

TEST(AllowListSourceAccessor, exactPaths)
{
    auto accessor = makeAllowList({CanonPath("/x/y")}, {CanonPath("/p/q")});
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/p/q")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/p")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/p/q/r")));
}

TEST(AllowListSourceAccessor, allowPrefixLater)
{
    auto accessor = makeAllowList({CanonPath("/nix/store/aaa-x")});
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/nix/store/bbb-y/file")));
    accessor->allowPrefix(CanonPath("/nix/store/bbb-y"));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/nix/store/bbb-y/file")));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/nix/store/aaa-x/file")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/nix/store/ccc-z")));

    /* An ancestor of an earlier prefix becomes a prefix itself. */
    accessor->allowPrefix(CanonPath("/nix"));
    ASSERT_TRUE(accessor->isAllowed(CanonPath("/nix/store/ccc-z")));
    ASSERT_FALSE(accessor->isAllowed(CanonPath("/etc")));
}

TEST(AllowListSourceAccessor, deniedError)
{
    auto accessor = makeAllowList({CanonPath("/foo")});
    try {
        accessor->readFile(CanonPath("/bar/baz"));
        FAIL() << "expected RestrictedPathError";
    } catch (RestrictedPathError & e) {
        ASSERT_EQ(filterANSIEscapes(e.info().msg.str(), true), "access to absolute path '/bar/baz' is forbidden in restricted mode");
    }
    ASSERT_FALSE(accessor->pathExists(CanonPath("/bar")));
    ASSERT_FALSE(accessor->maybeLstat(CanonPath("/bar")));
    ASSERT_THROW(accessor->lstat(CanonPath("/bar")), RestrictedPathError);
}

} // namespace nix
