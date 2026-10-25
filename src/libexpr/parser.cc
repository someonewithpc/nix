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
#include "nix/util/finally.hh"
#include "nix/util/users.hh"
#include "nix/util/util.hh"

#include "lexer.hh"

#include <bit>
#include <cstring>

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

    static uint64_t hashOf(const char * p, size_t len)
    {
        constexpr uint64_t k = 0x9e3779b97f4a7c15ull;
        uint64_t h = len * k;
        while (len >= 8) {
            uint64_t w;
            memcpy(&w, p, 8);
            h = (std::rotl(h, 29) ^ w) * k;
            p += 8;
            len -= 8;
        }
        if (len) {
            uint64_t w = 0;
            memcpy(&w, p, len);
            h = (std::rotl(h, 29) ^ w) * k;
        }
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
        uint64_t h = hashOf(s.data(), s.size());
        size_t mask = entries.size() - 1;
        size_t i = h & mask;
        for (; entries[i].sym; i = (i + 1) & mask) {
            auto & e = entries[i];
            if (e.hash != h || e.len != s.size())
                continue;
            if (memcmp(s.size() <= inlineLen ? e.text : e.ptr, s.data(), s.size()) == 0)
                return e.sym;
        }
        return insert(symbols, s, h, i);
    }
};

thread_local SymbolCache symbolCache;

/* Nesting the parser handles before giving up instead of running out of
   stack */
constexpr unsigned maxDepth = 20000;

class Parser
{
    ParserState & st;
    LexerState & ls;
    char * const text;
    const Tokens & tk;
    const Token * t;
    unsigned depth = 0;

    PosIdx at(uint32_t offset)
    {
        return st.positions.add(st.origin, offset);
    }

    [[noreturn]] void lexError(const Token & tok)
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

    const Token & peek()
    {
        if (t->kind == LEX_ERROR) [[unlikely]]
            lexError(*t);
        return *t;
    }

    Kind kind()
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

    const Token & next()
    {
        auto & tok = peek();
        t++;
        return tok;
    }

    [[noreturn]] void syntaxError(const Token & tok, std::initializer_list<Kind> expected = {})
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

    const Token & expect(Kind k, std::initializer_list<Kind> expected)
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
        return symbolCache.get(st.symbols, raw(tok));
    }

    void setDocPosition(ExprLambda * lambda, uint32_t begin)
    {
        auto it = ls.positionToDocComment.find(at(begin));
        if (it != ls.positionToDocComment.end())
            lambda->setDocComment(it->second);
    }

    Expr * makeCall(PosIdx pos, Expr * fn, Expr * arg)
    {
        if (auto e2 = dynamic_cast<ExprCall *>(fn)) {
            e2->args->push_back(arg);
            return fn;
        }
        return st.exprs.add<ExprCall>(pos, fn, {arg});
    }

    struct DepthGuard
    {
        Parser & p;

        DepthGuard(Parser & p, const Token & tok)
            : p(p)
        {
            if (++p.depth > maxDepth)
                throw ParseError(
                    {.msg = HintFmt("expression is nested too deeply"), .pos = p.st.positions[p.at(tok.begin)]});
        }

        ~DepthGuard()
        {
            p.depth--;
        }
    };

    E parseExpr();
    E parseExprFunction();
    E parseExprIf();
    E parseExprOp(int minPrec);
    E parseUnary();
    E parseExprApp();
    E parseExprSelect();
    E parseSimple();
    bool isFormals();
    FormalsBuilder parseFormalSet();
    std::vector<AttrName> parseAttrPath();
    ToBeStringyExpr parseStringParts();
    std::vector<std::pair<PosIdx, Expr *>> parseInterpolated(bool path);
    Expr * parsePathStart(const Token & tok);

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

    ExprAttrs * parseBinds(BindsContext ctx);
    void parseInherit(ExprAttrs * attrs);

public:
    Parser(ParserState & st, LexerState & ls, char * text, const Tokens & tk)
        : st(st)
        , ls(ls)
        , text(text)
        , tk(tk)
        , t(tk.tokens.data())
    {
        symbolCache.start(st.symbols);
    }

    Expr * parseStart(bool repl);
};

