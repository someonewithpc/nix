#pragma once
///@file

#include <boost/unordered/unordered_flat_map.hpp>

#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval-settings.hh"

namespace nix {

typedef boost::unordered_flat_map<PosIdx, DocComment, std::hash<PosIdx>> DocCommentMap;

/**
 * Parse a Nix expression from a buffer
 *
 * `text` holds `length` bytes, the last two of which must be NUL; the
 * parser writes into the buffer
 */
Expr * parseExprFromBuf(
    char * text,
    size_t length,
    Pos::Origin origin,
    const SourcePath & basePath,
    Exprs & exprs,
    SymbolTable & symbols,
    const EvalSettings & settings,
    PosTable & positions,
    DocCommentMap & docComments,
    const ref<SourceAccessor> rootFS);

/**
 * Parse REPL bindings from a buffer
 *
 * Returns ExprAttrs with bindings to add to scope
 */
ExprAttrs * parseReplBindingsFromBuf(
    char * text,
    size_t length,
    Pos::Origin origin,
    const SourcePath & basePath,
    Exprs & exprs,
    SymbolTable & symbols,
    const EvalSettings & settings,
    PosTable & positions,
    DocCommentMap & docComments,
    const ref<SourceAccessor> rootFS);

} // namespace nix
