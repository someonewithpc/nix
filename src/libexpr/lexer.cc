#include "lexer.hh"
#include "lexer-simd.hh"
#include "work-pool.hh"

#include <array>
#include <cassert>
#include <bit>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

#include "nix/util/util.hh"

namespace nix::lexer {

namespace {

/* Token vectors that were used before, biggest first */
class TokenPool
{
    std::mutex m;
    std::vector<std::vector<Token>> spare;
    size_t bytes = 0;
    static constexpr size_t maxBytes = size_t(256) << 20;

public:
    std::vector<Token> get(size_t want)
    {
        std::lock_guard lock(m);
        /* The smallest one that is big enough, or else the biggest */
        size_t best = spare.size();
        for (size_t i = 0; i < spare.size(); i++)
            if (spare[i].capacity() >= want && (best == spare.size() || spare[i].capacity() < spare[best].capacity()))
                best = i;
        if (best == spare.size()) {
            if (spare.empty())
                return {};
            best = 0;
            for (size_t i = 1; i < spare.size(); i++)
                if (spare[i].capacity() > spare[best].capacity())
                    best = i;
        }
        auto v = std::move(spare[best]);
        spare.erase(spare.begin() + best);
        bytes -= v.capacity() * sizeof(Token);
        return v;
    }

    void put(std::vector<Token> && v)
    {
        size_t b = v.capacity() * sizeof(Token);
        if (b < 4096)
            return;
        v.clear();
        std::lock_guard lock(m);
        if (bytes + b > maxBytes)
            return;
        bytes += b;
        spare.push_back(std::move(v));
    }
};

TokenPool tokenPool;

enum CharClass : uint16_t {
    IdStart = 1 << 0,
    IdCont = 1 << 1,
    PathChar = 1 << 2,
    UriScheme = 1 << 3,
    UriChar = 1 << 4,
    Digit = 1 << 5,
    Space = 1 << 6,
    Letter = 1 << 7,
};

constexpr std::array<uint16_t, 256> makeClasses()
{
    std::array<uint16_t, 256> t{};
    auto add = [&](unsigned char c, uint16_t f) { t[c] |= f; };
    for (int c = 'a'; c <= 'z'; c++)
        for (auto u : {c, c - 'a' + 'A'})
            add(u, IdStart | IdCont | PathChar | UriScheme | UriChar | Letter);
    for (int c = '0'; c <= '9'; c++)
        add(c, IdCont | PathChar | UriScheme | UriChar | Digit);
    add('_', IdStart | IdCont | PathChar | UriChar);
    add('\'', IdCont | UriChar);
    add('-', IdCont | PathChar | UriScheme | UriChar);
    add('.', PathChar | UriScheme | UriChar);
    add('+', PathChar | UriScheme | UriChar);
    for (unsigned char c : std::string_view("%/?:@&=$,!~*"))
        add(c, UriChar);
    for (unsigned char c : std::string_view(" \t\r\n"))
        add(c, Space);
    return t;
}

constexpr auto classes = makeClasses();

inline bool is(unsigned char c, uint16_t f)
{
    return classes[c] & f;
}

/* Start conditions of the flex lexer */
enum State : uint8_t {
    Initial,
    Default,
    String,
    IndString,
    InPath,
    InPathSlash,
    /* On the stack of a speculative lexer: whatever state the chunk
       started in */
    Unknown,
};

/* The order of the flex rules, which breaks ties between matches of equal
   length */
enum Rule : uint8_t {
    RKeyword,
    REllipsis,
    ROperator,
    RId,
    RInt,
    RFloat,
    RDollarCurly,
    RRBrace,
    RLBrace,
    RQuote,
    RIndOpen,
    RPathSegDollar,
    RPath,
    RHPath,
    RSPath,
    RUri,
    RDocComment,
    RSpace,
    RLineComment,
    RBlockComment,
    RAny,
};

struct Match
{
    uint32_t len = 0;
    Rule rule = RAny;
    Kind kind = UNDEF;

