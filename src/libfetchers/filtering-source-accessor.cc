#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/util/strings.hh"
#include "nix/util/sync.hh"
#include "nix/util/util.hh"

#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/unordered/concurrent_flat_set.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

namespace nix {

std::optional<std::filesystem::path> FilteringSourceAccessor::getPhysicalPath(const CanonPath & path)
{
    checkAccess(path);
    return next->getPhysicalPath(prefix / path);
}

void FilteringSourceAccessor::readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback)
{
    checkAccess(path);
    return next->readFile(prefix / path, sink, sizeCallback);
}

bool FilteringSourceAccessor::pathExists(const CanonPath & path)
{
    return isAllowed(path) && next->pathExists(prefix / path);
}

std::optional<SourceAccessor::Stat> FilteringSourceAccessor::maybeLstat(const CanonPath & path)
{
    return isAllowed(path) ? next->maybeLstat(prefix / path) : std::nullopt;
}

SourceAccessor::Stat FilteringSourceAccessor::lstat(const CanonPath & path)
{
    checkAccess(path);
    return next->lstat(prefix / path);
}

SourceAccessor::DirEntries FilteringSourceAccessor::readDirectory(const CanonPath & path)
{
    checkAccess(path);
    DirEntries entries;
    for (auto & entry : next->readDirectory(prefix / path)) {
        if (isAllowed(path / entry.first))
            entries.insert(std::move(entry));
    }
    return entries;
}

std::string FilteringSourceAccessor::readLink(const CanonPath & path)
{
    checkAccess(path);
    return next->readLink(prefix / path);
}

std::string FilteringSourceAccessor::showPath(const CanonPath & path)
{
    return displayPrefix + next->showPath(prefix / path) + displaySuffix;
}

std::pair<CanonPath, std::optional<std::string>> FilteringSourceAccessor::getFingerprint(const CanonPath & path)
{
    if (fingerprint)
        return {path, fingerprint};
    return next->getFingerprint(prefix / path);
}

void FilteringSourceAccessor::checkAccess(const CanonPath & path)
{
    if (!isAllowed(path))
        throw makeNotAllowedError(path);
}

struct AllowListSourceAccessorImpl : AllowListSourceAccessor
{
private:
    void anchor() override {};

    /**
     * Every allowed prefix (true) and every ancestor of one (false). It
     * holds the parent of each of its paths, so a walk down from the root
     * can stop at the first path that is missing.
     */
    using Prefixes = boost::unordered_flat_map<std::string, bool, StringViewHash, std::equal_to<>>;

    static void insertPrefix(Prefixes & prefixes, const CanonPath & prefix)
    {
        auto & s = prefix.abs();
        prefixes.insert_or_assign(s, true);
        for (auto i = s.size(); i > 1;) {
            i = s.rfind('/', i - 1);
            if (!prefixes.try_emplace(s.substr(0, i ? i : 1), false).second)
                break;
        }
    }

    SharedSync<Prefixes> allowedPrefixes;
    boost::concurrent_flat_set<CanonPath> allowedPaths;

public:
    AllowListSourceAccessorImpl(
        ref<SourceAccessor> next,
        const std::set<CanonPath> & allowedPrefixes,
        const std::unordered_set<CanonPath> & allowedPaths,
        MakeNotAllowedError && makeNotAllowedError)
        : AllowListSourceAccessor(SourcePath(next), std::move(makeNotAllowedError))
        , allowedPaths(allowedPaths.begin(), allowedPaths.end())
    {
        auto prefixes(this->allowedPrefixes.lock());
        for (auto & prefix : allowedPrefixes)
            insertPrefix(*prefixes, prefix);
    }

    /**
     * Same verdict as `path.isAllowed(prefixes)`: `path` is an allowed
     * prefix, lies under one or is an ancestor of one.
     */
    bool isAllowed(const CanonPath & path) override
    {
        if (allowedPaths.contains(path))
            return true;

        auto prefixes(allowedPrefixes.readLock());
        std::string_view s = path.abs();
        if (prefixes->contains(s))
            return true;
        if (s.size() == 1)
            return false;
        for (size_t end = 1; end != s.npos; end = s.find('/', end + 1)) {
            auto i = prefixes->find(s.substr(0, end));
            if (i == prefixes->end())
                return false;
            if (i->second)
                return true;
        }
        return false;
    }

    void allowPrefix(CanonPath prefix) override
    {
        insertPrefix(*allowedPrefixes.lock(), prefix);
    }
};

ref<AllowListSourceAccessor> AllowListSourceAccessor::create(
    ref<SourceAccessor> next,
    const std::set<CanonPath> & allowedPrefixes,
    const std::unordered_set<CanonPath> & allowedPaths,
    MakeNotAllowedError && makeNotAllowedError)
{
    return make_ref<AllowListSourceAccessorImpl>(next, allowedPrefixes, allowedPaths, std::move(makeNotAllowedError));
}

CachingFilteringSourceAccessor::CachingFilteringSourceAccessor(
    const SourcePath & src, MakeNotAllowedError && makeNotAllowedError)
    : FilteringSourceAccessor(src, std::move(makeNotAllowedError))
    , cache(make_ref<boost::concurrent_flat_map<CanonPath, bool>>())
{
}

bool CachingFilteringSourceAccessor::isAllowed(const CanonPath & path)
{
    if (auto allowed = getConcurrent(*cache, path))
        return *allowed;
    auto res = isAllowedUncached(path);
    cache->emplace(path, res);
    return res;
}

} // namespace nix
