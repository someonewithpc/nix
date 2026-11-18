#pragma once
///@file

#include "nix/store/derivation/masked.hh"
#include "nix/store/parsed-derivations.hh"

namespace nix::derivation {

/**
 * What `derivationStrict` needs from a derivation it built.
 */
struct Instantiated
{
    StorePath path;

    StorePathSet references;

    /**
     * The ATerm, when asked for.
     */
    std::string contents;

    /**
     * What `masked::hashInput` gives for the derivation.
     */
    masked::MaskedHash hash;
};

/**
 * Does what `fillInOutputPaths` (if `fillIn`), then `computeStorePath`,
 * then `masked::hashInput` do, with the same results and errors in the
 * same order. The parts the ATerms behind them share are rendered once,
 * and only the one written to the store is ever built as a string.
 *
 * @param drv With `fillIn`, every output is `Deferred` and has an empty
 * env var of its name, and the output paths are filled in
 *
 * @param structuredAttrs In place of `drv.structuredAttrs`, which must be
 * unset when this is given
 *
 * @param wantContents Whether to return the ATerm
 */
Instantiated
instantiate(Store & store, Full & drv, const StructuredAttrsText * structuredAttrs, bool fillIn, bool wantContents);

} // namespace nix::derivation