    void offer(uint32_t l, Rule r, Kind k = UNDEF)
    {
        if (l > len || (l == len && l && r < rule)) {
            len = l;
            rule = r;
            kind = k;
        }
    }
};

struct Keyword
{
    std::string_view text;
    Kind kind;
};

constexpr Keyword keywords[] = {
    {"if", IF},
    {"then", THEN},
    {"else", ELSE},
    {"assert", ASSERT},
    {"with", WITH},
    {"let", LET},
    {"in", IN_KW},
    {"rec", REC},
    {"inherit", INHERIT},
    {"or", OR_KW},
};

Kind charKind(unsigned char c)
{
    switch (+c) {
    case 0:
        return EOF_;
    case '<':
        return LT;
    case '>':
        return GT;
    case '+':
        return PLUS;
    case '-':
        return MINUS;
    case '*':
        return STAR;
    case '/':
        return SLASH;
    case '?':
        return QUESTION;
    case ':':
        return COLON;
    case '@':
        return AT;
    case ';':
        return SEMICOLON;
    case '!':
        return BANG;
    case '.':
        return DOT;
    case '"':
        return QUOTE;
    case '(':
        return LPAREN;
    case ')':
        return RPAREN;
    case '{':
        return LBRACE;
    case '}':
        return RBRACE;
    case '[':
        return LBRACKET;
    case ']':
        return RBRACKET;
    case '=':
        return ASSIGN;
    case ',':
        return COMMA;
    default:
        return UNDEF;
    }
}

simd::Classifier classifier()
{
    static const simd::Classifier c = simd::pickClassifier();
    return c;
}

/* Tokens of one character that no longer token starts with */
constexpr std::array<Kind, 256> makeSingles()
{
    std::array<Kind, 256> t{};
    for (auto & k : t)
        k = NUM_KINDS;
    t[';'] = SEMICOLON;
    t['('] = LPAREN;
    t[')'] = RPAREN;
    t['['] = LBRACKET;
    t[']'] = RBRACKET;
    t[','] = COMMA;
    t['@'] = AT;
    t['?'] = QUESTION;
    t[':'] = COLON;
    t['*'] = STAR;
    return t;
}

constexpr auto singles = makeSingles();

class Lexer
{
    char * const s;
    const uint32_t n;
    LexerState & ls;
    Tokens out;

public:
    State state = Initial;
    std::vector<State> stack;
    uint32_t pos = 0;
    uint32_t start = 0;
    /* Past an error or the end of the input */
    bool done = false;

    /* Lexing a chunk that starts at a line in some default state that is
       not known yet; see `lexParallel` */
    bool spec = false;
    /* Leave escapes for later, so that lexers running on other parts of
       the buffer never see it change */
    bool defer = false;
    /* The `}` tokens of a speculative lexer that popped past the start of
       its chunk */
    std::vector<uint32_t> underflows;
    /* A speculative lexer counts `docDistance` up from here, which tells
       distances from the chunk apart from ones that need the distance at
       its start */
    static constexpr int64_t specBase = int64_t(1) << 40;
    /* The rules that see a distance of `specBase` plus 0 or 1: whether they
       record a doc comment depends on the distance at the start */
    std::vector<std::pair<int64_t, PosIdx>> docCandidates;
private:

    /* The flex location, including its stash, which `yyless(0)` rules
       restore */
    ParserLocation loc{0, 0, 0, 0};
    /* `LexerState::docCommentDistance`, wide enough not to overflow */
    int64_t docDistance = std::numeric_limits<int>::max();

    void push(State st)
    {
        stack.push_back(spec && stack.empty() ? Unknown : state);
        state = st;
    }

    void pop()
    {
        if (spec && stack.empty()) {
            underflows.push_back(out.tokens.size());
            state = Default;
            return;
        }
        assert(!stack.empty());
        state = stack.back();
        stack.pop_back();
        if (state == Unknown)
            state = Default;
    }

    /* `adjustLoc`, run before the action of every matched rule */
    void adjust(uint32_t len)
    {
        loc.stash();
        if (docDistance == 1) {
            ParserLocation doc{ls.lastDocCommentLoc.beginOffset, 0, 0, 0};
            ParserLocation docEnd{ls.lastDocCommentLoc.endOffset, 0, 0, 0};
            ls.positionToDocComment.emplace(ls.at(loc), DocComment{ls.at(doc), ls.at(docEnd)});
        } else if (spec && docDistance >= specBase && docDistance <= specBase + 1)
            docCandidates.emplace_back(docDistance - specBase, ls.at(loc));
        docDistance++;
        loc.beginOffset = loc.endOffset;
        loc.endOffset += len;
    }

    void emit(Kind k, StrFlavour f = Raw, uint32_t aux = 0)
    {
        out.tokens.push_back(
            Token{
                .kind = k,
                .flavour = f,
                .begin = uint32_t(loc.beginOffset),
                .end = uint32_t(loc.endOffset),
                .aux = aux,
            });
    }

    /* Stop at the first error: the parser throws when it gets there, and
       the flex lexer never got past it */
    bool error(LexError::Kind k, uint32_t offset, std::string text = {})
    {
        out.errors.push_back(LexError{k, offset, std::move(text)});
        emit(LEX_ERROR, Raw, out.errors.size() - 1);
        return false;
    }

    simd::Classifier classify;
    uint32_t maskBlock = UINT32_MAX;
    simd::Masks masks;

    const simd::Masks & masksFor(uint32_t block)
    {
        if (block != maskBlock) {
            uint32_t start = block << 6;
            /* Past the content are the two NULs, and then maybe nothing */
            if (start + 64 <= n + 2)
                classify(reinterpret_cast<const unsigned char *>(s + start), masks);
            else {
                unsigned char tail[64] = {};
                memcpy(tail, s + start, n + 2 - start);
                classify(tail, masks);
            }
            maskBlock = block;
        }
        return masks;
    }

