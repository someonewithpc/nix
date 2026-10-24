// No include guard: lexer-simd.cc includes this once per backend, each
// time inside that backend's target region
// NOLINTBEGIN(llvm-header-guard)

/**
 * Classification over any vector type that offers the operations below
 *
 * `V` holds `V::width` bytes; `V::mask` turns a byte-wise comparison
 * result into a bitmask of `V::width` bits
 */
template<class V>
inline void classifyWith(const unsigned char * block, Masks & out)
{
    out = {};
    for (unsigned i = 0; i < 64; i += V::width) {
        V x = V::load(block + i);
        V lower = V::or_(x, V::splat(0x20));
        V letter = V::inRange(lower, 'a', 'z');
        V digit = V::inRange(x, '0', '9');
        V alnum = V::or_(letter, digit);
        V under = V::eq(x, '_');
        V minus = V::eq(x, '-');
        V dollar = V::eq(x, '$');
        V nl = V::or_(V::eq(x, '\n'), V::eq(x, '\r'));
        V space = V::or_(nl, V::or_(V::eq(x, ' '), V::eq(x, '\t')));
        V both = V::or_(alnum, V::or_(under, minus));
        V ident = V::or_(both, V::eq(x, '\''));
        V path = V::or_(both, V::or_(V::eq(x, '.'), V::eq(x, '+')));
        V strStop = V::or_(dollar, V::or_(V::eq(x, '"'), V::eq(x, '\\')));
        V indStop = V::or_(dollar, V::eq(x, '\''));
        auto shift = [&](uint64_t m) { return m << i; };
        out.space |= shift(V::mask(space));
        out.ident |= shift(V::mask(ident));
        out.path |= shift(V::mask(path));
        out.digit |= shift(V::mask(digit));
        out.strStop |= shift(V::mask(strStop));
        out.indStop |= shift(V::mask(indStop));
        out.newline |= shift(V::mask(nl));
        out.star |= shift(V::mask(V::eq(x, '*')));
    }
}

// NOLINTEND(llvm-header-guard)
