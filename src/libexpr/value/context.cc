#include "nix/util/util.hh"
#include "nix/expr/value/context.hh"
#include "nix/store/store-dir-config.hh"
#include "nix/expr/value.hh"

namespace nix {

void BadNixStringContextElem::anchor() {}

NixStringContextElem NixStringContextElem::parse(std::string_view s0, const ExperimentalFeatureSettings & xpSettings)
{
    std::string_view s = s0;

    auto parseRest = [&](this auto & parseRest) -> SingleDerivedPath {
        // Case on whether there is a '!'
        size_t index = s.find("!");
        if (index == std::string_view::npos) {
            return SingleDerivedPath::Opaque{
                .path = StorePath{s},
            };
        } else {
            std::string output{s.substr(0, index)};
            // Advance string to parse after the '!'
            s = s.substr(index + 1);
            auto drv = make_ref<SingleDerivedPath>(parseRest());
            drvRequireExperiment(*drv, xpSettings);
            return SingleDerivedPath::Built{
                .drvPath = std::move(drv),
                .output = std::move(output),
            };
        }
    };

    if (s.size() == 0) {
        throw BadNixStringContextElem(s0, "String context element should never be an empty string");
    }

    switch (s.at(0)) {
    case '!': {
        // Advance string to parse after the '!'
        s = s.substr(1);

        // Find *second* '!'
        if (s.find("!") == std::string_view::npos) {
            throw BadNixStringContextElem(s0, "String content element beginning with '!' should have a second '!'");
        }

        return std::visit([&](auto x) -> NixStringContextElem { return std::move(x); }, parseRest());
    }
    case '=': {
        return NixStringContextElem::DrvDeep{
            .drvPath = StorePath{s.substr(1)},
        };
    }
    default: {
        // Ensure no '!'
        if (s.find("!") != std::string_view::npos) {
            throw BadNixStringContextElem(
                s0, "String content element not beginning with '!' should not have a second '!'");
        }
        return std::visit([&](auto x) -> NixStringContextElem { return std::move(x); }, parseRest());
    }
    }
}

NixStringContextElem NixStringContextElem::parseValid(std::string_view s)
{
    auto parsePath = [](this auto & parsePath, std::string_view s) -> SingleDerivedPath {
        auto index = s.find('!');
        if (index == std::string_view::npos)
            return SingleDerivedPath::Opaque{.path = StorePath::fromValidBaseName(s)};
        return SingleDerivedPath::Built{
            .drvPath = make_ref<SingleDerivedPath>(parsePath(s.substr(index + 1))),
            .output = std::string{s.substr(0, index)},
        };
    };

    switch (s[0]) {
    case '!':
        return std::visit([&](auto x) -> NixStringContextElem { return std::move(x); }, parsePath(s.substr(1)).raw());
    case '=':
        return NixStringContextElem::DrvDeep{.drvPath = StorePath::fromValidBaseName(s.substr(1))};
    default:
        return NixStringContextElem::Opaque{.path = StorePath::fromValidBaseName(s)};
    }
}

std::string NixStringContextElem::to_string() const
{
    std::string res;

    std::function<void(const SingleDerivedPath &)> toStringRest;
    toStringRest = [&](auto & p) {
        std::visit(
            overloaded{
                [&](const SingleDerivedPath::Opaque & o) { res += o.path.to_string(); },
                [&](const SingleDerivedPath::Built & o) {
                    res += o.output;
                    res += '!';
                    toStringRest(*o.drvPath);
                },
            },
            p.raw());
    };

    std::visit(
        overloaded{
            [&](const NixStringContextElem::Built & b) {
                res += '!';
                toStringRest(b);
            },
            [&](const NixStringContextElem::Opaque & o) { toStringRest(o); },
            [&](const NixStringContextElem::DrvDeep & d) {
                res += '=';
                res += d.drvPath.to_string();
            },
        },
        raw);

    return res;
}

std::string NixStringContextElem::display(const StoreDirConfig & store) const
{
    return std::visit(
        overloaded{
            [&](const NixStringContextElem::Opaque & o) -> std::string {
                return SingleDerivedPath{o}.to_string(store);
            },
            [&](const NixStringContextElem::DrvDeep & d) -> std::string {
                return store.printStorePath(d.drvPath) + " (deep)";
            },
            [&](const NixStringContextElem::Built & b) -> std::string { return SingleDerivedPath{b}.to_string(store); },
        },
        raw);
}

static std::strong_ordering compareEncodedSingleDerivedPaths(std::string_view a, std::string_view b) noexcept
{
    auto ia = a.find('!');
    auto ib = b.find('!');
    if (ia == std::string_view::npos || ib == std::string_view::npos) {
        if (ia != ib)
            return ia == std::string_view::npos ? std::strong_ordering::less : std::strong_ordering::greater;
        return a <=> b;
    }
    if (auto c = compareEncodedSingleDerivedPaths(a.substr(ia + 1), b.substr(ib + 1)); c != 0)
        return c;
    return a.substr(0, ia) <=> b.substr(0, ib);
}

std::strong_ordering compareEncodedContextElems(std::string_view a, std::string_view b) noexcept
{
    /* Variant index of `NixStringContextElem::Raw`: Opaque, DrvDeep, Built */
    auto kind = [](std::string_view s) { return s[0] == '!' ? 2 : s[0] == '=' ? 1 : 0; };
    auto ka = kind(a);
    auto kb = kind(b);
    if (ka != kb)
        return ka <=> kb;
    if (ka == 0)
        return a <=> b;
    if (ka == 1)
        return a.substr(1) <=> b.substr(1);
    return compareEncodedSingleDerivedPaths(a.substr(1), b.substr(1));
}

void NixStringContext::addEncoded(
    const void * context, const StringData * const * begin, const StringData * const * end)
{
    if (begin == end)
        return;
    wholeContext = empty() ? context : nullptr;
    encoded.insert(encoded.end(), begin, end);
    /* A derivation's attributes add the same few elements many times */
    if (encoded.size() > 2 * compactedSize + 64) {
        std::ranges::sort(encoded);
        encoded.erase(std::unique(encoded.begin(), encoded.end()), encoded.end());
        compactedSize = encoded.size();
        wholeContext = nullptr;
    }
}

void NixStringContext::materialize() const
{
    if (encoded.empty())
        return;
    std::ranges::sort(encoded);
    auto last = std::unique(encoded.begin(), encoded.end());
    for (auto i = encoded.begin(); i != last; ++i)
        elems.insert(NixStringContextElem::parseValid((*i)->view()));
    encoded.clear();
    encoded.shrink_to_fit();
}

} // namespace nix
