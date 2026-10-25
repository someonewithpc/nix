#pragma once
///@file
/**
 * Byte classification for the lexer, one 64-byte block at a time
 *
 * The classification is written once, against a small vector interface
 * that each instruction set implements; adding an instruction set means
 * adding one such backend and listing it in `pickClassifier`
 */

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace nix::lexer::simd {

/**
 * Bit `i` of a mask is set when byte `i` of the block is in the class
 */
struct Masks
{
    /** `[ \t\r\n]` */
    uint64_t space;
    /** `[a-zA-Z0-9_'-]`, the characters after the first of an identifier */
    uint64_t ident;
    /** `[a-zA-Z0-9._+-]` */
    uint64_t path;
    /** `[0-9]` */
    uint64_t digit;
    /** `["\\$]`, where a string body stops */
    uint64_t strStop;
    /** `[$'\0]`, where an indented string body stops or has a NUL */
    uint64_t indStop;
    /** `[\r\n]` */
    uint64_t newline;
    /** `*` */
    uint64_t star;
};

using Classifier = void (*)(const unsigned char * block, Masks & out);

/**
 * The offset of the first `\\`, `\r` or NUL in the `n` bytes at `p`, or
 * `n`: the bytes a string body cannot be copied over unchanged
 */
using EscapeFinder = size_t (*)(const char * p, size_t n);

/**
 * The best classifier for this CPU
 */
Classifier pickClassifier();

/**
 * The best escape finder for this CPU
 */
EscapeFinder pickEscapeFinder();

/**
 * Every escape finder this build has and this CPU can run, scalar first
 */
std::vector<std::pair<const char *, EscapeFinder>> allEscapeFinders();

/**
 * Every classifier this build has and this CPU can run, scalar first, for
 * checking them against each other
 */
std::vector<std::pair<const char *, Classifier>> allClassifiers();

} // namespace nix::lexer::simd
