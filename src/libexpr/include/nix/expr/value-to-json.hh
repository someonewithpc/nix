#pragma once
///@file

#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval.hh"
#include "nix/util/json-text.hh"

#include <string>
#include <map>
#include <nlohmann/json_fwd.hpp>

namespace nix {

nlohmann::json printValueAsJSON(
    EvalState & state, bool strict, Value & v, const PosIdx pos, NixStringContext & context, bool copyToStore = true);

void printValueAsJSON(
    EvalState & state,
    bool strict,
    Value & v,
    const PosIdx pos,
    std::ostream & str,
    NixStringContext & context,
    bool copyToStore = true);

/**
 * Like the tree form, but writes the text directly. Invalid UTF-8 is left
 * for `JSONTextWriter::check`, as the tree form leaves it for `dump()`.
 */
void printValueAsJSON(
    EvalState & state,
    bool strict,
    Value & v,
    const PosIdx pos,
    JSONTextWriter & out,
    NixStringContext & context,
    bool copyToStore = true);

MakeError(JSONSerializationError, Error);

} // namespace nix
