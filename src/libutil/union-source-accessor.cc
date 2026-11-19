#include "nix/util/source-accessor.hh"

namespace nix {

namespace {

struct UnionSourceAccessor : SourceAccessor
{
private:
    void anchor() override {};

public:
    std::vector<ref<SourceAccessor>> accessors;

    UnionSourceAccessor(std::vector<ref<SourceAccessor>> _accessors)
        : accessors(std::move(_accessors))
    {
        displayPrefix.clear();
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        for (auto & accessor : accessors) {
            auto st = accessor->maybeLstat(path);
            if (st) {
                accessor->readFile(path, sink, sizeCallback);
                return;
            }
        }
        throw FileNotFound("path '%s' does not exist", showPath(path));
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        for (auto & accessor : accessors) {
            auto st = accessor->maybeLstat(path);
            if (st)
                return st;
        }
        return std::nullopt;
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        DirEntries result;
        bool exists = false;
        /* Where the first listing came from. Impure evaluation puts the
           store over the root file system, and both list the same store
           directory */
        std::optional<std::filesystem::path> readAt;
        for (auto & accessor : accessors) {
            auto st = accessor->maybeLstat(path);
            if (!st)
                continue;
            std::optional<DirEntries> entries;
            if (!exists) {
                entries = accessor->readDirectory(path);
                readAt = accessor->getPhysicalPath(path);
            } else if (readAt)
                entries = accessor->readDirectoryUnlessAt(path, *readAt);
            else
                entries = accessor->readDirectory(path);
            if (!entries)
                continue;
            if (!exists)
                result = std::move(*entries);
            else
                for (auto & entry : *entries)
                    // Don't override entries from previous accessors.
                    result.insert(std::move(entry));
            exists = true;
        }
        if (!exists)
            throw FileNotFound("path '%s' does not exist", showPath(path));
        return result;
    }

    std::string readLink(const CanonPath & path) override
    {
        for (auto & accessor : accessors) {
            auto st = accessor->maybeLstat(path);
            if (st)
                return accessor->readLink(path);
        }
        throw FileNotFound("path '%s' does not exist", showPath(path));
    }

    std::string showPath(const CanonPath & path) override
    {
        for (auto & accessor : accessors)
            return accessor->showPath(path);
        return SourceAccessor::showPath(path);
    }

    void invalidateCache() override
    {
        for (auto & accessor : accessors)
            accessor->invalidateCache();
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        for (auto & accessor : accessors) {
            auto p = accessor->getPhysicalPath(path);
            if (p)
                return p;
        }
        return std::nullopt;
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        if (fingerprint)
            return {path, fingerprint};
        for (auto & accessor : accessors) {
            auto [subpath, fingerprint] = accessor->getFingerprint(path);
            if (fingerprint)
                return {subpath, fingerprint};
        }
        return {path, std::nullopt};
    }
};

} // namespace

ref<SourceAccessor> makeUnionSourceAccessor(std::vector<ref<SourceAccessor>> && accessors)
{
    return make_ref<UnionSourceAccessor>(std::move(accessors));
}

} // namespace nix
