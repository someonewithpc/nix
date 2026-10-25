/* The parser is very performance sensitive and loses out on a lot of
   performance even with basic stdlib assertions. Since those don't affect
   ABI we can disable those just for this file */
#if defined(_GLIBCXX_ASSERTIONS) && !defined(_GLIBCXX_DEBUG)
#  undef _GLIBCXX_ASSERTIONS
#endif

#include "nix/expr/parser.hh"
#include "nix/expr/diagnose.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/parser-state.hh"
#include "work-pool.hh"
#include "nix/util/finally.hh"
#include "nix/util/users.hh"
#include "nix/util/util.hh"

#include "lexer.hh"

#include <algorithm>
#include <bit>
#include <cstring>
#include <exception>
#include <optional>
#include <thread>

namespace nix {

namespace {

using namespace lexer;

/* Binding power of the binary operators, as in the precedence declarations
   of the Bison grammar */
enum Prec : int8_t {
    PNone = 0,
    PImpl = 1,
    POr = 2,
    PAnd = 3,
    PEq = 4,
    PCmp = 5,
    PUpdate = 6,
    PNot = 7,
    PAdd = 8,
    PMul = 9,
    PConcat = 10,
    PHasAttr = 11,
    PNegate = 12,
};

constexpr std::array<Prec, NUM_KINDS> makePrecs()
{
    std::array<Prec, NUM_KINDS> t{};
    t[IMPL] = PImpl;
    t[OR] = POr;
    t[AND] = PAnd;
    t[EQ] = t[NEQ] = PEq;
    t[LT] = t[GT] = t[LEQ] = t[GEQ] = PCmp;
    t[UPDATE] = PUpdate;
    t[PLUS] = t[MINUS] = PAdd;
    t[STAR] = t[SLASH] = PMul;
    t[CONCAT] = PConcat;
    t[QUESTION] = PHasAttr;
    return t;
}

constexpr auto precs = makePrecs();

bool rightAssoc(Kind k)
{
    return k == IMPL || k == UPDATE || k == CONCAT;
}

/* `?` is declared non-associative too, but its right side is an attribute
   path, so `a ? b ? c` never reaches the conflict that would reject it */
bool nonAssoc(Prec p)
{
    return p == PEq || p == PCmp;
}

/* Tokens that start an `expr_simple`, and so an argument of an application
   or an element of a list */
constexpr std::array<bool, NUM_KINDS> makeSimpleStarts()
{
    std::array<bool, NUM_KINDS> t{};
    for (auto k : {ID, INT_LIT, FLOAT_LIT, QUOTE, IND_STRING_OPEN, PATH, HPATH, SPATH, URI, LPAREN, LET, REC, LBRACE, LBRACKET})
        t[k] = true;
    return t;
}

constexpr auto simpleStarts = makeSimpleStarts();

/* An expression and the offset of its first token, which is where Bison
   put the location of the nonterminal */
struct E
{
    Expr * e;
    uint32_t begin;
};

/* Interning through the shared table is a concurrent hash map operation,
   and most identifiers repeat, so each thread keeps a plain hash map in
   front of it. Names up to `inlineLen` bytes live in the entry itself, so a
   hit touches one cache line */
class SymbolCache
{
    static constexpr size_t inlineLen = 16;

    struct Entry
    {
        uint64_t hash;
        Symbol sym;
        uint32_t len;
        union
        {
            char text[inlineLen];
            const char * ptr;
        };
    };

    static_assert(sizeof(Entry) == 32);

    std::vector<Entry> entries;
    size_t used = 0;
    uint64_t serial = 0;

    static uint64_t load64(const char * p)
    {
        uint64_t w;
        memcpy(&w, p, 8);
        return w;
    }

    /* The first `len` bytes at `p`, zero-extended, for `len` up to 8. Two
       loads that overlap but stay inside the string: copying the bytes to
       the stack and loading them back as a word stalls on store forwarding,
       which was most of the time spent here */
    static uint64_t loadTail(const char * p, size_t len)
    {
        if (len >= 8)
            return load64(p);
        if (len >= 4) {
            uint32_t lo, hi;
            memcpy(&lo, p, 4);
            memcpy(&hi, p + len - 4, 4);
            return lo | (uint64_t(hi) << ((len - 4) * 8));
        }
        if (len == 0)
            return 0;
        auto b = [&](size_t i) { return uint64_t(uint8_t(p[i])) << (i * 8); };
        return b(0) | b(len / 2) | b(len - 1);
    }

    static uint64_t mix(uint64_t h, uint64_t w)
    {
        return (std::rotl(h, 29) ^ w) * 0x9e3779b97f4a7c15ull;
    }

    static uint64_t hashOf(const char * p, size_t len)
    {
        uint64_t h = len * 0x9e3779b97f4a7c15ull;
        while (len >= 8) {
            h = mix(h, load64(p));
            p += 8;
            len -= 8;
        }
        if (len)
            h = mix(h, loadTail(p, len));
        return h ^ (h >> 32);
    }

    void grow()
    {
        std::vector<Entry> old(entries.size() * 2);
        old.swap(entries);
        size_t mask = entries.size() - 1;
        for (auto & e : old)
            if (e.sym) {
                size_t i = e.hash & mask;
                while (entries[i].sym)
                    i = (i + 1) & mask;
                entries[i] = e;
            }
    }

    [[gnu::noinline]] Symbol insert(SymbolTable & symbols, std::string_view s, uint64_t h, size_t i)
    {
        Symbol sym = symbols.create(s);
        auto & e = entries[i];
        e.hash = h;
        e.sym = sym;
        e.len = uint32_t(s.size());
        if (s.size() <= inlineLen) {
            memset(e.text, 0, inlineLen);
            memcpy(e.text, s.data(), s.size());
        } else
            /* The table's copy, which lives as long as the table */
            e.ptr = std::string_view(symbols[sym]).data();
        if (++used * 2 > entries.size())
            grow();
        return sym;
    }

public:
    /**
     * Forget symbols from any other table
     */
    void start(const SymbolTable & symbols)
    {
        if (serial == symbols.serial)
            return;
        entries.assign(4096, Entry{});
        used = 0;
        serial = symbols.serial;
    }

    Symbol get(SymbolTable & symbols, std::string_view s)
    {
        if (s.size() <= inlineLen) {
            /* The name as the zero-padded words the entry keeps */
            size_t n = s.size();
            uint64_t w0 = loadTail(s.data(), std::min<size_t>(n, 8));
            uint64_t w1 = n > 8 ? loadTail(s.data() + 8, n - 8) : 0;
            uint64_t h = n * 0x9e3779b97f4a7c15ull;
            if (n)
                h = mix(h, w0);
            if (n > 8)
                h = mix(h, w1);
            h ^= h >> 32;
            size_t mask = entries.size() - 1;
            size_t i = h & mask;
            for (; entries[i].sym; i = (i + 1) & mask) {
                auto & e = entries[i];
                if (e.hash == h && e.len == n && load64(e.text) == w0 && load64(e.text + 8) == w1)
                    return e.sym;
            }
            return insert(symbols, s, h, i);
        }
        uint64_t h = hashOf(s.data(), s.size());
        size_t mask = entries.size() - 1;
        size_t i = h & mask;
        for (; entries[i].sym; i = (i + 1) & mask) {
            auto & e = entries[i];
            if (e.hash == h && e.len == s.size() && memcmp(e.ptr, s.data(), s.size()) == 0)
                return e.sym;
        }
        return insert(symbols, s, h, i);
    }
};

thread_local SymbolCache symbolCache;

/* Attribute sets of at least twice this many tokens are parsed in pieces
   on several threads */
size_t minPiece()
{
    static const size_t v = [] {
        auto e = getenv("NIX_PARSER_MIN_PIECE");
        return e ? size_t(std::stoul(e)) : size_t(1) << 16;
    }();
    return v;
}

unsigned maxPieces()
{
    static const unsigned v = [] {
        if (auto e = getenv("NIX_PARSER_THREADS"))
            return unsigned(std::stoul(e));
        /* Each pool thread keeps a few MB of caches and allocator heap, and
           past 8 the big files hardly get faster */
        return std::clamp(std::thread::hardware_concurrency() * 3 / 4, 1u, 8u);
    }();
    return v;
}

class Parser
{
    ParserState & st;
    LexerState & ls;
    char * const text;
    const Tokens & tk;
    const Token * t;
    /* This thread's, looked up once: a thread_local with a constructor
       goes through a wrapper on every use */
    SymbolCache & symbols = symbolCache;

