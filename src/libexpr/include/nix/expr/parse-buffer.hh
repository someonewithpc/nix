#pragma once
///@file

#include <cstddef>
#include <exception>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "nix/util/source-path.hh"

namespace nix {

/**
 * A source text that the parser can lex in place
 *
 * The text starts on a 64-byte boundary and is followed by at least
 * `padding` zero bytes, so the lexer can read whole 64-byte blocks and
 * look ahead past the last byte without checking for the end. Files of a
 * few MB and up are in huge pages where the kernel allows it
 */
class ParseBuffer
{
    char * p = nullptr;
    size_t len = 0;
    size_t cap = 0;

public:
    static constexpr size_t padding = 64;

    /**
     * Room for `len` bytes of text, which the caller fills
     */
    explicit ParseBuffer(size_t len);

    explicit ParseBuffer(std::string_view s);

    ParseBuffer(ParseBuffer && other) noexcept;
    ParseBuffer & operator=(ParseBuffer && other) noexcept;
    ParseBuffer(const ParseBuffer &) = delete;
    ParseBuffer & operator=(const ParseBuffer &) = delete;
    ~ParseBuffer();

    char * data()
    {
        return p;
    }

    size_t size() const
    {
        return len;
    }
};

/**
 * Read a file into a parse buffer
 *
 * `path` should have its symlinks resolved already
 */
ParseBuffer readParseBuffer(const SourcePath & path);

/**
 * Read several files at once: one io_uring submission opens, reads and
 * closes a batch of them where the kernel allows it. Each result is the
 * file's buffer, or what reading it threw
 */
std::vector<std::variant<ParseBuffer, std::exception_ptr>> readParseBuffers(std::span<const SourcePath> paths);

} // namespace nix
