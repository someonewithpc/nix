#!/usr/bin/env python
# profile-classify.py < perf-script-output: bucket perf samples by what the
# evaluator was doing
#
#   perf record -e cycles:u --user-callchains -g -F 1999 -o p.data -- nix eval ...
#   perf script --no-inline -i p.data -F comm,tid,ip,sym > p.script
#   python profile-classify.py < p.script
#
# A sample goes to the innermost frame that matches a category. Samples whose
# leaf is generic (malloc, memcpy, GC allocation, forcing, symbols) are also
# folded into the nearest frame above that says what the work was for.
# TID=<tid> keeps one thread, such as the main thread
import sys, re, collections
# perf script -F comm,tid,ip,sym,dso  -> per-sample blocks. Attribute each sample to the innermost frame that matches a category.
cats = [
 ("gc-mark/sweep",   re.compile(r'^GC_(mark|push|help|reclaim|clear|stopped|try_to_collect|collect|finish|start|add_to_black|header_cache|is_black|promote|apply_to|sweep|freehblk|remove|install_header|get_first|scratch|expand|unmap|merge|initiate|should|wait|notify|do_local|steal|return_mark|block_was|set_fl|reset|generic_lock)')),
 ("gc-alloc",        re.compile(r'^GC_|allocValue|allocEnv|allocBytes|ListBuilder::ListBuilder|allocBindings|buildBindings')),
 ("malloc/free",     re.compile(r'^(mi_|_mi_|operator new|operator delete|malloc|free|calloc|realloc|__libc_malloc|cfree)')),
 ("json",            re.compile(r'nlohmann|printValueAsJSON|StructuredAttrs')),
 ("drv-unparse/hash",re.compile(r'derivation::unparse|masked::|infoForDerivation|computeStorePath|fillInOutputPaths|hashDerivation|printString|printUnquoted|makeFixedOutputPath|makeStorePath|makeOutputPath|writeDerivation|sha256|SHA256|EVP_|base32|base16|hashString|HashSink|Hash::')),
 ("parse",           re.compile(r'Parser::|Lexer|lexer::|parseExprFromBuf|ParserState|bindVars|ImportPrefetcher|ParseBuffer|\(anonymous namespace\)::parse\(|Exprs::|stripIndentation')),
 ("fs/accessor",     re.compile(r'isAllowed|maybeLstat|SourceAccessor|pathExists|readFile|readDirectory|lstat|statx|CanonPath|std::filesystem|PosixSourceAccessor|fetchToStore|copyPathToStore|io_uring|readLink')),
 ("symbols",         re.compile(r'SymbolTable|SymbolStr|concurrent_table')),
 ("regex",           re.compile(r'std::__detail::_(Executor|BracketMatcher|NFA|Compiler|Scanner|State|RegexTranslator|AnyMatcher|CharMatcher)|regex_traits|basic_regex|regex_')),
 ("string-ctx",      re.compile(r'NixStringContext|copyContext|StringWithContext|Context::fromBuilder|_Rb_tree.*NixStringContextElem|StorePath::StorePath|printStorePath|SingleDerivedPath|DerivedPath|__variant')),
 ("bindings-iter",   re.compile(r'Bindings::iterator|BindingsCursor|__adjust_heap|__push_heap|__pop_heap|lexicographicOrder|finishSizeIfNecessary|Bindings::sort')),
 ("attr-lookup",     re.compile(r'Bindings::get')),
 ("derivationStrict",re.compile(r'derivationStrictInternal|prim_derivationStrict')),
 ("coerceToString",  re.compile(r'coerceToString|peelToStringOutPath')),
 ("eqValues",        re.compile(r'eqValues')),
 ("op-update",       re.compile(r'ExprOpUpdate')),
 ("concatStrings",   re.compile(r'ExprConcatStrings|concatStrings')),
 ("concatLists",     re.compile(r'concatLists|ExprOpConcatLists')),
 ("primop",          re.compile(r'nix::prim_(\w+)')),
 ("callFunction",    re.compile(r'EvalState::callFunction|ExprCall::eval|ExprLambda')),
 ("select/hasAttr",  re.compile(r'ExprSelect|ExprOpHasAttr')),
 ("var",             re.compile(r'ExprVar|lookupVar')),
 ("attrs/let/list",  re.compile(r'ExprAttrs|ExprLet|ExprList|ExprWith')),
 ("thunk/force",     re.compile(r'maybeThunk|forceValue|forceAttrs|forceList|handleEval|getInternalType|isa<|ValueStorage|mkThunk')),
 ("other-expr",      re.compile(r'nix::Expr|evalBool|evalAttrs|EvalState::')),
 ("libc-mem",        re.compile(r'^__mem|^__str|^mem|^str')),
 ("dyn-link",        re.compile(r'^_dl_|do_lookup_x|__tls_get_addr|_dl_relocate|\$plt$|@plt$')),
]
generic = {"malloc/free","libc-mem","gc-alloc","thunk/force","symbols","dyn-link","string-ctx"}
tot = collections.Counter(); prim = collections.Counter(); threads = collections.Counter()
own = collections.Counter()   # leaf-only category
attributed = collections.Counter()  # generic leaf attributed to nearest non-generic owner
frames = []; hdr = None; n = 0
import os
ONLY = os.environ.get("TID")
def flush():
    global frames, hdr, n
    if hdr is None: return
    if ONLY and hdr.split()[-1] != ONLY: frames = []; return
    n += 1
    comm = hdr.split()[0]
    threads[hdr.split()[-1]] += 1
    leafcat = None; owner = None
    for i, f in enumerate(frames):
        c = None
        for name, rx in cats:
            m = rx.search(f)
            if m:
                c = name
                if name == "primop": c = "primop:" + m.group(1)
                break
        if c is None: continue
        if leafcat is None:
            leafcat = c
            if c not in generic: owner = c; break
        elif c not in generic:
            owner = c; break
    own[leafcat or "unclassified:" + (frames[0][:60] if frames else "?")] += 1
    attributed[(owner or "none", leafcat or "unclassified")] += 1
    frames = []
for line in sys.stdin:
    if not line.strip():
        flush(); hdr = None; continue
    if not line.startswith((' ', '\t')):
        flush(); hdr = line; frames = []; continue
    parts = line.strip().split(None, 1)
    frames.append(parts[1] if len(parts) > 1 else parts[0])
flush()
print("samples", n)
tt = threads.most_common(6)
print("threads (tid: share):", ", ".join(f"{k}: {100*v/n:.1f}%" for k, v in tt))
print("\n== leaf category (self) ==")
for k, v in own.most_common(45): print(f"{100*v/n:6.2f}%  {k}")
print("\n== owner (generic leaves folded into nearest semantic frame) ==")
o = collections.Counter()
for (ow, lc), v in attributed.items(): o[ow] += v
for k, v in o.most_common(60): print(f"{100*v/n:6.2f}%  {k}")
print("\n== generic leaf by owner (top) ==")
for (ow, lc), v in attributed.most_common(400):
    if lc in generic and 100*v/n >= 0.25: print(f"{100*v/n:6.2f}%  {lc:14s} <- {ow}")
