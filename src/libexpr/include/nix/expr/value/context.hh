#pragma once
///@file

#include "nix/util/comparator.hh"
#include "nix/store/derived-path.hh"
#include "nix/util/variant-wrapper.hh"
#include "nix/expr/eval-gc.hh"

#include <algorithm>
#include <set>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace nix {

class BadNixStringContextElem final : public CloneableError<BadNixStringContextElem, Error>
{
    void anchor() override;

public:
    std::string_view raw;

    template<typename... Args>
    BadNixStringContextElem(std::string_view raw_, Args &&... args)
        : CloneableError("")
    {
        raw = raw_;
        auto hf = HintFmt(std::forward<Args>(args)...);
        err.msg = HintFmt("Bad String Context element: %1%: %2%", Uncolored(hf.str()), raw);
    }
};

/**
 * @todo This should be renamed to `StringContextBuilderElem`, since:
 *
 * 1. We use `*Builder` for off-heap temporary data structures
 *
 * 2. The `Nix*` is totally redundant. (And my mistake from a long time
 * ago.)
 */
struct NixStringContextElem
{
    /**
     * Plain opaque path to some store object.
     *
     * Encoded as just the path: `<path>`.
     */
    using Opaque = SingleDerivedPath::Opaque;

    /**
     * Path to a derivation and its entire build closure.
     *
     * The path doesn't just refer to derivation itself and its closure, but
     * also all outputs of all derivations in that closure (including the
     * root derivation).
     *
     * Encoded in the form `=<drvPath>`.
     */
    struct DrvDeep
    {
        StorePath drvPath;

        GENERATE_CMP(DrvDeep, me->drvPath);
    };

    /**
     * Derivation output.
     *
     * Encoded in the form `!<output>!<drvPath>`.
     */
    using Built = SingleDerivedPath::Built;

    using Raw = std::variant<Opaque, DrvDeep, Built>;

    Raw raw;

    GENERATE_CMP(NixStringContextElem, me->raw);

    MAKE_WRAPPER_CONSTRUCTOR(NixStringContextElem);

    /**
     * Decode a context string, one of:
     * - `<path>`
     * - `=<path>`
     * - `!<name>!<path>`
     *
     * @param xpSettings Stop-gap to avoid globals during unit tests.
     */
    static NixStringContextElem
    parse(std::string_view s, const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);

    std::string to_string() const;

    /**
     * Render for use in error messages and other user-facing output.
     *
     * Uses store paths and `DerivedPath` syntax, unlike `to_string()`
     * which uses the internal encoding.
     */
    std::string display(const StoreDirConfig & store) const;
};

class StringData;

/**
 * Order of encoded context elements, equal to the order of the parsed
 * `NixStringContextElem`s. The arguments must be valid encodings
 */
std::strong_ordering compareEncodedContextElems(std::string_view a, std::string_view b) noexcept;

/**
 * @todo This should be renamed to `StringContextBuilder`.
 *
 * @see NixStringContextElem for explanation why.
 *
 * Elements copied from string values are kept in their encoded form and
 * parsed only when the builder is read element by element. Turning the
 * builder back into a value's context needs no parse or print for them
 */
class NixStringContext
{
public:
    using Elems = std::set<NixStringContextElem>;
    using value_type = NixStringContextElem;
    using const_iterator = Elems::const_iterator;
    using iterator = const_iterator;
    using size_type = Elems::size_type;

    /**
     * Encoded elements, possibly repeated, which a GC scan must see
     */
    using Encoded = std::vector<const StringData *, traceable_allocator<const StringData *>>;

private:
    mutable Elems elems;
    mutable Encoded encoded;

    /**
     * The value context (`Value::StringWithContext::Context`) that
     * `encoded` is a copy of, when it was copied whole into an empty
     * builder and nothing else was added
     */
    const void * wholeContext = nullptr;

    size_type compactedSize = 0;

    void materialize() const;

public:
    NixStringContext() = default;

    NixStringContext(std::initializer_list<NixStringContextElem> init)
        : elems(init)
    {
    }

    const_iterator begin() const
    {
        materialize();
        return elems.begin();
    }

    const_iterator end() const
    {
        materialize();
        return elems.end();
    }

    size_type size() const
    {
        materialize();
        return elems.size();
    }

    bool empty() const noexcept
    {
        return elems.empty() && encoded.empty();
    }

    auto insert(const NixStringContextElem & elem)
    {
        wholeContext = nullptr;
        return elems.insert(elem);
    }

    auto insert(NixStringContextElem && elem)
    {
        wholeContext = nullptr;
        return elems.insert(std::move(elem));
    }

    template<typename... Args>
    auto emplace(Args &&... args)
    {
        wholeContext = nullptr;
        return elems.emplace(std::forward<Args>(args)...);
    }

    /**
     * Add the elements of a value's context, which are valid encodings
     */
    void addEncoded(const void * context, const StringData * const * begin, const StringData * const * end);

    const void * getWholeContext() const noexcept
    {
        return elems.empty() ? wholeContext : nullptr;
    }

    /**
     * The elements added in parsed form, without the encoded ones
     */
    const Elems & getTyped() const noexcept
    {
        return elems;
    }

    const Encoded & getEncoded() const noexcept
    {
        return encoded;
    }

    bool operator==(const NixStringContext & other) const
    {
        return std::ranges::equal(*this, other);
    }
};

} // namespace nix