    /* The `{` and `}` of the attribute sets big enough to parse in
       pieces, by `{` */
    std::vector<std::pair<uint32_t, uint32_t>> bigSets;
    /* Parsing a piece of a big attribute set, which must not start
       parsing pieces itself */
    bool inPiece = false;
    Symbol sImport;

    PosIdx at(uint32_t offset)
    {
        return st.positions.add(st.origin, offset);
    }

    [[noreturn, gnu::noinline, gnu::cold]] void lexError(const Token & tok)
    {
        auto & err = tk.errors[tok.aux];
        auto pos = st.positions[at(err.offset)];
        switch (+err.kind) {
        case LexError::InvalidInteger:
            throw ParseError(ErrorInfo{.msg = HintFmt("invalid integer '%1%'", err.text), .pos = pos});
        case LexError::InvalidFloat:
            throw ParseError(ErrorInfo{.msg = HintFmt("invalid float '%1%'", err.text), .pos = pos});
        case LexError::PipeOperators:
            throw ParseError(
                ErrorInfo{
                    .msg = HintFmt(
                        "experimental Nix feature '%1%' is disabled; add '--extra-experimental-features %1%' to enable it",
                        showExperimentalFeature(Xp::PipeOperators)),
                    .pos = pos,
                });
        case LexError::NullByte:
            forceNoNullByte(err.text, [&]() { return pos; });
            unreachable();
        case LexError::TrailingSlash:
            throw ParseError(ErrorInfo{.msg = HintFmt("path has a trailing slash"), .pos = pos});
        }
        unreachable();
    }

    [[gnu::always_inline]] const Token & peek()
    {
        if (t->kind == LEX_ERROR) [[unlikely]]
            lexError(*t);
        return *t;
    }

    [[gnu::always_inline]] Kind kind()
    {
        return peek().kind;
    }

    Kind kindAt(size_t k)
    {
        auto & tok = t[k];
        for (size_t i = 0; i <= k; i++)
            if (t[i].kind == LEX_ERROR || t[i].kind == EOF_) {
                if (t[i].kind == LEX_ERROR)
                    lexError(t[i]);
                return EOF_;
            }
        return tok.kind;
    }

    [[gnu::always_inline]] const Token & next()
    {
        auto & tok = peek();
        t++;
        return tok;
    }

    [[noreturn, gnu::noinline, gnu::cold]] void syntaxError(const Token & tok, std::initializer_list<Kind> expected = {})
    {
        std::string msg = "syntax error, unexpected ";
        msg += kindName(tok.kind);
        if (expected.size() && expected.size() <= 4) {
            std::vector<Kind> ks(expected);
            std::sort(ks.begin(), ks.end());
            const char * sep = ", expecting ";
            for (auto k : ks) {
                msg += sep;
                msg += kindName(k);
                sep = " or ";
            }
        }
        bool eof = tok.kind == EOF_;
        throw ParseError({.msg = HintFmt(msg), .pos = st.positions[at(eof ? tok.end : tok.begin)]}).setIncomplete(eof);
    }

    [[gnu::always_inline]] const Token & expect(Kind k, std::initializer_list<Kind> expected)
    {
        auto & tok = peek();
        if (tok.kind != k)
            syntaxError(tok, expected);
        t++;
        return tok;
    }

    std::string_view raw(const Token & tok)
    {
        return {text + tok.begin, tok.end - tok.begin};
    }

    /* The value of a `STR` token */
    std::string_view strValue(const Token & tok)
    {
        if (tok.flavour == Unescaped)
            return {text + tok.begin, tok.aux};
        return raw(tok);
    }

    StringToken indStrValue(const Token & tok)
    {
        switch (+tok.flavour) {
        case Indented:
            return {text + tok.begin, tok.end - tok.begin, true};
        case Unescaped:
            return {text + tok.begin + 2, tok.aux, false};
        case Literal:
            switch (+tok.aux) {
            case LitDollar:
                return {"$", 1, false};
            case LitQuotes:
                return {"''", 2, false};
            default:
                return {"'", 1, false};
            }
        default:
            return {text + tok.begin, tok.end - tok.begin, false};
        }
    }

    Symbol sym(const Token & tok)
    {
        return symbols.get(st.symbols, raw(tok));
    }

    void setDocPosition(ExprLambda * lambda, uint32_t begin)
    {
        auto it = ls.positionToDocComment.find(at(begin));
        if (it != ls.positionToDocComment.end())
            lambda->setDocComment(it->second);
    }

    /* Whether this parse made a call with `or` as its argument. Until it
       does, nothing can carry a cursed `or`, and the virtual calls that look
       for one are skipped */
    bool anyCursedOr = false;

    void warnIfCursedOr(Expr * e)
    {
        if (!anyCursedOr)
            return;
        if (!st.quiet)
            e->warnIfCursedOr(st.symbols, st.positions);
        else if (auto call = dynamic_cast<ExprCall *>(e); call && call->cursedOrEndPos)
            throw ParseAbandoned();
    }

    template<typename F>
    void lint(const Setting<Diagnose> & setting, const F & mkError)
    {
        if (st.quiet && setting.get() == Diagnose::Warn) {
            if (mkError(false))
                throw ParseAbandoned();
            return;
        }
        diagnose(setting, mkError);
    }

    Expr * makeCall(PosIdx pos, Expr * fn, Expr * arg)
    {
        if (auto e2 = dynamic_cast<ExprCall *>(fn)) {
            e2->args->push_back(arg);
            return fn;
        }
        return st.exprs.add<ExprCall>(pos, fn, {arg});
    }

    bool isFormals();
    Expr * parsePathStart(const Token & tok);

    void addStringy(std::vector<AttrName> & path, ToBeStringyExpr && x)
    {
        x.visit(
            overloaded{
                [&](std::string_view str) { path.emplace_back(st.symbols.create(str)); },
                [&](Expr * expr) { path.emplace_back(expr); }});
    }

    enum BindsContext {
        /* `{ ... }`, which needs one binding at least */
        Braces,
        /* `let ... in` */
        LetIn,
        /* `rec { ... }` and `let { ... }` */
        RecBraces,
        /* REPL bindings, which need one at least */
        Repl,
    };

    struct Inherit
    {
        Expr * from = nullptr;
        uint32_t fromBegin = 0;
        std::vector<std::pair<AttrName, PosIdx>> names;
    };

    /* A binding of a piece, to be added to the attribute set once the
       piece is known to be right */
    struct Binding
    {
        std::vector<AttrName> path;
        uint32_t begin = 0;
        E e{};
        std::optional<Inherit> inherit;
    };

