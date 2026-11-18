#pragma once
///@file

#include "nix/store/derivations.hh"
#include "nix/store/derivation/full-inputs.hh"
#include "nix/util/fun.hh"

namespace nix {

struct Source;
struct Sink;

namespace derivation {

/**
 * Whether the store directory may contain characters that the ATerm
 * format escapes --- in practice the `\` of a Windows store directory
 * such as `C:\ProgramData\nix\store`.
 *
 * We could always enable it, as escape sequences should only be allowed
 * to be used when needed, but out of an abundance of caution we make
 * it a flag instead.
 */
inline constexpr bool defaultSupportWindowsStoreDir =
#ifdef _WIN32
    true
#else
    false
#endif
    ;

/**
 * Print a derivation.
 */
template<typename Out>
std::string unparse(
    const Derivation<std::set<SingleDerivedPath>, Out> & drv,
    const StoreDirConfig & store,
    bool supportWindowsStoreDir = defaultSupportWindowsStoreDir);

extern template std::string unparse(const Full & drv, const StoreDirConfig & store, bool supportWindowsStoreDir);
extern template std::string
unparse(const FullDeferred & drv, const StoreDirConfig & store, bool supportWindowsStoreDir);
extern template std::string
unparse(const FullInputAddressed & drv, const StoreDirConfig & store, bool supportWindowsStoreDir);

namespace masked {
struct HashInputs;
}

/**
 * The derivation shapes `unparse` below can print: with the inputs
 * already flattened (`FullInputs`), or with them replaced by their
 * masked hashes (`masked::HashInputs`), which is the form whose hash is
 * an input address.
 *
 * @note This is a named concept rather than a `requires` clause written
 * out at each declaration because two atomic constraints are only
 * identical when formed from the *same appearance* of an expression;
 * spelling the same condition twice would declare two distinct
 * overloads.
 */
template<typename Inputs, typename Out>
concept RenderableDerivation =
    // Regular `FullInputs` case takes the `Output` variant, or either of
    // the input-addressing alternatives on their own
    (std::is_same_v<Inputs, FullInputs>
     && (std::is_same_v<Out, Output> || std::is_same_v<Out, Output::InputAddressed>
         || std::is_same_v<Out, Output::Deferred>) )
    // Masked hash is only for input addressing, with masked (`Deferred`) or unmasked (`InputAddressed`) outputs
    || (std::is_same_v<Inputs, masked::HashInputs>
        && (std::is_same_v<Out, Output::InputAddressed> || std::is_same_v<Out, Output::Deferred>) );

/**
 * Print a derivation in one of the intermediate forms.
 *
 * The `masked::HashInputs` cases are not round-trippable: `parse` cannot
 * read them back, as their input derivations are named by hash rather
 * than by store path.
 */
template<typename Inputs, typename Out>
    requires RenderableDerivation<Inputs, Out>
std::string unparse(
    const Derivation<Inputs, Out> & drv,
    const StoreDirConfig & store,
    bool supportWindowsStoreDir = defaultSupportWindowsStoreDir);

/**
 * A derivation's ATerm from the input sources to the end, with gaps
 * where the values of the env vars named after outputs go.
 *
 * The three ATerms input addressing needs (fully masked for the
 * derivation's own input address, as written to the store, and input
 * masked for its dependents) differ only in the outputs, the input
 * derivations and those env vars, so the rest is rendered once and put
 * together with `unparseHead` or `unparseMaskedHead`.
 */
struct UnparsedTail
{
    std::string text;

    /**
     * Offset into `text` and name of the env var, in order.
     */
    std::vector<std::pair<size_t, std::string>> gaps;

    /**
     * Write the text with `value(name)` in each gap.
     */
    void write(Sink & sink, fun<std::string_view(std::string_view)> value) const;
};

/**
 * @param srcs The input sources, `FullInputs::fromSet(drv.inputs).srcs`
 *
 * @param structuredAttrsJson The structured attrs as JSON, in place of
 * `drv.structuredAttrs`
 *
 * @param gapsForOutputs Leave gaps for the env vars named after outputs
 */
UnparsedTail unparseTail(
    const StoreDirConfig & store,
    const Full & drv,
    const StorePathSet & srcs,
    std::optional<std::string_view> structuredAttrsJson,
    bool gapsForOutputs,
    bool supportWindowsStoreDir = defaultSupportWindowsStoreDir);

/**
 * The ATerm up to the input sources, with the input derivations by path.
 */
std::string unparseHead(
    const StoreDirConfig & store,
    const Full & drv,
    const FullInputs & inputs,
    bool supportWindowsStoreDir = defaultSupportWindowsStoreDir);

/**
 * The ATerm up to the input sources, with the input derivations by masked
 * hash, and with `maskOutputs` the outputs masked too.
 */
std::string unparseMaskedHead(
    const StoreDirConfig & store,
    const Full & drv,
    const std::map<Hash, std::set<OutputName, std::less<>>> & inputDrvs,
    bool maskOutputs,
    bool supportWindowsStoreDir = defaultSupportWindowsStoreDir);

/**
 * Read a derivation from a file.
 */
Full parse(
    const StoreDirConfig & store,
    std::string && s,
    std::string_view name,
    bool supportWindowsStoreDir = defaultSupportWindowsStoreDir,
    const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);

Source & read(Source & in, const StoreDirConfig & store, Basic & drv, std::string_view name);
void write(Sink & out, const StoreDirConfig & store, const Basic & drv);

} // namespace derivation

} // namespace nix
