#pragma once
///@file

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace nix {

/**
 * Writes JSON text byte for byte as nlohmann's `dump()` without
 * indentation would, without building a tree first.
 *
 * Invalid UTF-8 does not throw where it is written. The writer keeps the
 * first value that has it, and `check` throws the error `dump()` would
 * have thrown, so that the caller can raise it where `dump()` used to run.
 */
class JSONTextWriter
{
    std::string out;
    bool needComma = false;
    std::optional<nlohmann::json> invalid;

    void separate()
    {
        if (needComma)
            out += ',';
    }

    void escape(std::string_view s);

public:
    void null()
    {
        separate();
        out += "null";
        needComma = true;
    }

    void boolean(bool b)
    {
        separate();
        out += b ? "true" : "false";
        needComma = true;
    }

    void integer(int64_t n);

    void floating(double d);

    void string(std::string_view s)
    {
        separate();
        escape(s);
        needComma = true;
    }

    void beginObject()
    {
        separate();
        out += '{';
        needComma = false;
    }

    void key(std::string_view k)
    {
        separate();
        escape(k);
        out += ':';
        needComma = false;
    }

    void endObject()
    {
        out += '}';
        needComma = true;
    }

    void beginArray()
    {
        separate();
        out += '[';
        needComma = false;
    }

    void endArray()
    {
        out += ']';
        needComma = true;
    }

    void value(const nlohmann::json & j);

    /**
     * Throws what `dump()` would have thrown for the text written so far,
     * if anything.
     */
    void check() const;

    /**
     * The text. Only meaningful once `check` has passed.
     */
    const std::string & text() const
    {
        return out;
    }

    std::string && take() &&
    {
        return std::move(out);
    }
};

} // namespace nix