    /* Where the machine goes on once the rule it went into is done */
    enum Cont : uint8_t {
        KExit,
        KLambda,
        KLambdaAtFormals,
        KLambdaAtBody,
        KSetLambdaFormals,
        KSetLambdaBody,
        KSetLambdaAtBody,
        KAssertCond,
        KAssertBody,
        KWithAttrs,
        KWithBody,
        KLetBinds,
        KLetBody,
        KIfCond,
        KIfThen,
        KIfElse,
        KIfOp,
        KPipeFrom,
        KPipeInto,
        KNot,
        KNegate,
        KOpLhs,
        KOpHasAttr,
        KOpRhs,
        KAppFirst,
        KAppArg,
        KSelSimple,
        KSelPath,
        KSelDefault,
        KPathQuote,
        KPathDollar,
        KStrParts,
        KInterpDollar,
        KSimpleQuote,
        KIndDollar,
        KPathInterp,
        KParen,
        KLetSet,
        KRecSet,
        KSet,
        KList,
        KBindPath,
        KBindValue,
        KInheritFrom,
        KInheritQuote,
        KInheritDollar,
        KFormalDefault,
    };

    struct Frame
    {
        Cont k;
        Prec lastNonAssoc = PNone;
        bool path = false;
        int minPrec = 0;
        const Token * tok = nullptr;
        const Token * tok2 = nullptr;
        E a{}, b{};
    };

    /* A stack whose popped elements keep their buffers for the next push */
    template<typename T>
    struct Stack
    {
        std::vector<T> items;
        size_t n = 0;

        T & push()
        {
            if (n == items.size())
                items.emplace_back();
            return items[n++];
        }

        T & top()
        {
            return items[n - 1];
        }

        void pop()
        {
            n--;
        }
    };

    struct Binds
    {
        ExprAttrs * attrs = nullptr;
        bool any = false;
        const Token * limit = nullptr;
        std::vector<Binding> * record = nullptr;
        BindsContext ctx = Braces;
        /* Whether the bindings went on past `limit` */
        bool more = false;
        /* Started by `runBindsTo`, which gets them back */
        bool entry = false;
    };

    std::vector<Frame> frames;
    Stack<std::vector<AttrName>> paths;
    Stack<std::vector<std::pair<PosIdx, Expr *>>> parts;
    Stack<std::vector<std::pair<PosIdx, std::variant<Expr *, StringToken>>>> indParts;
    Stack<std::pmr::vector<Expr *>> lists;
    Stack<std::vector<std::pair<uint32_t, Expr *>>> pipes;
    Stack<FormalsBuilder> formals;
    Stack<Inherit> inherits;
    Stack<Binds> bindsStack;

    enum Entry { EntryExpr, EntryBinds, EntryBindsTo };

    E run(Entry entry, BindsContext bindsCtx = Braces);
    /* Parse bindings into `attrs`, or into `record` when it is set, until
       `t` reaches `limit`; false at the first token that starts no
       binding */
    bool runBindsTo(ExprAttrs *& attrs, bool & any, const Token * limit, std::vector<Binding> * record);
    bool parseBindsInPieces(ExprAttrs *& attrs, bool & any);
    void addInherit(ExprAttrs * attrs, Inherit && inherit);
    void findBigSets();

public:
    Parser(ParserState & st, LexerState & ls, char * text, const Tokens & tk)
        : st(st)
        , ls(ls)
        , text(text)
        , tk(tk)
        , t(tk.tokens.data())
    {
        symbols.start(st.symbols);
        if (st.imports)
            sImport = symbols.get(st.symbols, "import");
    }

