#include <bit>
#include <random>

#include <gtest/gtest.h>

#include "nix/util/json-text.hh"

namespace nix {

using nlohmann::json;

/* The writer must give `dump()`'s text, or throw its error. */
static void expectSameAsDump(const json & j)
{
    JSONTextWriter w;
    w.value(j);
    std::string expected;
    try {
        expected = j.dump();
    } catch (json::exception & e) {
        try {
            w.check();
            FAIL() << "no error, expected: " << e.what();
        } catch (json::exception & e2) {
            EXPECT_EQ(std::string(e2.what()), std::string(e.what()));
        }
        return;
    }
    EXPECT_NO_THROW(w.check());
    EXPECT_EQ(w.text(), expected);
}

static std::string randomBytes(std::mt19937 & rng)
{
    static const unsigned char interesting[] = {
        0x00, 0x01, 0x08, 0x09, 0x0A, 0x0C, 0x0D, 0x1F, 0x20, '"',  '\\', '/',  'a',  0x7F, 0x80, 0xBF, 0xC0, 0xC1,
        0xC2, 0xDF, 0xE0, 0xE1, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF3, 0xF4, 0xF5, 0xFF, 0x9F, 0xA0, 0x8F, 0x90,
    };
    std::string s;
    auto len = rng() % 12;
    for (size_t i = 0; i < len; ++i) {
        switch (rng() % 4) {
        case 0:
            s += static_cast<char>(rng() % 256);
            break;
        case 1:
            s += static_cast<char>(interesting[rng() % sizeof(interesting)]);
            break;
        case 2:
            s += "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80" + (rng() % 9);
            break;
        default:
            s += static_cast<char>('a' + rng() % 26);
        }
    }
    return s;
}

static json randomJSON(std::mt19937 & rng, int depth)
{
    static const double floats[] = {
        0.0,
        -0.0,
        1.0,
        0.1,
        1e21,
        1e-7,
        123456789012345678.0,
        1.5e300,
        -2.5,
        3.14159,
        1e16,
        1e15,
    };
    switch (rng() % (depth > 3 ? 6 : 8)) {
    case 0:
        return nullptr;
    case 1:
        return rng() % 2 == 0;
    case 2: {
        int64_t n = (int64_t(rng()) << 32) | rng();
        if (rng() % 2)
            n = int64_t(rng() % 2000) - 1000;
        return n;
    }
    case 3:
        if (rng() % 3 == 0)
            return std::bit_cast<double>((uint64_t(rng()) << 32) | rng());
        return floats[rng() % std::size(floats)];
    case 4:
    case 5:
        return randomBytes(rng);
    case 6: {
        json a = json::array();
        for (auto n = rng() % 5; n > 0; --n)
            a.push_back(randomJSON(rng, depth + 1));
        return a;
    }
    default: {
        json o = json::object();
        for (auto n = rng() % 5; n > 0; --n)
            o[randomBytes(rng)] = randomJSON(rng, depth + 1);
        return o;
    }
    }
}

TEST(JSONTextWriter, matchesDumpOnRandomValues)
{
    std::mt19937 rng(7);
    for (int i = 0; i < 50000; ++i)
        expectSameAsDump(randomJSON(rng, 0));
}

TEST(JSONTextWriter, everyByteAndPair)
{
    for (int a = 0; a < 256; ++a) {
        expectSameAsDump(std::string(1, char(a)));
        for (int b = 0; b < 256; ++b)
            expectSameAsDump(std::string{char(a), char(b)});
    }
}

TEST(JSONTextWriter, numbers)
{
    for (auto d : {0.0, -0.0, 1.0, 1e100, 5e-324, 1.7976931348623157e308, 0.1 + 0.2, 1.0 / 0.0, 0.0 / 0.0})
        expectSameAsDump(d);
    for (auto n : {std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max(), int64_t(0), int64_t(-1)})
        expectSameAsDump(n);
}

TEST(JSONTextWriter, firstInvalidStringWins)
{
    json j = json::object();
    j["a"] = "ok";
    j["b"] = "bad\xff";
    j["c\xfe"] = "bad\xc3";
    expectSameAsDump(j);
}

} // namespace nix