Expr * Parser::parseStart(bool repl)
{
    if (repl && kind() == REPL_BINDINGS) {
        next();
        auto attrs = parseBinds(Repl);
        expect(EOF_, {EOF_});
        return attrs;
    }
    auto e = parseExpr();
    expect(EOF_, {EOF_});
    return e.e;
}

E Parser::parseExpr()
{
    DepthGuard guard(*this, *t);
    return parseExprFunction();
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

FormalsBuilder Parser::parseFormalSet()
{
    FormalsBuilder fs;
    fs.ellipsis = false;
    expect(LBRACE, {LBRACE});
    if (kind() == RBRACE) {
        next();
        return fs;
    }
    if (kind() == ELLIPSIS) {
        next();
        expect(RBRACE, {RBRACE});
        fs.ellipsis = true;
        return fs;
    }
    while (true) {
        auto & id = expect(ID, {ID, ELLIPSIS, RBRACE});
        Expr * def = nullptr;
        if (kind() == QUESTION) {
            next();
            def = parseExpr().e;
        }
        fs.formals.emplace_back(Formal{at(id.begin), sym(id), def});
        auto k = kind();
        if (k == RBRACE) {
            next();
            return fs;
        }
        if (k != COMMA)
            syntaxError(peek(), {RBRACE, COMMA});
        next();
        k = kind();
        if (k == RBRACE) {
            next();
            return fs;
        }
        if (k == ELLIPSIS) {
            next();
            expect(RBRACE, {RBRACE});
            fs.ellipsis = true;
            return fs;
        }
    }
}

E Parser::parseExprFunction()
{
    auto & tok = peek();
    switch (+tok.kind) {
    case ID: {
        auto k = kindAt(1);
        if (k == COLON) {
            next();
            next();
            auto body = parseExprFunction();
            auto me = st.exprs.add<ExprLambda>(at(tok.begin), sym(tok), body.e);
            setDocPosition(me, tok.begin);
            return {me, tok.begin};
        }
        if (k == AT) {
            next();
            next();
            auto fs = parseFormalSet();
            expect(COLON, {COLON});
            auto body = parseExprFunction();
            auto arg = sym(tok);
            st.validateFormals(fs, at(tok.begin), arg);
            auto me = st.exprs.add<ExprLambda>(st.positions, st.exprs.alloc, at(tok.begin), arg, fs, body.e);
            setDocPosition(me, tok.begin);
            return {me, tok.begin};
        }
        break;
    }
    case LBRACE:
        if (isFormals()) {
            auto fs = parseFormalSet();
            auto & k = peek();
            if (k.kind == COLON) {
                next();
                auto body = parseExprFunction();
                st.validateFormals(fs);
                auto me = st.exprs.add<ExprLambda>(st.positions, st.exprs.alloc, at(tok.begin), fs, body.e);
                setDocPosition(me, tok.begin);
                return {me, tok.begin};
            }
            if (k.kind == AT) {
                next();
                auto & id = expect(ID, {ID});
                expect(COLON, {COLON});
                auto body = parseExprFunction();
                auto arg = sym(id);
                st.validateFormals(fs, at(tok.begin), arg);
                auto me = st.exprs.add<ExprLambda>(st.positions, st.exprs.alloc, at(tok.begin), arg, fs, body.e);
                setDocPosition(me, tok.begin);
                return {me, tok.begin};
            }
            syntaxError(k, {COLON, AT});
        }
        break;
    case ASSERT: {
        next();
        auto cond = parseExpr();
        expect(SEMICOLON, {SEMICOLON});
        auto body = parseExprFunction();
        return {st.exprs.add<ExprAssert>(at(tok.begin), cond.e, body.e), tok.begin};
    }
    case WITH: {
        next();
        auto attrs = parseExpr();
        expect(SEMICOLON, {SEMICOLON});
        auto body = parseExprFunction();
        return {st.exprs.add<ExprWith>(at(tok.begin), attrs.e, body.e), tok.begin};
    }
    case LET: {
        if (kindAt(1) == LBRACE)
            break;
        next();
        auto binds = parseBinds(LetIn);
        expect(IN_KW, {IN_KW, INHERIT});
        auto body = parseExprFunction();
        if (!binds->dynamicAttrs->empty())
            throw ParseError({.msg = HintFmt("dynamic attributes not allowed in let"), .pos = st.positions[at(tok.begin)]});
        return {st.exprs.add<ExprLet>(binds, body.e), tok.begin};
    }
    default:
        break;
    }
    return parseExprIf();
}

E Parser::parseExprIf()
{
    auto & tok = peek();
    if (tok.kind == IF) {
        next();
        auto cond = parseExpr();
        expect(THEN, {THEN});
        auto then = parseExpr();
        expect(ELSE, {ELSE});
        auto else_ = parseExpr();
        return {st.exprs.add<ExprIf>(at(tok.begin), cond.e, then.e, else_.e), tok.begin};
    }

    auto lhs = parseExprOp(PImpl);

    if (kind() == PIPE_FROM) {
        /* Right associative */
        std::vector<std::pair<uint32_t, Expr *>> chain;
        chain.emplace_back(0, lhs.e);
        while (kind() == PIPE_FROM) {
            auto & op = next();
            chain.emplace_back(op.begin, parseExprOp(PImpl).e);
        }
        Expr * acc = chain.back().second;
        for (size_t i = chain.size() - 1; i > 0; i--)
            acc = makeCall(at(chain[i].first), chain[i - 1].second, acc);
        return {acc, lhs.begin};
    }

    if (kind() == PIPE_INTO) {
        Expr * acc = lhs.e;
        while (kind() == PIPE_INTO) {
            auto & op = next();
            auto rhs = parseExprOp(PImpl);
            acc = makeCall(at(op.begin), rhs.e, acc);
        }
        return {acc, lhs.begin};
    }

    return lhs;
}

E Parser::parseUnary()
{
    auto & tok = peek();
    if (tok.kind == BANG) {
        next();
        DepthGuard guard(*this, tok);
        auto operand = parseExprOp(PNot + 1);
        return {st.exprs.add<ExprOpNot>(operand.e), tok.begin};
    }
    if (tok.kind == MINUS) {
        next();
        DepthGuard guard(*this, tok);
        auto operand = parseExprOp(PNegate + 1);
        return {
            st.exprs.add<ExprCall>(
                at(tok.begin), st.exprs.add<ExprVar>(st.s.sub), {st.exprs.add<ExprInt>(0), operand.e}),
            tok.begin};
    }
    return parseExprApp();
}

E Parser::parseExprOp(int minPrec)
{
    auto lhs = parseUnary();
    Prec lastNonAssoc = PNone;

    while (true) {
        auto & op = peek();
        Prec p = precs[op.kind];
        if (p == PNone || p < minPrec)
            break;
        if (p == lastNonAssoc)
            syntaxError(op);
        next();

        if (op.kind == QUESTION) {
            auto path = parseAttrPath();
            lhs.e = st.exprs.add<ExprOpHasAttr>(st.exprs.alloc, lhs.e, path);
            lastNonAssoc = PNone;
            continue;
        }

        DepthGuard guard(*this, op);
        auto rhs = parseExprOp(rightAssoc(op.kind) ? p : p + 1);
        auto pos = at(op.begin);
        auto & ex = st.exprs;
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
            e = ex.add<ExprConcatStrings>(
                ex.alloc, pos, false, {{at(lhs.begin), lhs.e}, {at(rhs.begin), rhs.e}});
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
        lastNonAssoc = nonAssoc(p) ? p : PNone;
    }

    return lhs;
}

E Parser::parseExprApp()
{
    auto first = parseExprSelect();
    first.e->resetCursedOr();
    Expr * acc = first.e;
    while (simpleStarts[kind()]) {
        auto arg = parseExprSelect();
        acc = makeCall(at(first.begin), acc, arg.e);
        arg.e->warnIfCursedOr(st.symbols, st.positions);
    }
    return {acc, first.begin};
}

E Parser::parseExprSelect()
{
    auto s = parseSimple();
    auto & tok = peek();
    if (tok.kind == DOT) {
        next();
        auto path = parseAttrPath();
        if (kind() == OR_KW) {
            next();
            auto def = parseExprSelect();
            auto e = st.exprs.add<ExprSelect>(st.exprs.alloc, at(s.begin), s.e, path, def.e);
            def.e->warnIfCursedOr(st.symbols, st.positions);
            return {e, s.begin};
        }
        return {st.exprs.add<ExprSelect>(st.exprs.alloc, at(s.begin), s.e, path, nullptr), s.begin};
    }
    if (tok.kind == OR_KW) {
        next();
        return {
            st.exprs.add<ExprCall>(
                at(s.begin), s.e, {st.exprs.add<ExprVar>(at(s.begin), st.s.or_)}, st.positions.add(st.origin, tok.end)),
            s.begin};
    }
    return s;
}

std::vector<AttrName> Parser::parseAttrPath()
{
    std::vector<AttrName> path;
    auto addStringy = [&](ToBeStringyExpr && x) {
        x.visit(
            overloaded{
                [&](std::string_view str) { path.emplace_back(st.symbols.create(str)); },
                [&](Expr * expr) { path.emplace_back(expr); }});
    };
    while (true) {
        auto & tok = peek();
        switch (+tok.kind) {
        case ID:
            next();
            path.emplace_back(sym(tok));
            break;
        case OR_KW:
            next();
            path.emplace_back(st.symbols.create("or"));
            break;
        case QUOTE: {
            next();
            auto parts = parseStringParts();
            expect(QUOTE, {QUOTE});
            addStringy(std::move(parts));
            break;
        }
        case DOLLAR_CURLY: {
            next();
            auto e = parseExpr();
            expect(RBRACE, {RBRACE});
            addStringy(ToBeStringyExpr(e.e));
            break;
        }
        default:
            syntaxError(tok, {ID, OR_KW, DOLLAR_CURLY, QUOTE});
        }
        if (kind() != DOT)
            return path;
        next();
    }
}

ToBeStringyExpr Parser::parseStringParts()
{
    auto & tok = peek();
    if (tok.kind == STR && kindAt(1) != DOLLAR_CURLY) {
        next();
        return ToBeStringyExpr(strValue(tok));
    }
    if (tok.kind != STR && tok.kind != DOLLAR_CURLY)
        return ToBeStringyExpr(std::string_view());
    auto parts = parseInterpolated(false);
    return ToBeStringyExpr(st.exprs.add<ExprConcatStrings>(st.exprs.alloc, at(tok.begin), true, parts));
}

std::vector<std::pair<PosIdx, Expr *>> Parser::parseInterpolated(bool path)
{
    std::vector<std::pair<PosIdx, Expr *>> parts;
    while (true) {
        auto & tok = peek();
        if (tok.kind == STR) {
            next();
            parts.emplace_back(at(tok.begin), st.exprs.add<ExprString>(st.exprs.alloc, path ? raw(tok) : strValue(tok)));
        } else if (tok.kind == DOLLAR_CURLY) {
            next();
            auto e = parseExpr();
            expect(RBRACE, {RBRACE});
            parts.emplace_back(at(tok.begin), e.e);
        } else
            return parts;
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
        diagnose(st.settings.lintAbsolutePathLiterals, [&](bool) -> std::optional<ParseError> {
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
        diagnose(st.settings.lintAbsolutePathLiterals, [&](bool) -> std::optional<ParseError> {
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
    diagnose(st.settings.lintShortPathLiterals, [&](bool) -> std::optional<ParseError> {
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

E Parser::parseSimple()
{
    auto & tok = peek();
    auto & ex = st.exprs;
    switch (+tok.kind) {
    case ID: {
        next();
        auto s = raw(tok);
        if (s == "__curPos")
            return {ex.add<ExprPos>(at(tok.begin)), tok.begin};
        return {ex.add<ExprVar>(at(tok.begin), sym(tok)), tok.begin};
    }
    case INT_LIT:
        next();
        return {ex.add<ExprInt>(NixInt(int64_t(tk.numbers[tok.aux]))), tok.begin};
    case FLOAT_LIT:
        next();
        return {ex.add<ExprFloat>(std::bit_cast<double>(tk.numbers[tok.aux])), tok.begin};
    case QUOTE: {
        next();
        auto parts = parseStringParts();
        expect(QUOTE, {QUOTE});
        return {parts.toExpr(ex), tok.begin};
    }
    case IND_STRING_OPEN: {
        next();
        std::vector<std::pair<PosIdx, std::variant<Expr *, StringToken>>> parts;
        while (true) {
            auto & part = peek();
            if (part.kind == IND_STR) {
                next();
                parts.emplace_back(at(part.begin), indStrValue(part));
            } else if (part.kind == DOLLAR_CURLY) {
                next();
                auto e = parseExpr();
                expect(RBRACE, {RBRACE});
                parts.emplace_back(at(part.begin), e.e);
            } else
                break;
        }
        expect(IND_STRING_CLOSE, {IND_STR, DOLLAR_CURLY, IND_STRING_CLOSE});
        return {st.stripIndentation(at(tok.begin), parts), tok.begin};
    }
    case PATH:
    case HPATH: {
        next();
        auto start = parsePathStart(tok);
        if (kind() == PATH_END) {
            next();
            return {start, tok.begin};
        }
        /* The grammar only takes a literal segment before an interpolation */
        if (kind() == STR && kindAt(1) != DOLLAR_CURLY)
            syntaxError(t[1], {DOLLAR_CURLY});
        auto parts = parseInterpolated(true);
        expect(PATH_END, {STR, PATH_END, DOLLAR_CURLY});
        parts.insert(parts.begin(), {at(tok.begin), start});
        return {ex.add<ExprConcatStrings>(ex.alloc, at(tok.begin), false, parts), tok.begin};
    }
    case SPATH: {
        next();
        auto s = raw(tok);
        std::string_view path(s.data() + 1, s.size() - 2);
        return {
            ex.add<ExprCall>(
                at(tok.begin),
                ex.add<ExprVar>(st.s.findFile),
                {ex.add<ExprVar>(st.s.nixPath), ex.add<ExprString>(ex.alloc, path)}),
            tok.begin};
    }
    case URI: {
        next();
        auto s = raw(tok);
        diagnose(st.settings.lintUrlLiterals, [&](bool fatal) -> std::optional<ParseError> {
            return ParseError(
                {.msg = HintFmt(
                     "URL literals are %s. Consider using a string literal \"%s\" instead",
                     fatal ? "disallowed" : "discouraged",
                     s),
                 .pos = st.positions[at(tok.begin)]});
        });
        return {ex.add<ExprString>(ex.alloc, s), tok.begin};
    }
    case LPAREN: {
        next();
        auto e = parseExpr();
        expect(RPAREN, {RPAREN});
        return {e.e, tok.begin};
    }
    case LET: {
        next();
        expect(LBRACE, {LBRACE});
        auto binds = parseBinds(RecBraces);
        expect(RBRACE, {INHERIT, RBRACE});
        binds->recursive = true;
        binds->pos = at(tok.begin);
        return {ex.add<ExprSelect>(ex.alloc, at(tok.begin), binds, st.s.body), tok.begin};
    }
    case REC: {
        next();
        expect(LBRACE, {LBRACE});
        auto binds = parseBinds(RecBraces);
        expect(RBRACE, {INHERIT, RBRACE});
        binds->recursive = true;
        binds->pos = at(tok.begin);
        return {binds, tok.begin};
    }
    case LBRACE: {
        next();
        if (kind() == RBRACE) {
            next();
            return {ex.add<ExprAttrs>(at(tok.begin)), tok.begin};
        }
        auto binds = parseBinds(Braces);
        expect(RBRACE, {INHERIT});
        binds->pos = at(tok.begin);
        return {binds, tok.begin};
    }
    case LBRACKET: {
        next();
        std::pmr::vector<Expr *> list;
        while (simpleStarts[kind()]) {
            auto e = parseExprSelect();
            list.push_back(e.e);
            e.e->warnIfCursedOr(st.symbols, st.positions);
        }
        expect(RBRACKET, {});
        return {ex.add<ExprList>(ex.alloc, list), tok.begin};
    }
    default:
        syntaxError(tok);
    }
}

ExprAttrs * Parser::parseBinds(BindsContext ctx)
{
    ExprAttrs * attrs = nullptr;
    bool any = false;

    while (true) {
        auto & tok = peek();
        switch (+tok.kind) {
        case ID:
        case OR_KW:
        case QUOTE:
        case DOLLAR_CURLY: {
            auto path = parseAttrPath();
            expect(ASSIGN, {DOT, ASSIGN});
            auto e = parseExpr();
            expect(SEMICOLON, {SEMICOLON});
            if (!attrs)
                attrs = st.exprs.add<ExprAttrs>();
            st.addAttr(attrs, std::move(path), ParserLocation{int(tok.begin), 0, 0, 0}, e.e, ParserLocation{int(e.begin), 0, 0, 0});
            any = true;
            continue;
        }
        case INHERIT:
            next();
            if (!attrs)
                attrs = st.exprs.add<ExprAttrs>();
            parseInherit(attrs);
            any = true;
            continue;
        default:
            break;
        }
        break;
    }

    if (!any) {
        switch (+ctx) {
        case Braces:
        case Repl:
            syntaxError(peek(), {INHERIT});
        default:
            break;
        }
    }

    if (!attrs)
        attrs = st.exprs.add<ExprAttrs>();
    return attrs;
}

void Parser::parseInherit(ExprAttrs * attrs)
{
    Expr * from = nullptr;
    uint32_t fromBegin = 0;
    if (kind() == LPAREN) {
        next();
        auto e = parseExpr();
        expect(RPAREN, {RPAREN});
        from = e.e;
        fromBegin = e.begin;
    }

    std::vector<std::pair<AttrName, PosIdx>> names;
    while (true) {
        auto & tok = peek();
        if (tok.kind == ID) {
            next();
            names.emplace_back(sym(tok), at(tok.begin));
        } else if (tok.kind == OR_KW) {
            next();
            names.emplace_back(st.symbols.create("or"), at(tok.begin));
        } else if (tok.kind == QUOTE || tok.kind == DOLLAR_CURLY) {
            next();
            ToBeStringyExpr x;
            if (tok.kind == QUOTE) {
                x = parseStringParts();
                expect(QUOTE, {QUOTE});
            } else {
                auto e = parseExpr();
                expect(RBRACE, {RBRACE});
                x = ToBeStringyExpr(e.e);
            }
            x.visit(
                overloaded{
                    [&](std::string_view str) { names.emplace_back(st.symbols.create(str), at(tok.begin)); },
                    [&](Expr * expr) {
                        throw ParseError(
                            {.msg = HintFmt("dynamic attributes not allowed in inherit"),
                             .pos = st.positions[at(tok.begin)]});
                    }});
        } else
            break;
    }
    expect(SEMICOLON, {});

    if (!from) {
        for (auto & [i, iPos] : names) {
            if (attrs->attrs->find(i.symbol) != attrs->attrs->end())
                st.dupAttr(i.symbol, iPos, (*attrs->attrs)[i.symbol].pos);
            attrs->attrs->emplace(
                i.symbol,
                ExprAttrs::AttrDef(st.exprs.add<ExprVar>(iPos, i.symbol), iPos, ExprAttrs::AttrDef::Kind::Inherited));
        }
        return;
    }

    if (!attrs->inheritFromExprs)
        attrs->inheritFromExprs = std::make_unique<std::pmr::vector<Expr *>>();
    attrs->inheritFromExprs->push_back(from);
    auto fromExpr = st.exprs.add<ExprInheritFrom>(at(fromBegin), attrs->inheritFromExprs->size() - 1);
    for (auto & [i, iPos] : names) {
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
    bool repl)
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
    const ref<SourceAccessor> rootFS)
{
    return parse(text, length, origin, basePath, exprs, symbols, settings, positions, docComments, rootFS, false);
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
    auto result = parse(text, length, origin, basePath, exprs, symbols, settings, positions, docComments, rootFS, true);
    assert(result);
    auto bindings = dynamic_cast<ExprAttrs *>(result);
    assert(bindings);
    return bindings;
}

} // namespace nix