    /* The first offset from `p` whose byte is (`in`) or is not (`!in`) in
       the class, or `n` */
    template<bool in>
    uint32_t scan(uint32_t p, uint64_t simd::Masks::*cls)
    {
        while (p < n) {
            uint32_t block = p >> 6;
            uint64_t m = masksFor(block).*cls;
            if (!in)
                m = ~m;
            m >>= p & 63;
            if (m)
                return std::min(p + uint32_t(std::countr_zero(m)), n);
            p = (block + 1) << 6;
        }
        return n;
    }

    uint32_t run(uint32_t p, uint16_t f) const
    {
        while (p < n && is(s[p], f))
            p++;
        return p;
    }

    /* `{PATH_CHAR}*(\/{PATH_CHAR}+)+\/?`, as a length from `p` */
    uint32_t pathLen(uint32_t p) const
    {
        uint32_t q = run(p, PathChar);
        bool segment = false;
        while (q + 1 < n && s[q] == '/' && is(s[q + 1], PathChar)) {
            q = run(q + 1, PathChar);
            segment = true;
        }
        if (!segment)
            return 0;
        if (q < n && s[q] == '/')
            q++;
        return q - p;
    }

    /* `{PATH_CHAR}*\/` */
    uint32_t pathSegLen(uint32_t p) const
    {
        uint32_t q = run(p, PathChar);
        return q < n && s[q] == '/' ? q + 1 - p : 0;
    }

    /* `\~(\/{PATH_CHAR}+)+\/?` */
    uint32_t hpathLen(uint32_t p) const
    {
        if (s[p] != '~')
            return 0;
        uint32_t q = p + 1;
        bool segment = false;
        while (q + 1 < n && s[q] == '/' && is(s[q + 1], PathChar)) {
            q = run(q + 1, PathChar);
            segment = true;
        }
        if (!segment)
            return 0;
        if (q < n && s[q] == '/')
            q++;
        return q - p;
    }

    /* `\<{PATH_CHAR}+(\/{PATH_CHAR}+)*\>` */
    uint32_t spathLen(uint32_t p) const
    {
        uint32_t q = run(p + 1, PathChar);
        if (q == p + 1)
            return 0;
        while (q + 1 < n && s[q] == '/' && is(s[q + 1], PathChar))
            q = run(q + 1, PathChar);
        return q < n && s[q] == '>' ? q + 1 - p : 0;
    }

    /* `[a-zA-Z][a-zA-Z0-9\+\-\.]*\:[...]+` */
    uint32_t uriLen(uint32_t p) const
    {
        uint32_t q = run(p + 1, UriScheme);
        if (q >= n || s[q] != ':')
            return 0;
        uint32_t r = run(q + 1, UriChar);
        return r > q + 1 ? r - p : 0;
    }

    /* `(([1-9][0-9]*\.[0-9]*)|(0?\.[0-9]+))([Ee][+-]?[0-9]+)?` */
    uint32_t floatLen(uint32_t p) const
    {
        uint32_t q = p;
        if (s[p] >= '1' && s[p] <= '9') {
            uint32_t r = run(p, Digit);
            if (r < n && s[r] == '.')
                q = run(r + 1, Digit);
        }
        uint32_t r = s[p] == '0' ? p + 1 : p;
        if (r + 1 < n && s[r] == '.' && is(s[r + 1], Digit))
            q = std::max(q, run(r + 1, Digit));
        if (q == p)
            return 0;
        if (q < n && (s[q] == 'e' || s[q] == 'E')) {
            uint32_t e = q + 1;
            if (e < n && (s[e] == '+' || s[e] == '-'))
                e++;
            uint32_t t = run(e, Digit);
            if (t > e)
                q = t;
        }
        return q - p;
    }

    /* The block comment rules, as a length, and whether it is a doc
       comment */
    uint32_t blockCommentLen(uint32_t p, bool & doc) const
    {
        if (p + 1 >= n || s[p + 1] != '*')
            return 0;
        for (uint32_t q = p + 2; q + 1 < n; q++)
            if (s[q] == '*' && s[q + 1] == '/') {
                /* A doc comment opens with a slash and two stars, then neither a
                   slash nor a star */
                doc = q > p + 2 && s[p + 2] == '*' && s[p + 3] != '/' && s[p + 3] != '*';
                return q + 2 - p;
            }
        return 0;
    }

public:
    /* In place, as `unescapeStr` in the flex lexer did; returns the new
       length */
    static uint32_t unescape(char * text, uint32_t len, bool & nullByte)
    {
        char * t = text;
        const char * c = text;
        const char * e = text + len;
        nullByte = false;
        while (c < e) {
            char ch = *c++;
            nullByte |= ch == '\0';
            if (ch == '\\') {
                ch = *c++;
                *t++ = ch == 'n' ? '\n' : ch == 'r' ? '\r' : ch == 't' ? '\t' : ch;
            } else if (ch == '\r') {
                *t++ = '\n';
                if (*c == '\n')
                    c++;
            } else
                *t++ = ch;
        }
        return t - text;
    }

private:

