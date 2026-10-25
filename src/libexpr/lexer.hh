#pragma once
///@file

#include <cstdint>
#include <string>
#include <vector>

#include "nix/expr/parser-state.hh"

namespace nix::lexer {

/**
 * Token kinds. The numbering matches the symbol numbers of the Bison
 * grammar this lexer replaced, because syntax errors list the expected
 * tokens in this order and the messages must not change
 */
enum Kind : uint8_t {
    EOF_ = 0,
    UNDEF = 2,
    ID = 3,
    STR = 4,
    IND_STR = 5,
    INT_LIT = 6,
    FLOAT_LIT = 7,
    PATH = 8,
    HPATH = 9,
    SPATH = 10,
    PATH_END = 11,
    URI = 12,
    IF = 13,
    THEN = 14,
    ELSE = 15,
    ASSERT = 16,
    WITH = 17,
    LET = 18,
    IN_KW = 19,
    REC = 20,
    INHERIT = 21,
    EQ = 22,
    NEQ = 23,
    LEQ = 24,
    GEQ = 25,
    UPDATE = 26,
    CONCAT = 27,
    AND = 28,
    OR = 29,
    IMPL = 30,
    OR_KW = 31,
    PIPE_FROM = 32,
    PIPE_INTO = 33,
    DOLLAR_CURLY = 34,
    IND_STRING_OPEN = 35,
    IND_STRING_CLOSE = 36,
    ELLIPSIS = 37,
    REPL_BINDINGS = 38,
    LT = 39,
    GT = 40,
    PLUS = 42,
    MINUS = 43,
    STAR = 44,
    SLASH = 45,
    QUESTION = 46,
    COLON = 48,
    AT = 49,
    SEMICOLON = 50,
    BANG = 51,
    DOT = 52,
    QUOTE = 53,
    LPAREN = 54,
    RPAREN = 55,
    LBRACE = 56,
    RBRACE = 57,
    LBRACKET = 58,
    RBRACKET = 59,
    ASSIGN = 60,
    COMMA = 61,
    /**
     * Not a token of the grammar: an error the lexer found. The parser
     * throws it when it reaches the token, which is when the flex lexer
     * would have thrown it
     */
    LEX_ERROR = 62,
    NUM_KINDS
};

const char * kindName(Kind k);

/**
 * How the text of a `STR` or `IND_STR` token is to be read
 */
enum StrFlavour : uint8_t {
    /** The bytes of the token, as they are */
    Raw,
    /** Unescaped in place; the value is the first `aux` bytes */
    Unescaped,
    /** An indented string body that still has its indentation */
    Indented,
    /** `$`, `''` or `'` from an indented string, as a `Lit` in `aux` */
    Literal,
    /** Still to be unescaped: the value is the last `aux` bytes of the
        token. Only the lexer sees these */
    Escaped,
};

enum Lit : uint32_t { LitDollar, LitQuotes, LitQuote };

struct Token
{
    Kind kind;
    StrFlavour flavour = Raw;
    uint32_t begin;
    uint32_t end;
    /**
     * Length of an unescaped string, the `Lit` of a `Literal`, the index
     * of a number in `Tokens::numbers`, or the index of a `LexError`
     */
    uint32_t aux = 0;
};

struct LexError
{
    enum Kind : uint8_t {
        InvalidInteger,
        InvalidFloat,
        PipeOperators,
        NullByte,
        TrailingSlash,
    } kind;
    uint32_t offset;
    std::string text;
};

struct Tokens
{
    std::vector<Token> tokens;
    /** Integers, and floats as their bits */
    std::vector<uint64_t> numbers;
    std::vector<LexError> errors;
};

/**
 * Split `text` into tokens
 *
 * `text` is a `ParseBuffer`'s, so zero bytes follow it up to the end of
 * its last 64-byte block and for a whole block after; escape sequences are decoded in place, so the buffer must be
 * writable and must outlive the tokens; doc comments are recorded in
 * `lexerState` exactly where the flex lexer recorded them
 */
Tokens lex(char * text, size_t length, LexerState & lexerState, bool replBindings);

/**
 * Give the buffers of `tokens` back for the next `lex` to fill, so that
 * their pages stay mapped
 */
void recycle(Tokens && tokens);

} // namespace nix::lexer
