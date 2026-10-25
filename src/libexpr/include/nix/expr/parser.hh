#pragma once
///@file

#include <boost/unordered/unordered_flat_map.hpp>

#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/parse-buffer.hh"

namespace nix {

typedef boost::unordered_flat_map<PosIdx, DocComment, std::hash<PosIdx>> DocCommentMap;

/**
 * Parse a Nix expression from a buffer
 *
 * The parser writes into the buffer
 *
 * With `imports` set, the paths of `import <path literal>` calls and the
 * `.nix` path literals in lists are added to it. With `quiet` set, the parse throws `ParseAbandoned`
 * instead of printing a warning
 */
Expr * parseExprFromBuf(
    ParseBuffer & text,
    Pos::Origin origin,
    const SourcePath & basePath,
    Exprs & exprs,
    SymbolTable & symbols,
    const EvalSettings & settings,
    PosTable & positions,
    DocCommentMap & docComments,
    const ref<SourceAccessor> rootFS,
    std::vector<SourcePath> * imports = nullptr,
    bool quiet = false);

/**
 * Parse REPL bindings from a buffer
 *
 * Returns ExprAttrs with bindings to add to scope
 */
ExprAttrs * parseReplBindingsFromBuf(
    ParseBuffer & text,
    Pos::Origin origin,
    const SourcePath & basePath,
    Exprs & exprs,
    SymbolTable & symbols,
    const EvalSettings & settings,
    PosTable & positions,
    DocCommentMap & docComments,
    const ref<SourceAccessor> rootFS);

} // namespace nix