    /* Emit an unescaped string token over the current location, whose
       value starts at `p` */
    bool emitUnescaped(Kind k, uint32_t p, uint32_t len)
    {
        bool nullByte;
        if (defer) {
            if (!memchr(s + p, 0, len)) {
                emit(k, Escaped, len);
                return true;
            }
            std::string copy(s + p, len);
            copy.resize(unescape(copy.data(), len, nullByte));
            return error(LexError::NullByte, loc.beginOffset, std::move(copy));
        }
        uint32_t l = unescape(s + p, len, nullByte);
        if (nullByte)
            return error(LexError::NullByte, loc.beginOffset, std::string(s + p, l));
        emit(k, Unescaped, l);
        return true;
    }

    bool number(Kind k, uint32_t p, uint32_t len)
    {
        std::string_view text(s + p, len);
        if (k == INT_LIT) {
            auto v = string2Int<int64_t>(text);
            if (!v)
                return error(LexError::InvalidInteger, loc.beginOffset, std::string(text));
            out.numbers.push_back(uint64_t(*v));
        } else {
            char buf[64];
            std::string owned;
            const char * z;
            if (len < sizeof(buf)) {
                memcpy(buf, text.data(), len);
                buf[len] = 0;
                z = buf;
            } else {
                owned = std::string(text);
                z = owned.c_str();
            }
            errno = 0;
            double d = strtod(z, nullptr);
            if (errno != 0)
                return error(LexError::InvalidFloat, loc.beginOffset, std::string(text));
            out.numbers.push_back(std::bit_cast<uint64_t>(d));
        }
        emit(k, Raw, out.numbers.size() - 1);
        return true;
    }

    bool lexDefault(uint32_t & p);
    bool lexString(uint32_t & p);
    bool lexIndString(uint32_t & p);
    bool lexInPath(uint32_t & p);

public:
    Lexer(char * s, uint32_t n, LexerState & ls)
        : s(s)
        , n(n)
        , ls(ls)
        , classify(classifier())
    {
    }

    Tokens run(bool replBindings);

    /* Lex until `pos` reaches `limit`, or to the end of the input when
       `limit` is `n`; false once the lexer is done */
    bool runTo(uint32_t limit);

    /* Start a speculative lexer at `p`, the first token of a line */
    void startChunk(uint32_t p)
    {
        spec = true;
        defer = true;
        state = Default;
        pos = p;
        start = p;
        uint32_t b = p;
        while (b > 0 && is(s[b - 1], Space))
            b--;
        /* After the whitespace rule, which is the only one that can end
           here if the guess is right */
        loc = ParserLocation{int(b), int(p), int(b), int(p)};
        docDistance = specBase;
    }

    void reserve(uint32_t bytes)
    {
        size_t want = bytes / 4 + 16;
        if (out.tokens.capacity() < want) {
            out.tokens = tokenPool.get(want);
            out.tokens.reserve(want);
        }
    }

    void recycle()
    {
        tokenPool.put(std::move(out.tokens));
    }

    /* Take over the tokens of the speculative lexer of the chunk that
       starts where this one stopped, if its guesses hold */
    bool append(Lexer & c);

    Tokens take()
    {
        return std::move(out);
    }

