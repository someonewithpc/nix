#pragma once
///@file
/**
 * Byte classification for the lexer, one 64-byte block at a time
 *
 * The classification is written once, against a small vector interface
 * that each instruction set implements; adding an instruction set means
 * adding one such backend and listing it in `pickClassifier`
 */

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
    /** `[$']`, where an indented string body stops */
    uint64_t indStop;
    /** `[\r\n]` */
    uint64_t newline;
    /** `*` */
    uint64_t star;
};

using Classifier = void (*)(const unsigned char * block, Masks & out);

/**
 * The best classifier for this CPU
 */
Classifier pickClassifier();

/**
 * Every classifier this build has and this CPU can run, scalar first, for
 * checking them against each other
 */
std::vector<std::pair<const char *, Classifier>> allClassifiers();

} // namespace nix::lexer::simd
