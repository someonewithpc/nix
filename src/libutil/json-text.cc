#include "nix/util/json-text.hh"
#include "nix/util/error.hh"

#include <charconv>

namespace nix {

/**
 * Length of the well-formed UTF-8 sequence at the start of `s`, or 0.
 * Well-formed is what nlohmann's decoder accepts: no overlong forms, no
 * surrogates, nothing past U+10FFFF.
 */
static size_t utf8SequenceLength(std::string_view s)
{
    auto at = [&](size_t i) -> unsigned char { return i < s.size() ? static_cast<unsigned char>(s[i]) : 0; };
    auto cont = [&](size_t i, unsigned char lo = 0x80, unsigned char hi = 0xBF) {
        auto c = at(i);
        return i < s.size() && c >= lo && c <= hi;
    };
    auto c = at(0);
    if (c >= 0xC2 && c <= 0xDF)
        return cont(1) ? 2 : 0;
    if (c == 0xE0)
        return cont(1, 0xA0) && cont(2) ? 3 : 0;
    if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF)
        return cont(1) && cont(2) ? 3 : 0;
    if (c == 0xED)
        return cont(1, 0x80, 0x9F) && cont(2) ? 3 : 0;
    if (c == 0xF0)
        return cont(1, 0x90) && cont(2) && cont(3) ? 4 : 0;
    if (c >= 0xF1 && c <= 0xF3)
        return cont(1) && cont(2) && cont(3) ? 4 : 0;
    if (c == 0xF4)
        return cont(1, 0x80, 0x8F) && cont(2) && cont(3) ? 4 : 0;
    return 0;
}

void JSONTextWriter::escape(std::string_view s)
{
    static constexpr char hex[] = "0123456789abcdef";
    out += '"';
    size_t i = 0, run = 0;
    auto flush = [&] { out.append(s.data() + run, i - run); };
    while (i < s.size()) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c != '"' && c != '\\' && c < 0x80) {
            ++i;
            continue;
        }
        if (c >= 0x80) {
            auto len = utf8SequenceLength(s.substr(i));
            if (len == 0) {
                if (!invalid)
                    invalid.emplace(std::string{s});
                len = 1;
            }
            i += len;
            continue;
        }
        flush();
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
        run = ++i;
    }
    flush();
    out += '"';
}

void JSONTextWriter::integer(int64_t n)
{
    separate();
    char buf[24];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), n);
    out.append(buf, end);
    needComma = true;
}

void JSONTextWriter::floating(double d)
{
    separate();
    /* nlohmann's float formatting is its own; let it do it. */
    out += nlohmann::json(d).dump();
    needComma = true;
}

void JSONTextWriter::value(const nlohmann::json & j)
{
    using nlohmann::json;
    switch (j.type()) {
    case json::value_t::object:
        beginObject();
        for (auto & [k, v] : j.items()) {
            key(k);
            value(v);
        }
        endObject();
        break;
    case json::value_t::array:
        beginArray();
        for (auto & v : j)
            value(v);
        endArray();
        break;
    case json::value_t::string:
        string(j.get_ref<const json::string_t &>());
        break;
    case json::value_t::boolean:
        boolean(j.get<bool>());
        break;
    case json::value_t::number_integer:
        integer(j.get<int64_t>());
        break;
    case json::value_t::null:
        null();
        break;
    case json::value_t::number_unsigned:
    case json::value_t::number_float:
    case json::value_t::binary:
    case json::value_t::discarded:
        separate();
        out += j.dump();
        needComma = true;
        break;
    }
}

void JSONTextWriter::check() const
{
    if (invalid) {
        (void) invalid->dump();
        unreachable();
    }
}

} // namespace nix