    /* Unescape the strings that `defer` left */
    void unescapeDeferred()
    {
        for (auto & t : out.tokens)
            if (t.flavour == Escaped) {
                bool nullByte;
                t.aux = unescape(s + t.end - t.aux, t.aux, nullByte);
                t.flavour = Unescaped;
            }
    }
};

bool Lexer::lexDefault(uint32_t & p)
{
    unsigned char c = s[p];
    Match m;

    if (is(c, Space)) {
        uint32_t q = scan<false>(p, &simd::Masks::space);
        adjust(q - p);
        docDistance--;
        p = q;
        return true;
    }

    if (is(c, IdStart)) {
        uint32_t e = scan<false>(p + 1, &simd::Masks::ident);
        char d = s[e];
        /* Only these can make it a path or a URI instead */
        if (d != '/' && d != ':' && d != '.' && d != '+') {
            uint32_t l = e - p;
            Kind k = ID;
            if (l <= 7)
                for (auto & kw : keywords)
                    if (kw.text.size() == l && std::string_view(s + p, l) == kw.text) {
                        k = kw.kind;
                        break;
                    }
            adjust(l);
            emit(k);
            p = e;
            return true;
        }
    }

    if (is(c, Digit)) {
        uint32_t e = scan<false>(p + 1, &simd::Masks::digit);
        unsigned char d = s[e];
        if (!is(d, PathChar) && d != '/') {
            adjust(e - p);
            if (!number(INT_LIT, p, e - p)) {
                p = e;
                return false;
            }
            p = e;
            return true;
        }
    }

    if (Kind k = singles[c]; k != NUM_KINDS) {
        adjust(1);
        emit(k);
        p++;
        return true;
    }

    switch (+c) {
    case '#': {
        uint32_t q = scan<true>(p + 1, &simd::Masks::newline);
        adjust(q - p);
        docDistance--;
        p = q;
        return true;
    }
    case '/': {
        bool doc = false;
        m.offer(blockCommentLen(p, doc), RBlockComment);
        if (m.len && doc)
            m.rule = RDocComment;
        if (p + 1 < n && s[p + 1] == '/')
            m.offer(2, ROperator, UPDATE);
        m.offer(pathLen(p), RPath, PATH);
        if (p + 2 < n && s[p + 1] == '$' && s[p + 2] == '{')
            m.offer(3, RPathSegDollar, PATH);
        break;
    }
    case '.':
        if (p + 2 < n && s[p + 1] == '.' && s[p + 2] == '.')
            m.offer(3, REllipsis, ELLIPSIS);
        m.offer(floatLen(p), RFloat, FLOAT_LIT);
        break;
    case '=':
        if (p + 1 < n && s[p + 1] == '=')
            m.offer(2, ROperator, EQ);
        break;
    case '!':
        if (p + 1 < n && s[p + 1] == '=')
            m.offer(2, ROperator, NEQ);
        break;
    case '<':
        if (p + 1 < n && s[p + 1] == '=')
            m.offer(2, ROperator, LEQ);
        if (p + 1 < n && s[p + 1] == '|')
            m.offer(2, ROperator, PIPE_FROM);
        m.offer(spathLen(p), RSPath, SPATH);
        break;
    case '>':
        if (p + 1 < n && s[p + 1] == '=')
            m.offer(2, ROperator, GEQ);
        break;
    case '&':
        if (p + 1 < n && s[p + 1] == '&')
            m.offer(2, ROperator, AND);
        break;
    case '|':
        if (p + 1 < n && s[p + 1] == '|')
            m.offer(2, ROperator, OR);
        if (p + 1 < n && s[p + 1] == '>')
            m.offer(2, ROperator, PIPE_INTO);
        break;
    case '-':
        if (p + 1 < n && s[p + 1] == '>')
            m.offer(2, ROperator, IMPL);
        break;
    case '+':
        if (p + 1 < n && s[p + 1] == '+')
            m.offer(2, ROperator, CONCAT);
        break;
    case '$':
        if (p + 1 < n && s[p + 1] == '{')
            m.offer(2, RDollarCurly, DOLLAR_CURLY);
        break;
    case '}':
        m.offer(1, RRBrace, RBRACE);
        break;
    case '{':
        m.offer(1, RLBrace, LBRACE);
        break;
    case '"':
        m.offer(1, RQuote, QUOTE);
        break;
    case '\'':
        if (p + 1 < n && s[p + 1] == '\'') {
            uint32_t q = p + 2;
            while (q < n && s[q] == ' ')
                q++;
            m.offer(q < n && s[q] == '\n' ? q + 1 - p : 2, RIndOpen, IND_STRING_OPEN);
        }
        break;
    case '~':
        m.offer(hpathLen(p), RHPath, HPATH);
        if (p + 3 < n && s[p + 1] == '/' && s[p + 2] == '$' && s[p + 3] == '{')
            m.offer(4, RPathSegDollar, HPATH);
        break;
    default:
        break;
    }

    if (is(c, PathChar)) {
        if (is(c, IdStart)) {
            uint32_t l = run(p, IdCont) - p;
            m.offer(l, RId, ID);
            for (auto & kw : keywords)
                if (kw.text.size() == l && std::string_view(s + p, l) == kw.text) {
                    m.offer(l, RKeyword, kw.kind);
                    break;
                }
        }
        if (is(c, Digit)) {
            m.offer(run(p, Digit) - p, RInt, INT_LIT);
            m.offer(floatLen(p), RFloat, FLOAT_LIT);
        }
        m.offer(pathLen(p), RPath, PATH);
        uint32_t seg = pathSegLen(p);
        if (seg && p + seg + 1 < n && s[p + seg] == '$' && s[p + seg + 1] == '{')
            m.offer(seg + 2, RPathSegDollar, PATH);
        if (is(c, Letter))
            m.offer(uriLen(p), RUri, URI);
    }

    m.offer(1, RAny, charKind(c));

    uint32_t len = m.len;
    switch (+m.rule) {
    case RDocComment:
        adjust(len);
        docDistance = 0;
        ls.lastDocCommentLoc.beginOffset = loc.beginOffset;
        ls.lastDocCommentLoc.endOffset = loc.endOffset;
        break;
    case RBlockComment:
        adjust(len);
        docDistance--;
        break;
    case RPathSegDollar: {
        /* `yyless(0)` into PATH_START, which matches the segment again */
        adjust(len);
        loc.unstash();
        uint32_t seg = len - 2;
        adjust(seg);
        push(InPathSlash);
        emit(m.kind);
        len = seg;
        break;
    }
    case RPath:
    case RHPath:
        adjust(len);
        push(s[p + len - 1] == '/' ? InPathSlash : InPath);
        emit(m.kind);
        break;
    case RDollarCurly:
    case RLBrace:
        adjust(len);
        push(Default);
        emit(m.kind);
        break;
    case RRBrace:
        adjust(len);
        if (state != Initial)
            pop();
        emit(m.kind);
        break;
    case RQuote:
        adjust(len);
        push(String);
        emit(m.kind);
        break;
    case RIndOpen:
        adjust(len);
        push(IndString);
        emit(m.kind);
        break;
    case ROperator:
        adjust(len);
        if (m.kind == PIPE_FROM || m.kind == PIPE_INTO)
            if (!experimentalFeatureSettings.isEnabled(Xp::PipeOperators)) {
                p += len;
                return error(LexError::PipeOperators, loc.beginOffset);
            }
        emit(m.kind);
        break;
    case RInt:
    case RFloat:
        adjust(len);
        if (!number(m.kind, p, len)) {
            p += len;
            return false;
        }
        break;
    case RKeyword:
    case REllipsis:
    case RId:
    case RSPath:
    case RUri:
        adjust(len);
        emit(m.kind);
        break;
    case RAny:
        adjust(len);
        emit(m.kind);
        if (m.kind == EOF_) {
            /* A NUL byte, which Bison takes for the end of the input */
            p += len;
            return false;
        }
        break;
    default:
        assert(false);
    }
    p += len;
    return true;
}

bool Lexer::lexString(uint32_t & p)
{
    if (s[p] == '$' && p + 1 < n && s[p + 1] == '{') {
        adjust(2);
        push(Default);
        emit(DOLLAR_CURLY);
        p += 2;
        return true;
    }
    if (s[p] == '"') {
        adjust(1);
        pop();
        emit(QUOTE);
        p += 1;
        return true;
    }

    /* `([^\$\"\\]|\$[^\{\"\\]|\\{ANY}|\$\\{ANY})*` */
    uint32_t q = p;
    while (q < n) {
        char c = s[q];
        if (c == '"')
            break;
        if (c == '\\') {
            if (q + 1 >= n)
                break;
            q += 2;
        } else if (c == '$') {
            if (q + 1 >= n)
                break;
            char d = s[q + 1];
            if (d == '{' || d == '"')
                break;
            if (d == '\\') {
                if (q + 2 >= n)
                    break;
                q += 3;
            } else
                q += 2;
        } else
            q++;
    }

    uint32_t len = q - p;
    /* The rule with the trailing context `\$/\"` */
    if (q + 1 < n && s[q] == '$' && s[q + 1] == '"')
        len++;

    if (len) {
        adjust(len);
        if (!emitUnescaped(STR, p, len))
            return false;
        p += len;
        return true;
    }

    /* `\$|\\|\$\\` at the end of the input */
    uint32_t l = s[p] == '$' && p + 1 < n && s[p + 1] == '\\' ? 2 : 1;
    adjust(l);
    emit(EOF_);
    p += l;
    return false;
}

bool Lexer::lexIndString(uint32_t & p)
{
    char c = s[p];
    if (c == '$' && p + 1 < n && s[p + 1] == '{') {
        adjust(2);
        push(Default);
        emit(DOLLAR_CURLY);
        p += 2;
        return true;
    }
    if (c == '\'' && p + 1 < n && s[p + 1] == '\'') {
        char d = p + 2 < n ? s[p + 2] : 0;
        if (p + 2 < n && d == '\'') {
            adjust(3);
            emit(IND_STR, Literal, LitQuotes);
            p += 3;
            return true;
        }
        if (p + 2 < n && d == '$') {
            adjust(3);
            emit(IND_STR, Literal, LitDollar);
            p += 3;
            return true;
        }
        if (p + 3 < n && d == '\\') {
            adjust(4);
            if (!emitUnescaped(IND_STR, p + 2, 2))
                return false;
            p += 4;
            return true;
        }
        adjust(2);
        pop();
        emit(IND_STRING_CLOSE);
        p += 2;
        return true;
    }

    /* `([^\$\']|\$[^\{\']|\'[^\'\$])+` */
    uint32_t q = p;
    bool nullByte = false;
    while (q < n) {
        char e = s[q];
        if (e == '$' || e == '\'') {
            if (q + 1 >= n)
                break;
            char f = s[q + 1];
            if (e == '$' ? f == '{' || f == '\'' : f == '\'' || f == '$')
                break;
            nullByte |= f == '\0';
            q += 2;
        } else {
            nullByte |= e == '\0';
            q++;
        }
    }
    if (q > p) {
        adjust(q - p);
        if (nullByte)
            return error(LexError::NullByte, loc.beginOffset, std::string(s + p, q - p));
        emit(IND_STR, Indented);
        p = q;
        return true;
    }

    /* A lone `$` or `'` */
    adjust(1);
    emit(IND_STR, Literal, c == '$' ? LitDollar : LitQuote);
    p += 1;
    return true;
}

bool Lexer::lexInPath(uint32_t & p)
{
    if (s[p] == '$' && p + 1 < n && s[p + 1] == '{') {
        adjust(2);
        pop();
        push(InPath);
        push(Default);
        emit(DOLLAR_CURLY);
        p += 2;
        return true;
    }
    uint32_t len = std::max({pathLen(p), pathSegLen(p), run(p, PathChar) - p});
    if (len) {
        adjust(len);
        pop();
        push(s[p + len - 1] == '/' ? InPathSlash : InPath);
        emit(STR);
        p += len;
        return true;
    }
    adjust(1);
    if (state == InPathSlash)
        return error(LexError::TrailingSlash, loc.beginOffset);
    /* `yyless(0)`: end the path and lex this character again */
    pop();
    loc.unstash();
    emit(PATH_END);
    return true;
}

Tokens Lexer::run(bool replBindings)
{
    reserve(n);
    if (replBindings && n > 0) {
        adjust(1);
        loc.unstash();
        emit(REPL_BINDINGS);
    }
    runTo(n);
    return std::move(out);
}

bool Lexer::runTo(uint32_t limit)
{
    while (!done) {
        if (pos >= limit && limit < n)
            return true;
        if (pos >= n) {
            /* The `<<EOF>>` rules, which do not adjust the location */
            switch (+state) {
            case InPath:
                pop();
                loc.unstash();
                emit(PATH_END);
                continue;
            case InPathSlash:
                error(LexError::TrailingSlash, loc.beginOffset);
                done = true;
                return false;
            default:
                emit(EOF_);
                done = true;
                return false;
            }
        }

        bool more;
        switch (+state) {
        case Initial:
        case Default:
            more = lexDefault(pos);
            break;
        case String:
            more = lexString(pos);
            break;
        case IndString:
            more = lexIndString(pos);
            break;
        case InPath:
        case InPathSlash:
            more = lexInPath(pos);
            break;
        default:
            assert(false);
        }
        if (!more)
            done = true;
    }
    return false;
}

bool Lexer::append(Lexer & c)
{
    if (done || pos != c.start || (state != Default && state != Initial))
        return false;

    /* Replay the pops past the start of the chunk on the real stack; each
       must land in a default state for the chunk to have been lexed right */
    auto realStack = stack;
    State bottom = state;
    for (size_t i = 0; i < c.underflows.size(); i++) {
        if (bottom == Initial)
            continue;
        assert(!realStack.empty());
        bottom = realStack.back();
        realStack.pop_back();
        if (bottom != Default && bottom != Initial)
            return false;
    }

    if (c.stack.empty())
        state = bottom;
    else {
        assert(c.stack.front() == Unknown);
        realStack.push_back(bottom);
        realStack.insert(realStack.end(), c.stack.begin() + 1, c.stack.end());
        state = c.state;
    }
    stack = std::move(realStack);

    for (auto & [r, key] : c.docCandidates)
        if (docDistance + r == 1) {
            ParserLocation doc{ls.lastDocCommentLoc.beginOffset, 0, 0, 0};
            ParserLocation docEnd{ls.lastDocCommentLoc.endOffset, 0, 0, 0};
            ls.positionToDocComment.emplace(key, DocComment{ls.at(doc), ls.at(docEnd)});
        }
    for (auto & [key, doc] : c.ls.positionToDocComment)
        ls.positionToDocComment.emplace(key, doc);
    if (c.docDistance >= specBase / 2)
        docDistance += c.docDistance - specBase;
    else {
        docDistance = c.docDistance;
        ls.lastDocCommentLoc = c.ls.lastDocCommentLoc;
    }

    uint32_t numbers = out.numbers.size();
    uint32_t errors = out.errors.size();
    out.tokens.reserve(out.tokens.size() + c.out.tokens.size());
    for (auto t : c.out.tokens) {
        if (t.kind == INT_LIT || t.kind == FLOAT_LIT)
            t.aux += numbers;
        else if (t.kind == LEX_ERROR)
            t.aux += errors;
        out.tokens.push_back(t);
    }
    out.numbers.insert(out.numbers.end(), c.out.numbers.begin(), c.out.numbers.end());
    for (auto & e : c.out.errors)
        out.errors.push_back(std::move(e));

    loc = c.loc;
    pos = c.pos;
    done = c.done;
    return true;
}

/* Below this a file is lexed on one thread */
uint32_t minChunk()
{
    static const uint32_t v = [] {
        auto e = getenv("NIX_LEXER_MIN_CHUNK");
        return e ? uint32_t(std::stoul(e)) : uint32_t(1) << 20;
    }();
    return v;
}

unsigned maxChunks()
{
    static const unsigned v = [] {
        if (auto e = getenv("NIX_LEXER_THREADS"))
            return unsigned(std::stoul(e));
        return std::max(1u, std::thread::hardware_concurrency() * 3 / 4);
    }();
    return v;
}

/* Split the input at the first tokens of lines and lex every chunk at
   once, each one guessing that it starts in a default state. The chunks
   are then joined in order: a chunk whose guess held is taken as it is,
   and one whose guess did not is lexed again from where the previous one
   stopped */
Tokens lexParallel(char * text, uint32_t length, LexerState & ls, unsigned chunks)
{
    std::vector<uint32_t> cuts{0};
    for (unsigned k = 1; k < chunks; k++) {
        uint32_t c = std::max(uint32_t(uint64_t(length) * k / chunks), cuts.back() + 1);
        auto nl = static_cast<const char *>(memchr(text + c - 1, '\n', length - (c - 1)));
        c = nl ? nl - text + 1 : length;
        while (c < length && is(text[c], Space))
            c++;
        if (c >= length)
            break;
        cuts.push_back(c);
    }
    cuts.push_back(length);
    size_t k = cuts.size() - 1;

    std::vector<DocCommentMap> maps(k);
    std::vector<LexerState> states;
    states.reserve(k);
    std::vector<Lexer> lexers;
    lexers.reserve(k);
    lexers.emplace_back(text, length, ls);
    lexers[0].defer = true;
    lexers[0].reserve(length);
    for (size_t i = 1; i < k; i++) {
        states.push_back(
            LexerState{
                .positionToDocComment = maps[i],
                .positions = ls.positions,
                .origin = ls.origin,
            });
        lexers.emplace_back(text, length, states.back());
        lexers[i].startChunk(cuts[i]);
        lexers[i].reserve(cuts[i + 1] - cuts[i]);
    }

    WorkPool::shared().run(k, [&](size_t i) { lexers[i].runTo(cuts[i + 1]); }, maxChunks() - 1);

    auto & real = lexers[0];
    real.reserve(length);
    for (size_t i = 1; i < k; i++) {
        if (!real.append(lexers[i]))
            real.runTo(cuts[i + 1]);
        lexers[i].recycle();
    }
    real.unescapeDeferred();
    return real.take();
}

} // namespace

const char * kindName(Kind k)
{
    switch (+k) {
    case EOF_:
        return "end of file";
    case UNDEF:
        return "invalid token";
    case ID:
        return "identifier";
    case STR:
        return "string";
    case IND_STR:
        return "indented string";
    case INT_LIT:
        return "integer";
    case FLOAT_LIT:
        return "floating-point literal";
    case PATH:
        return "path";
    case HPATH:
        return "'~/…' path";
    case SPATH:
        return "'<…>' path";
    case PATH_END:
        return "end of path";
    case URI:
        return "URI";
    case IF:
        return "'if'";
    case THEN:
        return "'then'";
    case ELSE:
        return "'else'";
    case ASSERT:
        return "'assert'";
    case WITH:
        return "'with'";
    case LET:
        return "'let'";
    case IN_KW:
        return "'in'";
    case REC:
        return "'rec'";
    case INHERIT:
        return "'inherit'";
    case EQ:
        return "'=='";
    case NEQ:
        return "'!='";
    case LEQ:
        return "'<='";
    case GEQ:
        return "'>='";
    case UPDATE:
        return "'//'";
    case CONCAT:
        return "'++'";
    case AND:
        return "'&&'";
    case OR:
        return "'||'";
    case IMPL:
        return "'->'";
    case OR_KW:
        return "'or'";
    case PIPE_FROM:
        return "'<|'";
    case PIPE_INTO:
        return "'|>'";
    case DOLLAR_CURLY:
        return "'${'";
    case IND_STRING_OPEN:
        return "start of an indented string";
    case IND_STRING_CLOSE:
        return "end of an indented string";
    case ELLIPSIS:
        return "'...'";
    case REPL_BINDINGS:
        return "start of REPL bindings";
    case LT:
        return "'<'";
    case GT:
        return "'>'";
    case PLUS:
        return "'+'";
    case MINUS:
        return "'-'";
    case STAR:
        return "'*'";
    case SLASH:
        return "'/'";
    case QUESTION:
        return "'?'";
    case COLON:
        return "':'";
    case AT:
        return "'@'";
    case SEMICOLON:
        return "';'";
    case BANG:
        return "'!'";
    case DOT:
        return "'.'";
    case QUOTE:
        return "'\"'";
    case LPAREN:
        return "'('";
    case RPAREN:
        return "')'";
    case LBRACE:
        return "'{'";
    case RBRACE:
        return "'}'";
    case LBRACKET:
        return "'['";
    case RBRACKET:
        return "']'";
    case ASSIGN:
        return "'='";
    case COMMA:
        return "','";
    default:
        return "invalid token";
    }
}

Tokens lex(char * text, size_t length, LexerState & lexerState, bool replBindings)
{
    if (!replBindings && length >= 2 * uint64_t(minChunk())) {
        unsigned chunks = std::min<uint64_t>(maxChunks(), length / minChunk());
        if (chunks > 1)
            return lexParallel(text, length, lexerState, chunks);
    }
    Lexer lexer(text, length, lexerState);
    auto tokens = lexer.run(replBindings);
    return tokens;
}

void recycle(Tokens && tokens)
{
    tokenPool.put(std::move(tokens.tokens));
}

} // namespace nix::lexer