    Expr * parseStart(bool repl);
};

Expr * Parser::parseStart(bool repl)
{
    if (tk.tokens.size() >= 2 * minPiece() && maxPieces() > 1)
        findBigSets();
    frames.reserve(64);
    if (repl && kind() == REPL_BINDINGS) {
        next();
        auto attrs = run(EntryBinds, Repl);
        expect(EOF_, {EOF_});
        return attrs.e;
    }
    auto e = run(EntryExpr);
    expect(EOF_, {EOF_});
    return e.e;
}

bool Parser::isFormals()
{
    /* Decided the way the LALR(1) automaton decided, by the tokens after
       the brace */
    switch (+kindAt(1)) {
    case ELLIPSIS:
        return true;
    case RBRACE: {
        auto k = kindAt(2);
        return k == COLON || k == AT;
    }
    case ID: {
        auto k = kindAt(2);
        return k == COMMA || k == QUESTION || k == RBRACE;
    }
    default:
        return false;
    }
}

Expr * Parser::parsePathStart(const Token & tok)
{
    std::string_view literal = raw(tok);
    auto pos = at(tok.begin);

    if (tok.kind == HPATH) {
        if (st.settings.pureEval) {
            throw Error("the path '%s' can not be resolved in pure mode", literal);
        }
        lint(st.settings.lintAbsolutePathLiterals, [&](bool) -> std::optional<ParseError> {
            return ParseError(
                {.msg = HintFmt(
                     "home path literals are not portable. Consider replacing path literal '%s' by a string, relative path, or parameter",
                     literal),
                 .pos = st.positions[pos]});
        });
        auto path = CanonPath(concatStrings(getHome().string(), literal.substr(1))).abs();
        if (literal.size() > 1 && literal.back() == '/')
            path += '/';
        return st.exprs.add<ExprPath>(st.exprs.alloc, st.rootFS, path);
    }

    if (literal.front() == '/') {
        lint(st.settings.lintAbsolutePathLiterals, [&](bool) -> std::optional<ParseError> {
            return ParseError(
                {.msg = HintFmt(
                     "absolute path literals are not portable. Consider replacing path literal '%s' by a string, relative path, or parameter",
                     literal),
                 .pos = st.positions[pos]});
        });

        /* Absolute paths are always interpreted relative to the root
           filesystem accessor, rather than the accessor of the current Nix
           expression */
        auto path = CanonPath(literal).abs();
        /* add back in the trailing '/' to the first segment */
        if (literal.size() > 1 && literal.back() == '/')
            path += '/';
        return st.exprs.add<ExprPath>(st.exprs.alloc, st.rootFS, path);
    }

    /* check for short path literals */
    lint(st.settings.lintShortPathLiterals, [&](bool) -> std::optional<ParseError> {
        if (literal.front() != '.')
            return ParseError(
                {.msg = HintFmt(
                     "relative path literal '%s' should be prefixed with '.' for clarity: './%s'", literal, literal),
                 .pos = st.positions[pos]});
        return std::nullopt;
    });

    auto path = CanonPath(literal, st.basePath.path).abs();
    /* add back in the trailing '/' to the first segment */
    if (literal.size() > 1 && literal.back() == '/')
        path += '/';
    return st.exprs.add<ExprPath>(st.exprs.alloc, st.basePath.accessor, path);
}

/* The grammar as one state machine. Where the grammar nests, the machine
   pushes a frame that says where to go on, and jumps to the start of the
   inner rule; an inner rule ends at `ret`, with its expression in `r`,
   and `ret` jumps to where the top frame says. So nesting costs a frame
   on a vector instead of C++ stack, and nesting is only limited by
   memory, as with Bison. Only the first piece of a big attribute set
   runs a machine inside another one, so that nests as deep as big sets
   do */
E Parser::run(Entry entry, BindsContext bindsCtx)
{
    auto & ex = st.exprs;
    const size_t base = frames.size();
    E r{};
    /* What the string rules give back */
    ToBeStringyExpr rs;
    /* Arguments of the rules that take some */
    int minPrec = 0;
    bool interpPath = false;

    auto push = [&](Cont k, const Token * tok = nullptr) -> Frame & {
        frames.push_back(Frame{.k = k, .tok = tok});
        return frames.back();
    };
    auto top = [&]() -> Frame & { return frames.back(); };

    push(KExit);
    switch (+entry) {
    case EntryExpr:
        goto expr;
    case EntryBinds:
        goto binds;
    case EntryBindsTo:
        goto bindsLoop;
    }

ret: {
    auto & f = top();
    switch (+f.k) {
    case KExit:
        frames.pop_back();
        assert(frames.size() == base);
        return r;
    case KLambda: {
        auto & tok = *f.tok;
        auto me = ex.add<ExprLambda>(at(tok.begin), sym(tok), r.e);
        setDocPosition(me, tok.begin);
        r = {me, tok.begin};
        frames.pop_back();
        goto ret;
    }
    case KLambdaAtFormals:
        expect(COLON, {COLON});
        f.k = KLambdaAtBody;
        goto function;
    case KLambdaAtBody: {
        auto & tok = *f.tok;
        auto & fs = formals.top();
        auto arg = sym(tok);
        st.validateFormals(fs, at(tok.begin), arg);
        auto me = ex.add<ExprLambda>(st.positions, ex.alloc, at(tok.begin), arg, fs, r.e);
        setDocPosition(me, tok.begin);
        formals.pop();
        r = {me, tok.begin};
        frames.pop_back();
        goto ret;
    }
    case KSetLambdaFormals: {
        auto & k = peek();
        if (k.kind == COLON) {
            next();
            f.k = KSetLambdaBody;
            goto function;
        }
        if (k.kind == AT) {
            next();
            f.tok2 = &expect(ID, {ID});
            expect(COLON, {COLON});
            f.k = KSetLambdaAtBody;
            goto function;
        }
        syntaxError(k, {COLON, AT});
    }
    case KSetLambdaBody: {
        auto & tok = *f.tok;
        auto & fs = formals.top();
        st.validateFormals(fs);
        auto me = ex.add<ExprLambda>(st.positions, ex.alloc, at(tok.begin), fs, r.e);
        setDocPosition(me, tok.begin);
        formals.pop();
        r = {me, tok.begin};
        frames.pop_back();
        goto ret;
    }
    case KSetLambdaAtBody: {
        auto & tok = *f.tok;
        auto & fs = formals.top();
        auto arg = sym(*f.tok2);
        st.validateFormals(fs, at(tok.begin), arg);
        auto me = ex.add<ExprLambda>(st.positions, ex.alloc, at(tok.begin), arg, fs, r.e);
        setDocPosition(me, tok.begin);
        formals.pop();
        r = {me, tok.begin};
        frames.pop_back();
        goto ret;
    }
    case KAssertCond:
    case KWithAttrs:
        expect(SEMICOLON, {SEMICOLON});
        f.a = r;
        f.k = f.k == KAssertCond ? KAssertBody : KWithBody;
        goto function;
    case KAssertBody:
        r = {ex.add<ExprAssert>(at(f.tok->begin), f.a.e, r.e), f.tok->begin};
        frames.pop_back();
        goto ret;
    case KWithBody:
        r = {ex.add<ExprWith>(at(f.tok->begin), f.a.e, r.e), f.tok->begin};
        frames.pop_back();
        goto ret;
    case KLetBinds:
        expect(IN_KW, {IN_KW, INHERIT});
        f.a = r;
        f.k = KLetBody;
        goto function;
    case KLetBody: {
        auto binds = static_cast<ExprAttrs *>(f.a.e);
        if (!binds->dynamicAttrs->empty())
            throw ParseError(
                {.msg = HintFmt("dynamic attributes not allowed in let"), .pos = st.positions[at(f.tok->begin)]});
        r = {ex.add<ExprLet>(binds, r.e), f.tok->begin};
        frames.pop_back();
        goto ret;
    }
    case KIfCond:
        expect(THEN, {THEN});
        f.a = r;
        f.k = KIfThen;
        goto expr;
    case KIfThen:
        expect(ELSE, {ELSE});
        f.b = r;
        f.k = KIfElse;
        goto expr;
    case KIfElse:
        r = {ex.add<ExprIf>(at(f.tok->begin), f.a.e, f.b.e, r.e), f.tok->begin};
        frames.pop_back();
        goto ret;
    case KIfOp:
        goto ifOpRet;
    case KPipeFrom: {
        auto & chain = pipes.top();
        chain.emplace_back(f.tok->begin, r.e);
        if (kind() == PIPE_FROM) {
            f.tok = &next();
            minPrec = PImpl;
            goto op;
        }
        Expr * acc = chain.back().second;
        for (size_t i = chain.size() - 1; i > 0; i--)
            acc = makeCall(at(chain[i].first), chain[i - 1].second, acc);
        pipes.pop();
        r = {acc, f.a.begin};
        frames.pop_back();
        goto ret;
    }
    case KPipeInto:
        f.a.e = makeCall(at(f.tok->begin), r.e, f.a.e);
        if (kind() == PIPE_INTO) {
            f.tok = &next();
            minPrec = PImpl;
            goto op;
        }
        r = f.a;
        frames.pop_back();
        goto ret;
    case KNot:
        r = {ex.add<ExprOpNot>(r.e), f.tok->begin};
        frames.pop_back();
        goto ret;
    case KNegate:
        r = {
            ex.add<ExprCall>(at(f.tok->begin), ex.add<ExprVar>(st.s.sub), {ex.add<ExprInt>(0), r.e}), f.tok->begin};
        frames.pop_back();
        goto ret;
    case KOpLhs:
        goto opLhsRet;
    case KOpHasAttr:
        f.a.e = ex.add<ExprOpHasAttr>(ex.alloc, f.a.e, paths.top());
        paths.pop();
        f.lastNonAssoc = PNone;
        goto opLoop;
    case KOpRhs:
        goto opRhsRet;
    case KAppFirst:
        goto appFirstRet;
    case KAppArg:
        goto appArgRet;
    case KSelSimple:
        goto selSimpleRet;
    case KSelPath:
        if (kind() == OR_KW) {
            next();
            f.k = KSelDefault;
            goto select;
        }
        r = {ex.add<ExprSelect>(ex.alloc, at(f.a.begin), f.a.e, paths.top(), nullptr), f.a.begin};
        paths.pop();
        frames.pop_back();
        goto selectRet;
    case KSelDefault: {
        auto e = ex.add<ExprSelect>(ex.alloc, at(f.a.begin), f.a.e, paths.top(), r.e);
        warnIfCursedOr(r.e);
        paths.pop();
        r = {e, f.a.begin};
        frames.pop_back();
        goto selectRet;
    }
    case KPathQuote:
        expect(QUOTE, {QUOTE});
        addStringy(paths.top(), std::move(rs));
        frames.pop_back();
        goto attrPathNext;
    case KPathDollar:
        expect(RBRACE, {RBRACE});
        addStringy(paths.top(), ToBeStringyExpr(r.e));
        frames.pop_back();
        goto attrPathNext;
    case KStrParts:
        rs = ToBeStringyExpr(ex.add<ExprConcatStrings>(ex.alloc, at(f.tok->begin), true, parts.top()));
        parts.pop();
        frames.pop_back();
        goto ret;
    case KInterpDollar:
        expect(RBRACE, {RBRACE});
        parts.top().emplace_back(at(f.tok->begin), r.e);
        interpPath = f.path;
        frames.pop_back();
        goto interp;
    case KSimpleQuote:
        expect(QUOTE, {QUOTE});
        r = {rs.toExpr(ex), f.tok->begin};
        frames.pop_back();
        goto selSimpleRet;
    case KIndDollar:
        expect(RBRACE, {RBRACE});
        indParts.top().emplace_back(at(f.tok2->begin), r.e);
        goto indLoop;
    case KPathInterp: {
        expect(PATH_END, {STR, PATH_END, DOLLAR_CURLY});
        auto & ps = parts.top();
        ps.insert(ps.begin(), {at(f.tok->begin), f.a.e});
        r = {ex.add<ExprConcatStrings>(ex.alloc, at(f.tok->begin), false, ps), f.tok->begin};
        parts.pop();
        frames.pop_back();
        goto selSimpleRet;
    }
    case KParen:
        expect(RPAREN, {RPAREN});
        r = {r.e, f.tok->begin};
        frames.pop_back();
        goto selSimpleRet;
    case KLetSet:
    case KRecSet: {
        expect(RBRACE, {INHERIT, RBRACE});
        auto binds = static_cast<ExprAttrs *>(r.e);
        binds->recursive = true;
        binds->pos = at(f.tok->begin);
        if (f.k == KLetSet)
            r = {ex.add<ExprSelect>(ex.alloc, at(f.tok->begin), binds, st.s.body), f.tok->begin};
        else
            r = {binds, f.tok->begin};
        frames.pop_back();
        goto selSimpleRet;
    }
    case KSet: {
        expect(RBRACE, {INHERIT});
        auto binds = static_cast<ExprAttrs *>(r.e);
        binds->pos = at(f.tok->begin);
        r = {binds, f.tok->begin};
        frames.pop_back();
        goto selSimpleRet;
    }
    case KList:
        lists.top().push_back(r.e);
        warnIfCursedOr(r.e);
        /* As in `imports = [ ./foo.nix ]` */
        if (st.imports)
            if (auto path = dynamic_cast<ExprPath *>(r.e); path && path->v.pathStrView().ends_with(".nix"))
                st.imports->push_back(path->v.path());
        goto listLoop;
    case KBindPath:
        expect(ASSIGN, {DOT, ASSIGN});
        f.k = KBindValue;
        goto expr;
    case KBindValue: {
        auto & bs = bindsStack.top();
        expect(SEMICOLON, {SEMICOLON});
        if (bs.record)
            bs.record->push_back(Binding{.path = std::move(paths.top()), .begin = f.tok->begin, .e = r});
        else {
            if (!bs.attrs)
                bs.attrs = ex.add<ExprAttrs>(ex.alloc);
            st.addAttr(
                bs.attrs,
                std::move(paths.top()),
                ParserLocation{int(f.tok->begin), 0, 0, 0},
                r.e,
                ParserLocation{int(r.begin), 0, 0, 0});
        }
        paths.pop();
        bs.any = true;
        frames.pop_back();
        goto bindsLoop;
    }
    case KInheritFrom: {
        expect(RPAREN, {RPAREN});
        auto & inh = inherits.top();
        inh.from = r.e;
        inh.fromBegin = r.begin;
        frames.pop_back();
        goto inheritNames;
    }
    case KInheritQuote:
    case KInheritDollar: {
        auto & tok = *f.tok;
        ToBeStringyExpr x;
        if (f.k == KInheritQuote) {
            expect(QUOTE, {QUOTE});
            x = std::move(rs);
        } else {
            expect(RBRACE, {RBRACE});
            x = ToBeStringyExpr(r.e);
        }
        auto & names = inherits.top().names;
        x.visit(
            overloaded{
                [&](std::string_view str) { names.emplace_back(st.symbols.create(str), at(tok.begin)); },
                [&](Expr * expr) {
                    throw ParseError(
                        {.msg = HintFmt("dynamic attributes not allowed in inherit"),
                         .pos = st.positions[at(tok.begin)]});
                }});
        frames.pop_back();
        goto inheritNames;
    }
    case KFormalDefault:
        formals.top().formals.emplace_back(Formal{at(f.tok->begin), sym(*f.tok), r.e});
        goto formalNext;
    }
    unreachable();
}

    /* The continuations every atom returns through, out of the switch so
       that a rule whose caller is known, or one of two or three, jumps
       there directly instead of through the switch's indirect jump, which
       mispredicts most of the time */
ifOpRet: {
    auto & f = top();
    if (kind() == PIPE_FROM) {
        /* Right associative */
        auto & chain = pipes.push();
        chain.clear();
        chain.emplace_back(0, r.e);
        f.a = r;
        f.k = KPipeFrom;
        f.tok = &next();
        minPrec = PImpl;
        goto op;
    }
    if (kind() == PIPE_INTO) {
        f.a = r;
        f.k = KPipeInto;
        f.tok = &next();
        minPrec = PImpl;
        goto op;
    }
    frames.pop_back();
    goto ret;
}

opLhsRet: {
    auto & f = top();
    f.a = r;
    f.lastNonAssoc = PNone;
    goto opLoop;
}

opRhsRet: {
    auto & f = top();
    auto & op = *f.tok;
    auto & lhs = f.a;
    auto & rhs = r;
    Prec p = precs[op.kind];
    auto pos = at(op.begin);
    Expr * e;
    switch (+op.kind) {
    case EQ:
        e = ex.add<ExprOpEq>(pos, lhs.e, rhs.e);
        break;
    case NEQ:
        e = ex.add<ExprOpNEq>(pos, lhs.e, rhs.e);
        break;
    case LT:
        e = ex.add<ExprCall>(pos, ex.add<ExprVar>(st.s.lessThan), {lhs.e, rhs.e});
        break;
    case LEQ:
        e = ex.add<ExprOpNot>(ex.add<ExprCall>(pos, ex.add<ExprVar>(st.s.lessThan), {rhs.e, lhs.e}));
        break;
    case GT:
        e = ex.add<ExprCall>(pos, ex.add<ExprVar>(st.s.lessThan), {rhs.e, lhs.e});
        break;
    case GEQ:
        e = ex.add<ExprOpNot>(ex.add<ExprCall>(pos, ex.add<ExprVar>(st.s.lessThan), {lhs.e, rhs.e}));
        break;
    case AND:
        e = ex.add<ExprOpAnd>(pos, lhs.e, rhs.e);
        break;
    case OR:
        e = ex.add<ExprOpOr>(pos, lhs.e, rhs.e);
        break;
    case IMPL:
        e = ex.add<ExprOpImpl>(pos, lhs.e, rhs.e);
        break;
    case UPDATE:
        e = ex.add<ExprOpUpdate>(pos, lhs.e, rhs.e);
        break;
    case PLUS:
        e = ex.add<ExprConcatStrings>(ex.alloc, pos, false, {{at(lhs.begin), lhs.e}, {at(rhs.begin), rhs.e}});
        break;
    case MINUS:
        e = ex.add<ExprCall>(pos, ex.add<ExprVar>(st.s.sub), {lhs.e, rhs.e});
        break;
    case STAR:
        e = ex.add<ExprCall>(pos, ex.add<ExprVar>(st.s.mul), {lhs.e, rhs.e});
        break;
    case SLASH:
        e = ex.add<ExprCall>(pos, ex.add<ExprVar>(st.s.div), {lhs.e, rhs.e});
        break;
    case CONCAT:
        e = ex.add<ExprOpConcatLists>(pos, lhs.e, rhs.e);
        break;
    default:
        unreachable();
    }
    lhs.e = e;
    f.lastNonAssoc = nonAssoc(p) ? p : PNone;
    goto opLoop;
}

appFirstRet: {
    auto & f = top();
    if (anyCursedOr)
        r.e->resetCursedOr();
    f.a = r;
    f.b = r;
    goto appLoop;
}

appArgRet: {
    auto & f = top();
    if (st.imports && f.b.e == f.a.e)
        if (auto var = dynamic_cast<ExprVar *>(f.b.e); var && var->name == sImport)
            if (auto path = dynamic_cast<ExprPath *>(r.e))
                st.imports->push_back(path->v.path());
    /* Past the first argument `f.b` is the call this built */
    if (f.b.e != f.a.e)
        static_cast<ExprCall *>(f.b.e)->args->push_back(r.e);
    else
        f.b.e = makeCall(at(f.a.begin), f.b.e, r.e);
    warnIfCursedOr(r.e);
    goto appLoop;
}

selSimpleRet: {
    auto & f = top();
    auto & tok = peek();
    if (tok.kind == DOT) {
        next();
        f.a = r;
        f.k = KSelPath;
        goto attrPath;
    }
    if (tok.kind == OR_KW) {
        next();
        r = {
            ex.add<ExprCall>(
                at(r.begin), r.e, {ex.add<ExprVar>(at(r.begin), st.s.or_)}, st.positions.add(st.origin, tok.end)),
            r.begin};
        anyCursedOr = true;
    }
    frames.pop_back();
    goto selectRet;
}

    /* What called `select`: the head or an argument of an application, a
       list element, or the default of an `or` */
selectRet:
    if (top().k == KAppArg)
        goto appArgRet;
    if (top().k == KAppFirst)
        goto appFirstRet;
    goto ret;

    /* What called `op`: mostly `exprIf`, or an operator's left side */
opRet:
    if (top().k == KIfOp)
        goto ifOpRet;
    if (top().k == KOpRhs)
        goto opRhsRet;
    goto ret;

    /* expr: function

       function: ID ':' function | ID '@' formals ':' function
       | formals ':' function | formals '@' ID ':' function
       | ASSERT expr ';' function | WITH expr ';' function
       | LET binds IN function | expr_if */
expr:
function: {
    auto & tok = peek();
    switch (+tok.kind) {
    case ID: {
        auto k = kindAt(1);
        if (k == COLON) {
            next();
            next();
            push(KLambda, &tok);
            goto function;
        }
        if (k == AT) {
            next();
            next();
            push(KLambdaAtFormals, &tok);
            goto formalSet;
        }
        break;
    }
    case LBRACE:
        if (isFormals()) {
            push(KSetLambdaFormals, &tok);
            goto formalSet;
        }
        break;
    case ASSERT:
        next();
        push(KAssertCond, &tok);
        goto expr;
    case WITH:
        next();
        push(KWithAttrs, &tok);
        goto expr;
    case LET:
        if (kindAt(1) == LBRACE)
            break;
        next();
        push(KLetBinds, &tok);
        bindsCtx = LetIn;
        goto binds;
    default:
        break;
    }
    goto exprIf;
}

    /* expr_if: IF expr THEN expr ELSE expr | expr_pipe_from | expr_pipe_into
       | expr_op */
exprIf: {
    auto & tok = peek();
    if (tok.kind == IF) {
        next();
        push(KIfCond, &tok);
        goto expr;
    }
    push(KIfOp);
    minPrec = PImpl;
    goto op;
}

    /* Operators by precedence climbing: a unary expression, then every
       operator that binds at least `minPrec` with its right operand */
op:
    push(KOpLhs).minPrec = minPrec;
    goto unary;

opLoop: {
    auto & f = top();
    auto & opTok = peek();
    Prec p = precs[opTok.kind];
    if (p == PNone || p < f.minPrec) {
        r = f.a;
        frames.pop_back();
        goto opRet;
    }
    if (p == f.lastNonAssoc)
        syntaxError(opTok);
    next();
    if (opTok.kind == QUESTION) {
        f.k = KOpHasAttr;
        goto attrPath;
    }
    f.k = KOpRhs;
    f.tok = &opTok;
    minPrec = rightAssoc(opTok.kind) ? p : p + 1;
    goto op;
}

unary: {
    auto & tok = peek();
    if (tok.kind == BANG) {
        next();
        push(KNot, &tok);
        minPrec = PNot + 1;
        goto op;
    }
    if (tok.kind == MINUS) {
        next();
        push(KNegate, &tok);
        minPrec = PNegate + 1;
        goto op;
    }
    push(KAppFirst);
    goto select;
}

    /* f.a is the function, f.b the call built so far */
appLoop:
    if (simpleStarts[kind()]) {
        top().k = KAppArg;
        goto select;
    }
    r = {top().b.e, top().a.begin};
    frames.pop_back();
    /* `unary` is only entered from `op` */
    assert(top().k == KOpLhs);
    goto opLhsRet;

    /* expr_select: expr_simple '.' attrpath ['or' expr_select]
       | expr_simple OR_KW | expr_simple */
select:
    push(KSelSimple);
    goto simple;

    /* attrpath, onto `paths` */
attrPath:
    paths.push().clear();
attrPathLoop: {
    auto & tok = peek();
    auto & path = paths.top();
    switch (+tok.kind) {
    case ID:
        next();
        path.emplace_back(sym(tok));
        break;
    case OR_KW:
        next();
        path.emplace_back(st.symbols.create("or"));
        break;
    case QUOTE:
        next();
        push(KPathQuote);
        goto stringParts;
    case DOLLAR_CURLY:
        next();
        push(KPathDollar);
        goto expr;
    default:
        syntaxError(tok, {ID, OR_KW, DOLLAR_CURLY, QUOTE});
    }
}
attrPathNext:
    if (kind() != DOT)
        goto ret;
    next();
    goto attrPathLoop;

    /* The parts of a string, into `rs` */
stringParts: {
    auto & tok = peek();
    if (tok.kind == STR && kindAt(1) != DOLLAR_CURLY) {
        next();
        rs = ToBeStringyExpr(strValue(tok));
        goto ret;
    }
    if (tok.kind != STR && tok.kind != DOLLAR_CURLY) {
        rs = ToBeStringyExpr(std::string_view());
        goto ret;
    }
    push(KStrParts, &tok);
    parts.push().clear();
    interpPath = false;
    goto interp;
}

    /* Literal parts and interpolations, onto `parts.top()` */
interp: {
    auto & tok = peek();
    if (tok.kind == STR) {
        next();
        parts.top().emplace_back(
            at(tok.begin), ex.add<ExprString>(ex.alloc, interpPath ? raw(tok) : strValue(tok)));
        goto interp;
    }
    if (tok.kind == DOLLAR_CURLY) {
        next();
        push(KInterpDollar, &tok).path = interpPath;
        goto expr;
    }
    goto ret;
}

simple: {
    auto & tok = peek();
    switch (+tok.kind) {
    case ID: {
        next();
        auto s = raw(tok);
        if (s == "__curPos")
            r = {ex.add<ExprPos>(at(tok.begin)), tok.begin};
        else
            r = {ex.add<ExprVar>(at(tok.begin), sym(tok)), tok.begin};
        goto selSimpleRet;
    }
    case INT_LIT:
        next();
        r = {ex.add<ExprInt>(NixInt(int64_t(tk.numbers[tok.aux]))), tok.begin};
        goto selSimpleRet;
    case FLOAT_LIT:
        next();
        r = {ex.add<ExprFloat>(std::bit_cast<double>(tk.numbers[tok.aux])), tok.begin};
        goto selSimpleRet;
    case QUOTE:
        next();
        push(KSimpleQuote, &tok);
        goto stringParts;
    case IND_STRING_OPEN:
        next();
        push(KIndDollar, &tok);
        indParts.push().clear();
        goto indLoop;
    case PATH:
    case HPATH: {
        next();
        auto start = parsePathStart(tok);
        if (kind() == PATH_END) {
            next();
            r = {start, tok.begin};
            goto selSimpleRet;
        }
        /* The grammar only takes a literal segment before an interpolation */
        if (kind() == STR && kindAt(1) != DOLLAR_CURLY)
            syntaxError(t[1], {DOLLAR_CURLY});
        push(KPathInterp, &tok).a.e = start;
        parts.push().clear();
        interpPath = true;
        goto interp;
    }
    case SPATH: {
        next();
        auto s = raw(tok);
        std::string_view path(s.data() + 1, s.size() - 2);
        r = {
            ex.add<ExprCall>(
                at(tok.begin),
                ex.add<ExprVar>(st.s.findFile),
                {ex.add<ExprVar>(st.s.nixPath), ex.add<ExprString>(ex.alloc, path)}),
            tok.begin};
        goto selSimpleRet;
    }
    case URI: {
        next();
        auto s = raw(tok);
        lint(st.settings.lintUrlLiterals, [&](bool fatal) -> std::optional<ParseError> {
            return ParseError(
                {.msg = HintFmt(
                     "URL literals are %s. Consider using a string literal \"%s\" instead",
                     fatal ? "disallowed" : "discouraged",
                     s),
                 .pos = st.positions[at(tok.begin)]});
        });
        r = {ex.add<ExprString>(ex.alloc, s), tok.begin};
        goto selSimpleRet;
    }
    case LPAREN:
        next();
        push(KParen, &tok);
        goto expr;
    case LET:
    case REC:
        next();
        expect(LBRACE, {LBRACE});
        push(tok.kind == LET ? KLetSet : KRecSet, &tok);
        bindsCtx = RecBraces;
        goto binds;
    case LBRACE:
        next();
        if (kind() == RBRACE) {
            next();
            r = {ex.add<ExprAttrs>(ex.alloc, at(tok.begin)), tok.begin};
            goto selSimpleRet;
        }
        push(KSet, &tok);
        bindsCtx = Braces;
        goto binds;
    case LBRACKET:
        next();
        push(KList, &tok);
        lists.push().clear();
        goto listLoop;
    default:
        syntaxError(tok);
    }
}

    /* The frame is KIndDollar, with the opening token in `tok` */
indLoop: {
    auto & part = peek();
    if (part.kind == IND_STR) {
        next();
        indParts.top().emplace_back(at(part.begin), indStrValue(part));
        goto indLoop;
    }
    if (part.kind == DOLLAR_CURLY) {
        next();
        top().tok2 = &part;
        goto expr;
    }
    expect(IND_STRING_CLOSE, {IND_STR, DOLLAR_CURLY, IND_STRING_CLOSE});
    r = {st.stripIndentation(at(top().tok->begin), indParts.top()), top().tok->begin};
    indParts.pop();
    frames.pop_back();
    goto selSimpleRet;
}

    /* The frame is KList */
listLoop:
    if (simpleStarts[kind()])
        goto select;
    expect(RBRACKET, {});
    r = {ex.add<ExprList>(ex.alloc, lists.top()), top().tok->begin};
    lists.pop();
    frames.pop_back();
    goto selSimpleRet;

    /* binds, with the context in `bindsCtx`, into an ExprAttrs in `r` */
binds: {
    /* Not in `bindsStack` yet, which the first piece pushes onto */
    ExprAttrs * attrs = nullptr;
    bool any = false;
    bool more = true;
    if (bindsCtx == Braces || bindsCtx == RecBraces)
        more = parseBindsInPieces(attrs, any);
    bindsStack.push() = Binds{.attrs = attrs, .any = any, .ctx = bindsCtx};
    if (more)
        goto bindsLoop;
    goto bindsEnd;
}

bindsLoop: {
    auto & bs = bindsStack.top();
    if (bs.limit && t >= bs.limit) {
        bs.more = true;
        goto bindsStop;
    }
    auto & tok = peek();
    switch (+tok.kind) {
    case ID:
    case OR_KW:
    case QUOTE:
    case DOLLAR_CURLY:
        push(KBindPath, &tok);
        goto attrPath;
    case INHERIT:
        next();
        inherits.push() = Inherit{};
        if (kind() == LPAREN) {
            next();
            push(KInheritFrom);
            goto expr;
        }
        goto inheritNames;
    default:
        bs.more = false;
        goto bindsStop;
    }
}

bindsStop:
    if (bindsStack.top().entry)
        goto ret;
bindsEnd: {
    auto & bs = bindsStack.top();
    if (!bs.any) {
        switch (+bs.ctx) {
        case Braces:
        case Repl:
            syntaxError(peek(), {INHERIT});
        default:
            break;
        }
    }
    if (!bs.attrs)
        bs.attrs = ex.add<ExprAttrs>(ex.alloc);
    r = {bs.attrs, 0};
    bindsStack.pop();
    goto ret;
}

inheritNames: {
    auto & tok = peek();
    auto & names = inherits.top().names;
    if (tok.kind == ID) {
        next();
        names.emplace_back(sym(tok), at(tok.begin));
        goto inheritNames;
    }
    if (tok.kind == OR_KW) {
        next();
        names.emplace_back(st.symbols.create("or"), at(tok.begin));
        goto inheritNames;
    }
    if (tok.kind == QUOTE) {
        next();
        push(KInheritQuote, &tok);
        goto stringParts;
    }
    if (tok.kind == DOLLAR_CURLY) {
        next();
        push(KInheritDollar, &tok);
        goto expr;
    }
    expect(SEMICOLON, {});
    auto & bs = bindsStack.top();
    if (bs.record)
        bs.record->push_back(Binding{.inherit = std::move(inherits.top())});
    else {
        if (!bs.attrs)
            bs.attrs = ex.add<ExprAttrs>(ex.alloc);
        addInherit(bs.attrs, std::move(inherits.top()));
    }
    inherits.pop();
    bs.any = true;
    goto bindsLoop;
}

    /* formals, onto `formals` */
formalSet: {
    auto & fs = formals.push();
    fs.formals.clear();
    fs.ellipsis = false;
    expect(LBRACE, {LBRACE});
    if (kind() == RBRACE) {
        next();
        goto ret;
    }
    if (kind() == ELLIPSIS) {
        next();
        expect(RBRACE, {RBRACE});
        fs.ellipsis = true;
        goto ret;
    }
}
formalLoop: {
    auto & id = expect(ID, {ID, ELLIPSIS, RBRACE});
    if (kind() == QUESTION) {
        next();
        push(KFormalDefault, &id);
        goto expr;
    }
    formals.top().formals.emplace_back(Formal{at(id.begin), sym(id), nullptr});
    goto formalCheck;
}
formalNext:
    frames.pop_back();
formalCheck: {
    auto k = kind();
    if (k == RBRACE) {
        next();
        goto ret;
    }
    if (k != COMMA)
        syntaxError(peek(), {RBRACE, COMMA});
    next();
    k = kind();
    if (k == RBRACE) {
        next();
        goto ret;
    }
    if (k == ELLIPSIS) {
        next();
        expect(RBRACE, {RBRACE});
        formals.top().ellipsis = true;
        goto ret;
    }
    goto formalLoop;
}
}

bool Parser::runBindsTo(ExprAttrs *& attrs, bool & any, const Token * limit, std::vector<Binding> * record)
{
    auto & bs = bindsStack.push();
    bs = Binds{.attrs = attrs, .any = any, .limit = limit, .record = record, .entry = true};
    run(EntryBindsTo);
    auto & done = bindsStack.top();
    attrs = done.attrs;
    any = done.any;
    bool more = done.more;
    bindsStack.pop();
    return more;
}

void Parser::findBigSets()
{
    std::vector<uint32_t> open;
    auto & ts = tk.tokens;
    for (uint32_t i = 0; i < ts.size(); i++)
        switch (+ts[i].kind) {
        case LBRACE:
        case DOLLAR_CURLY:
            open.push_back(i);
            break;
        case RBRACE:
            if (open.empty())
                break;
            if (i - open.back() >= 2 * minPiece() && ts[open.back()].kind == LBRACE)
                bigSets.emplace_back(open.back(), i);
            open.pop_back();
            break;
        default:
            break;
        }
    std::sort(bigSets.begin(), bigSets.end());
}

/* Cut a big attribute set at bindings that look like they are its own,
   parse every piece after the first at once on its own thread, and take a
   piece only when the bindings before it end exactly where it starts. A
   piece that starts anywhere else, or that fails, is parsed again, so the
   result and the first error are the ones of a serial parse */
bool Parser::parseBindsInPieces(ExprAttrs *& attrs, bool & any)
{
    if (inPiece || bigSets.empty())
        return true;
    uint32_t open = t - tk.tokens.data() - 1;
    auto big = std::lower_bound(bigSets.begin(), bigSets.end(), std::pair{open, uint32_t(0)});
    if (big == bigSets.end() || big->first != open)
        return true;
    uint32_t close = big->second;
    auto & ts = tk.tokens;

    unsigned pieces = std::min<size_t>(maxPieces(), (close - open) / minPiece());
    std::vector<uint32_t> cuts{open + 1};
    int nest = 0;
    for (uint32_t i = open + 1; i < close && cuts.size() < pieces; i++) {
        switch (+ts[i].kind) {
        case LBRACE:
        case DOLLAR_CURLY:
        case LPAREN:
        case LBRACKET:
        case LET:
            nest++;
            continue;
        case RBRACE:
        case RPAREN:
        case RBRACKET:
        case IN_KW:
            nest--;
            continue;
        case SEMICOLON:
            break;
        default:
            continue;
        }
        if (nest != 0 || i + 2 >= close)
            continue;
        auto k = ts[i + 1].kind;
        auto k2 = ts[i + 2].kind;
        if (!((k == ID && (k2 == ASSIGN || k2 == DOT)) || k == QUOTE || k == INHERIT))
            continue;
        if (i + 1 >= open + uint64_t(close - open) * cuts.size() / pieces)
            cuts.push_back(i + 1);
    }
    if (cuts.size() < 2)
        return true;
    cuts.push_back(close);
    size_t k = cuts.size() - 1;

    struct Piece
    {
        std::vector<Binding> bindings;
        std::vector<std::pair<PosIdx, DocComment>> docs;
        std::vector<SourcePath> imports;
        bool ok = false;
    };

    std::vector<Piece> results(k);
    std::exception_ptr error;
    bool more = true;
    auto base = tk.tokens.data();
    /* Doc comments go to the outer set's pieces when this set is in one */
    auto outerDocs = st.deferredDocs;
    auto addDocs = [&](std::vector<std::pair<PosIdx, DocComment>> & docs) {
        if (outerDocs)
            outerDocs->insert(outerDocs->end(), docs.begin(), docs.end());
        else
            for (auto & [pos, doc] : docs)
                ls.positionToDocComment.emplace(pos, doc);
    };
    std::vector<std::pair<PosIdx, DocComment>> docs;
    st.deferredDocs = &docs;
    bool ran = WorkPool::shared().tryRun(
        k,
        [&](size_t i) {
            if (i == 0) {
                try {
                    more = runBindsTo(attrs, any, base + cuts[1], nullptr);
                } catch (...) {
                    error = std::current_exception();
                }
                return;
            }
            auto & r = results[i];
            try {
                ParserState pst = st;
                pst.deferredDocs = &r.docs;
                pst.quiet = true;
                if (st.imports)
                    pst.imports = &r.imports;
                Parser p(pst, ls, text, tk);
                p.t = base + cuts[i];
                p.inPiece = true;
                ExprAttrs * unused = nullptr;
                bool unusedAny = false;
                r.ok = p.runBindsTo(unused, unusedAny, base + cuts[i + 1], &r.bindings) && p.t == base + cuts[i + 1];
            } catch (...) {
            }
        },
        k - 1);
    st.deferredDocs = outerDocs;
    if (!ran)
        return true;
    if (error)
        std::rethrow_exception(error);
    addDocs(docs);

    for (size_t i = 1; i < k && more; i++) {
        if (t < base + cuts[i])
            more = runBindsTo(attrs, any, base + cuts[i], nullptr);
        if (!more)
            break;
        auto & r = results[i];
        if (t != base + cuts[i] || !r.ok) {
            if (t < base + cuts[i + 1])
                more = runBindsTo(attrs, any, base + cuts[i + 1], nullptr);
            continue;
        }
        addDocs(r.docs);
        if (st.imports)
            st.imports->insert(st.imports->end(), r.imports.begin(), r.imports.end());
        if (!attrs && !r.bindings.empty())
            attrs = st.exprs.add<ExprAttrs>(st.exprs.alloc);
        for (auto & b : r.bindings) {
            if (b.inherit)
                addInherit(attrs, std::move(*b.inherit));
            else
                st.addAttr(
                    attrs,
                    std::move(b.path),
                    ParserLocation{int(b.begin), 0, 0, 0},
                    b.e.e,
                    ParserLocation{int(b.e.begin), 0, 0, 0});
            any = true;
        }
        t = base + cuts[i + 1];
    }
    return more;
}

void Parser::addInherit(ExprAttrs * attrs, Inherit && inherit)
{
    if (!inherit.from) {
        for (auto & [i, iPos] : inherit.names) {
            if (attrs->attrs->find(i.symbol) != attrs->attrs->end())
                st.dupAttr(i.symbol, iPos, (*attrs->attrs)[i.symbol].pos);
            attrs->attrs->emplace(
                i.symbol,
                ExprAttrs::AttrDef(st.exprs.add<ExprVar>(iPos, i.symbol), iPos, ExprAttrs::AttrDef::Kind::Inherited));
        }
        return;
    }

    if (!attrs->inheritFromExprs)
        attrs->inheritFromExprs = std::make_unique<std::pmr::vector<Expr *>>(st.exprs.alloc);
    attrs->inheritFromExprs->push_back(inherit.from);
    auto fromExpr = st.exprs.add<ExprInheritFrom>(at(inherit.fromBegin), attrs->inheritFromExprs->size() - 1);
    for (auto & [i, iPos] : inherit.names) {
        if (attrs->attrs->find(i.symbol) != attrs->attrs->end())
            st.dupAttr(i.symbol, iPos, (*attrs->attrs)[i.symbol].pos);
        attrs->attrs->emplace(
            i.symbol,
            ExprAttrs::AttrDef(
                st.exprs.add<ExprSelect>(st.exprs.alloc, iPos, fromExpr, i.symbol),
                iPos,
                ExprAttrs::AttrDef::Kind::InheritedFrom));
    }
}

Expr * parse(
    char * text,
    size_t length,
    Pos::Origin origin,
    const SourcePath & basePath,
    Exprs & exprs,
    SymbolTable & symbols,
    const EvalSettings & settings,
    PosTable & positions,
    DocCommentMap & docComments,
    const ref<SourceAccessor> rootFS,
    bool repl,
    std::vector<SourcePath> * imports,
    bool quiet)
{
    LexerState lexerState{
        .positionToDocComment = docComments,
        .positions = positions,
        .origin = positions.addOrigin(origin, length),
    };
    ParserState state{
        .lexerState = lexerState,
        .exprs = exprs,
        .symbols = symbols,
        .positions = positions,
        .basePath = basePath,
        .origin = lexerState.origin,
        .rootFS = rootFS,
        .settings = settings,
        .quiet = quiet,
        .imports = imports,
    };

    assert(length >= 2 && text[length - 1] == 0 && text[length - 2] == 0);
    auto tokens = lexer::lex(text, length - 2, lexerState, repl);
    Finally recycle([&] { lexer::recycle(std::move(tokens)); });
    Parser parser(state, lexerState, text, tokens);
    return parser.parseStart(repl);
}

} // namespace

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
    const ref<SourceAccessor> rootFS,
    std::vector<SourcePath> * imports,
    bool quiet)
{
    return parse(
        text, length, origin, basePath, exprs, symbols, settings, positions, docComments, rootFS, false, imports, quiet);
}

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
    const ref<SourceAccessor> rootFS)
{
    auto result =
        parse(text, length, origin, basePath, exprs, symbols, settings, positions, docComments, rootFS, true, nullptr, false);
    assert(result);
    auto bindings = dynamic_cast<ExprAttrs *>(result);
    assert(bindings);
    return bindings;
}

} // namespace nix
