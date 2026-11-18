# Fast parser notes

Working notes for the `fast-parser` branch: what is done, how it is tested,
what was measured, and what is left. Not meant for upstream

## What the branch does

The flex lexer and Bison parser are replaced by a hand-written front end that
gives the same AST, doc comments and errors as Bison, byte for byte on the
test corpus

- `src/libexpr/lexer.cc`: a lexer that follows flex's rules (longest match,
  rule order, start conditions, `adjustLoc` and doc comment distances).
  Byte classification runs 64 bytes at a time through `lexer-simd.cc`, with
  scalar, SSE2, AVX2 and NEON backends picked at run time.
  `bench/simd-check` compares each backend against scalar
- Files of 2 MB and up are lexed in chunks on several threads. Each chunk
  after the first guesses that it starts in a default state. The merge
  checks each guess and lexes a chunk again when its guess was wrong
- `src/libexpr/parser.cc`: a state machine (`Parser::run`), one label per
  grammar rule, with frames on a vector instead of recursion. Nesting is
  only limited by memory, as with Bison
- Attribute sets of 128K tokens and more are parsed in pieces on several
  threads, and each piece is checked against where the serial parse got to.
  A piece that would print a warning gives up, so warnings print once and in
  order
- A thread-local cache sits in front of the symbol table, and each thread
  takes its own allocation chunks from the Expr bump allocator
- The parser takes a `ParseBuffer` (`parse-buffer.hh`): the text on a
  64-byte boundary with zero bytes to the end of its last block and for a
  block more, so the lexer reads whole blocks and looks ahead without end
  checks. Files are read straight into one, with io_uring where the
  kernel allows it: a per-thread ring opens, reads and closes each file
  as a linked chain on direct descriptors, so a batch of files costs one
  `io_uring_enter`. The accessor still stats each file, which keeps its
  allow lists and mtime tracking. Without io_uring, or for a failed or
  short read, the accessor's `readFile` reads the file. Buffers of 2 MB
  and up are mmapped with `MADV_HUGEPAGE`
- `EvalState` reads and parses imported files ahead of evaluation on two
  threads (`ImportPrefetcher` in `eval.cc`). Candidates are `import ./x.nix`
  calls and `.nix` path literals in lists. A file's paths are only queued
  once evaluation takes that file

## Knobs

| Variable | Default | Meaning |
|---|---|---|
| `NIX_LEXER_MIN_CHUNK` | 1 MiB | chunk size; files under twice this lex serially |
| `NIX_LEXER_THREADS` | 3/4 of cores | lexer chunks |
| `NIX_PARSER_MIN_PIECE` | 65536 tokens | sets under twice this parse serially |
| `NIX_PARSER_THREADS` | 3/4 of cores | pieces |
| `NIX_PREFETCH_IMPORTS` | 2 | prefetch threads, 0 turns it off |

Setting the sizes very low (chunk 64 bytes, piece 8 tokens) makes every file
go through the parallel paths. That is how the tests below exercise them

## How it is tested

Scripts in `bench/scripts`:

- `build.sh dev|asan|tsan|o3 [jobs]` builds this checkout into
  `build-<kind>` (`build` for dev) and installs into `outputs/<kind>`
  (`outputs/out` for dev). Set `NIX_DEVSHELL=native` on a host without
  ccache
- `build-bench.sh <prefix> <out> [flags]` builds `parse-bench` against an
  installed prefix
- `make-corpus.sh <dir> <list>` lists the `.nix` files of a source tree
- `check.sh <old-bench> <new-bench> <list>` compares dumps in eight
  configurations
- `nixos.nix` is the minimal NixOS system used for timing, and
  `eval-nixos.sh <lib-dir> [nix]` runs it. Pass nixpkgs as a string: as a
  path, the interpolation in `nixos.nix` copies it to the store, and
  hashing all of nixpkgs took 1.5 s of the 3.1 s every NixOS figure
  before 2026-10-25 included, for Bison and the branch alike
- `run-all.sh <base-tree> <list>` builds the branch and its base, checks
  the dumps, checks again under ASan and UBSan, and builds both with O3 and
  LTO

The corpus is a list of `.nix` files from store sources, one path per line.
The one used last had 5973 files (49 MB), including nixpkgs'
`hackage-packages.nix` (16.6 MB)

- `bench/parse-bench.cc` parses a file list and can dump every AST and doc
  comment (`--dump`) and parse on N threads (`--threads N`). Build it against
  an installed prefix: the old one gives the Bison baseline, the new one the
  branch. A changed `parseExprFromBuf` signature means the bench must be
  rebuilt, or it fails to load and a stale dump looks like a pass
- Compare `sha256sum` of the two dumps. A match is the pass condition
- `bench/fuzz.py <list> <out> <old-bench> <new-bench> [count] [seed]`
  mutates files and reports mutants whose AST or error differs.
  `bench/minimize.py` shrinks a difference
- Sanitizers: ASan+UBSan and TSan builds with `-Dlibexpr:gc=disabled`. Run
  the corpus with tiny chunk and piece sizes, serially and with
  `--threads 8`. For TSan, also run a nixpkgs `hello.drvPath` eval and a
  minimal NixOS toplevel eval with `NIX_PREFETCH_IMPORTS=4`. Run the
  sanitizer `nix` with `LD_LIBRARY_PATH` set to its own `lib`, since its
  rpath also finds the normal build

Results at the goto-parser commit (1fb393985) and the commit before it:

- Corpus: identical to Bison in every chunk and piece configuration and in
  thread mode
- Fuzzing: 50000 mutants, 0 differences (seeds 7 and 11)
- Deep nesting: a million nested parentheses, a 300000 term `++` chain,
  30000 nested lists and 100000 chained lambdas all give Bison's output
- TSan found one real race: speculative pieces read the parser's depth
  counter while the first piece changed it. It is fixed, and the counter is
  gone with the goto parser. TSan was then clean on the corpus and on both
  evals
- ASan+UBSan is clean on the goto parser (beryllium, all eight `check.sh`
  configurations, no reports)

## Measurements

helium (16 cores), -O2 debugoptimized builds, run back to back with the
performance profile. Absolute numbers move with the platform profile and
with other load, so only compare runs made together

| | Bison | branch |
|---|---|---|
| corpus, 1 thread, lex+parse | 1.30-1.37 s | 0.45 s |
| corpus, 12 threads, lex+parse+bindVars | 0.63 s | 0.21 s |
| hackage-packages.nix, lex+parse | 0.38 s | 0.10 s |
| nixpkgs `hello.drvPath` eval (hyperfine) | 251 ms | 217-221 ms |
| minimal NixOS toplevel eval | 3.74 s | 2.77-2.85 s |

The NixOS eval gives the same `.drv` with and without prefetching and with
Bison

Where the eval time goes:

- `hello.drvPath`: parsing is about 16% of the eval (lexing 4%, `bindVars`
  1.5%, file reads 1%)
- NixOS: parsing is 7.5%. Boehm GC marking dominates
- Prefetching saves nothing on `hello`, because most of the files it needs
  come from `callPackage` and are not literal imports. On NixOS it saves
  about 3% (2246 of the roughly 3400 files it needs come from the
  prefetcher), which is about the most there is to save. It costs 20-70 MB
  of memory
- File reading is about 1% of the NixOS eval and mostly happens on the
  prefetch threads now. An io_uring loader would save less than that, so it
  has not been written

### O3 and LTO on beryllium

beryllium (32 cores), `build.sh o3` of the branch at d765c1914 and of its
base 5533e016a, run back to back. The load average was around 17 from
other work, so treat small differences as noise

| | Bison | branch |
|---|---|---|
| corpus (5969 files), 1 thread, lex+parse | 0.86-0.88 s | 0.24 s |
| corpus, bindVars | 0.066 s | 0.067 s |
| corpus, 12 threads, lex+parse+bindVars | 0.45 s | 0.078 s |
| hackage-packages.nix, lex+parse | 0.26 s | 0.024-0.028 s |
| `hello.drvPath` eval (hyperfine) | 154 ms | 136 ms |
| minimal NixOS toplevel eval | 3.67 s, 671 MB | 3.08 s, 820-830 MB |

hackage-packages.nix against lexer and parser threads: 82 ms on 1, 37 ms
on 4, 28 ms on 8, 26 ms on 12, 24 ms on 24

The `nix` binaries that `build.sh` installs into `outputs/o3` and
`outputs/asan` have the devshell's `outputs/out/lib` in their rpath, so
they load the dev libraries unless `LD_LIBRARY_PATH` points at their own
`lib`. The parse-bench builds are not affected

Memory: each pool thread kept about 7 MB of caches and allocator heap,
which with 24 threads added 120 MB to the NixOS eval for no speedup. The
default is now at most 8 threads. What is left of the extra memory is
about 35 MB for prefetching, about 60 MB for 8 pool threads, and about 65
MB that is there even with 2 threads and no prefetching. That last part
has not been tracked down

### Tuning on beryllium

From a74d36456 to 9c82c80ed, one thread, the dev (-O2) build, corpus of
5969 files: lex+parse went from 0.320 s to 0.240 s and bindVars from
65 ms to 35 ms. Each step was timed with `ab.sh` against the libraries
before it

| Change | Gain |
|---|---|
| Symbol cache tail loads without a stack round trip | (in the 6% below) |
| Attribute sets and call arguments built in the Expr arena | bindVars 68 to 35 ms |
| Keyword lookup by packed word, `adjust` inlined | 5% |
| Whitespace lexed with the token after it | 1.7% |
| Token accessors inlined in the parser | 4% |
| Identifiers before `.` kept on the fast path | 3% |
| `{ } " = .` emitted directly | 2% |
| Comment ends found with the star mask | 1% |
| `stripIndentation` a run at a time | 3% |
| Symbol cache looked up once, Expr chunk TLS initial exec | 4% |
| Location offsets moved as one word | 1.5% |
| Direct returns for the commonest rules | 1% |
| String runs without escapes copied at once | 0.5% |

AVX-512 VBMI classifies a 64-byte block with one `vpermb` and eight
`vptestmb`, at 19 GB/s against 5.1 GB/s for AVX2. The escape finder
(`\`, `\r`, NUL) runs at about the same speed, ending with a masked load
instead of a scalar tail. Classification was only about 2% of a parse to
begin with, so most of the gains above are elsewhere

Tried and dropped, because they measured as noise:

- Threaded dispatch with computed gotos for the parser's returns. GCC
  merged the jumps again and branch misses did not move
- One 8-byte store of the location in `adjust` alone, before the stash
  was changed too
- Inlining `emit`, or the bump allocator into `Exprs::add`
- Skipping the cursed `or` checks gave nothing on its own, but is kept

About one branch miss per token remains (53M per five passes), almost all
of it the lexer choosing a rule and the parser choosing a continuation

LTO makes the parser slower. -O2 release without LTO matches the dev
build, while -O2 or -O3 with LTO is 3-4% slower on lex+parse. Under LTO
GCC stops inlining `emit`, `scan` and `sym` into the hot rules, and
forcing them inline did not win it back

### Memory on beryllium

NixOS minimal eval, dev builds, peak RSS: Bison 675 MB, branch 835 MB
with defaults, 709 MB with prefetching and pool threads off. Peak RSS
is stable to a few MB between runs, so these differences are real

- With everything off the branch is 34 MB over Bison. The token buffer
  of hackage-packages.nix, live while it is parsed (33 MB now that the
  lexer reserves one token per 8 bytes), and the symbol cache (8 MB for
  96k symbols) account for that
- The token pool used to keep that buffer for the rest of the process
  (63 MB, reserved at one token per 4 bytes). It now keeps buffers up to
  4 MB
- Lexer chunks add 23 MB and parser pieces 69 MB. Nearly all of that is
  mimalloc: each new thread heap commits and touches arena memory
  eagerly. With `MIMALLOC_ARENA_EAGER_COMMIT=0` the branch is at 724 MB
  and Bison at 652 MB, for 1-2% more eval time. parse-bench, which does
  not link mimalloc, shows only 6 MB for eight parser threads
- All pieces were accepted in both hackage and the NixOS eval, so none
  of the AST is built twice

### 2026-10-25 on beryllium

The NixOS benchmark without the nixpkgs hashing (see `eval-nixos.sh`),
dev builds, hyperfine: Bison 2.24 s and 652 MB, the branch 1.63 s and
792 MB, 27% faster. `hello.drvPath`: 160 ms against 138 ms

Where a corpus pass goes, one thread: 5.77M tokens in 0.205 s of lex and
parse, about 190 cycles a token at 5.4 GHz, and 1.7 branch misses a
token. The lexer is about a third, the parser's state machine a third,
and building the AST (symbols, the attribute `std::map`, arena
allocations, string copies) the rest

Mallocs per pass over the corpus (the second pass of two minus a single
pass), counted with an LD_PRELOAD shim that records callers with
`backtrace`: 628k before, 86k after these. The rest is mostly per file
(origins, `SourcePath` copies) and `ExprAttrs` inherit vectors

| Change | Effect |
|---|---|
| Parser stacks kept per thread across parses | 170k mallocs fewer |
| `stripIndentation` buffers kept per thread | 97k fewer |
| Path literals canonicalised in one buffer | 91k fewer |
| `bindVars` environments from a per-thread stack, insertion sort | 108k fewer, bindVars 6% faster |
| All of the above | lex+parse 3.5% faster |
| String bodies lexed with the `strStop`/`indStop` masks | 1.7% |
| Common tokens inlined into the lexer loop, the rest out of line | 3.2% |
| Default-state tokens in an inner loop on a local position | 1.8% |
| One walk down the attribute map per binding | 0.85% |
| `E` copied field by field (16-byte load stalled on store forwarding) | 0.6%, Parser::run 22% to 18% of samples |
| io_uring and padded buffers | within noise on time, 18 MB less memory on NixOS |

Tried and dropped: inlining `Parser::sym` (0.9% slower; the bigger
`run` costs more than the call)

`bindVars` recursion limits on a 16 MB stack, which the environment
stack must not lower: 300000 nested lambdas, 280000 `let`s, 390000
`with`s. Keeping its work out of line keeps the frames as small as
they were; with it inline the lambda frame grew from 0x98 to 0xc8 bytes
and the limit fell to 245000

Prefetching costs memory out of proportion to what it saves: `hello`
peaks at 123 MB without it, 161 MB with one thread and 175 MB with two,
for no gain in time; NixOS is 1.70 s and 772 MB without it, 1.60 s and
810 MB with two threads. Most of it is not mimalloc's eager commit
(with that off, `hello` with two threads is 121 MB), so something the
prefetch threads keep is worth finding

### 2026-10-26 on beryllium: attribute sets, keywords, symbols

The machine was on the power-saver profile, so absolute times are about 20%
above the ones of the day before. Only compare A/B runs made together

How the parser fills attribute sets, recorded from the corpus: 657k
lookups into 143k sets, 641k of them inserts. 98.6% of the sets have 16
names or fewer, and the biggest has 19437. Symbol ids follow interning
order, not source order, so 39% of the inserts land before the end of the
set. Kept sorted on every insert, a vector shifts 48M entries per corpus
pass

The candidates, replaying that trace (build every set the way `addAttr`
does, iterate each once as bindVars does, look each name up once as eval
does) over pre-faulted arena memory, best of 7, from
`bench/map-bench.cc`:

| map | build ms | iterate ms | find ms | total ms | MB requested |
|---|---|---|---|---|---|
| `std::pmr::map` (before) | 11.99 | 3.01 | 7.04 | 22.04 | 41.0 |
| `boost::container::flat_map` | 42.00 | 1.06 | 7.08 | 50.14 | 48.0 |
| same, 4 entries inline (`small_vector`) | 40.12 | 1.05 | 6.85 | 48.01 | 29.9 |
| `std::flat_map` (C++23, keys in their own array) | 23.34 | 0.96 | 7.01 | 31.31 | 42.0 |
| `absl::btree_map` 20260817 | 16.57 | 2.33 | 6.73 | 25.63 | 51.0 |
| `seq::flat_map` (tiered vector) | 15.52 | 2.11 | 8.88 | 26.51 | 59.3 |
| `seq::radix_map` | 15.52 | 2.78 | 4.56 | 22.86 | 70.9 |
| plain vector, linear lookups | 115.53 | 1.37 | 92.32 | 209.22 | 51.7 |
| vector, appended to and sorted when the set ends | 11.95 | 0.91 | 6.92 | 19.77 | 48.0 |
| same, keys in their own array | 16.49 | 0.91 | 6.64 | 24.04 | 57.3 |

The replay alone costs 1.4 ms. A drop-in `boost::container::flat_map` in
the real parser made lex+parse 10% slower and bindVars 8% faster

`ExprAttrs::AttrDefs` is now the appended-then-sorted vector, with the part
of the `std::map` interface the evaluator uses. While the parser builds a
set, a lookup is a binary search of the part in order and a scan of what
was appended since. Past √n appended entries a big set merges them in.
Each set is sorted when its bindings end, while it is still in the cache.
Sorting them all at the end of the parse instead cost 2% of lex+parse in
cache misses

| | before | after |
|---|---|---|
| corpus lex+parse | 0.250 s | 0.252 s |
| corpus bindVars | 0.037 s | 0.034 s |
| NixOS eval (hyperfine, 8 runs) | 1.965 s, 788 MiB | 1.936 s, 792 MiB |
| `hello.drvPath` eval (30 runs) | 168.1 ms, 170 MiB | 166.5 ms, 170 MiB |

NixOS gives the same `.drv` with Bison, the `std::map` build and this one

Keywords now go through a perfect hash. The identifier's bytes are loaded
as one word from the padded text and masked to its length. Multiplied by
0xcd613e30d8f16adf, the top 4 bits give each of the 10 keywords its own
slot of 16, and one compare settles it. That made lex+parse 1.2% faster

The symbol cache hashed names of up to 16 bytes with up to three
multiplies in a chain. Now each word has its own multiply and the index is
the top bits: 0.6% faster. CRC32 would be one instruction, but the build
targets baseline x86-64, which has no SSE4.2

Copying `AttrDef` a field at a time on append, against a store-forwarding
stall that perf shows there, gained nothing and was left out

Under ASan, attribute sets that fail to parse leak the `inheritFromExprs`
vectors of their `ExprAttrs`. The arena never destroys what it holds, and
a failed parse leaves nothing that reaches them. Bison's parser does the
same. ASan and UBSan found nothing else in the corpus check, 6000
attribute sets from `bench/gen-attrs.py` or 3000 fuzzed files

### 2026-11-18 on beryllium: symbol order (F0)

Symbol ids follow interning order and attribute sets are sorted by id, so
the pieces of big sets and the import prefetcher, which interned names on
their own threads, changed deepSeq, `==` and trace order from run to run.
Both now intern the names the table lacks into a `LocalSymbols`, with ids
that have the top bit set. The serial parse interns an accepted piece's
names when it reaches the piece, and `ImportPrefetcher::take` interns a
file's names before bindVars, where upstream parses it. Then
`Expr::remapSymbols` puts in the table's ids and sorts sets and formals
again. The order is Bison's by construction

- `parse-bench --dump` now ends with the symbol table in id order, and
  `check.sh` compares it on serial runs. Serial with one thread was already
  equal to Bison on the corpus (5991 files present here). Before the fix,
  the small-piece configuration differed in 120k symbol lines. After it,
  all eight `check.sh` configurations match, and so do eight more runs
  with chunk 64 and piece 8 on 8 threads
- Reproducers in `tests/functional/lang`: `eval-okay-symbol-order-pieces`
  (64 names in a set big enough to parse in pieces) and
  `eval-okay-symbol-order-imports` (the a.nix/b.nix case). Before: 3 of 6
  and 1 of 6 runs wrong. After: 200 runs each, none differ. A generated
  case of 200 files sharing new names, forced in shuffled order
  (`gen-prefetch.py` in the scratch dir), differed on every run before;
  after, 100 runs each of three seeds with `NIX_PREFETCH_IMPORTS=8` match
  base
- T0 quick suites (lang, hello, nixos) with `-r 5`: 0 of 500 differ. T0's
  lang suite reads its own checkout's tests, so it doesn't run the new ones
- TSan: no reports on the corpus with chunk 64 and piece 8, serial and
  `--threads 8`, on hello and minimal NixOS with `NIX_PREFETCH_IMPORTS=4`,
  and on the reproducers
- `tests/functional/lang` and `nix-expr-tests` pass

Instructions (`perf stat -e instructions:u`, three interleaved runs, all
threads, before is `fast-eval`):

| workload | before | after |
|---|---|---|
| `hello.drvPath` | 1.123-1.125 G | 1.129 G (+0.5%) |
| minimal NixOS | 19.96-20.02 G | 20.07-20.16 G (+0.6%) |
| real system, `--read-only`, rev 19709e05 | 71.06-71.25 G | 71.13-71.27 G |
| corpus parse-bench, serial | 2.823 G | 2.905 G (+2.9%) |
| hackage-packages.nix, 3 parses | 2.88 G | 3.03 G (+5%) |

With `NIX_PREFETCH_IMPORTS=0`, hello is 945 M both ways, so the cost is
the remap. On hello, sampling puts the main thread's share of it
(`remapSymbols`, `internLocalSymbols`) at about 0.5%. Big sets keep most
of their speedup: hackage-packages.nix lex+parse is 33.5 ms before, 36 ms
after, 64 ms serial. A piece of a parse that interns into the table may
use any symbol it finds there, since that is the id the parse would use;
counting only the static symbols as known made it 63 ms

### 2026-11-18 on beryllium: derivation text once (B1), batched writes (A4)

B1 renders each derivation's ATerm once and writes structured attrs as
JSON text without an nlohmann tree. Instructions (`perf stat -e
instructions:u`, `--read-only`, median of 3 interleaved runs, `fast-eval`
against the branch):

| Workload | fast-eval | B1 | Change |
|---|---|---|---|
| `hello.drvPath` | 1.116 G | 1.005 G | -9.9% |
| minimal NixOS (`nixos.nix`) | 20.02 G | 18.06 G | -9.8% |
| real system toplevel | 71.71 G | 59.56 G | -16.9% |

Checks for B1:

- T0 `eval-diff.sh -s all` against the base build: 0 of 1507 cases differ
- The `.drv` closures of `nixos.nix` (3656 files) and of the real system
  (17944 files), written by base and by B1 into two fresh chroot stores
  (`--store <dir>`): every file byte-identical
- `JSONTextWriter` against `dump()`: 50000 random trees, every one- and
  two-byte string, floats, int64 limits. Invalid UTF-8 throws the same
  error at the same point, in `builtins.toJSON` and in `derivationStrict`

A4 queues the derivation writes of a daemon client and sends them in
batches: the temp roots in one `AddTempRoots` (when the daemon has it),
the validity checks as pipelined `IsValidPath` in chunks of 256, then the
missing derivations in order. `QueryValidPaths` was tried first, but a
daemon serving a builder refuses it (`dep-built-drv-submitted`). The queue
is flushed before an operation on a queued path, before any operation that
could read one without naming it, before a direct file system read of a
queued path, at 4096 entries or 32 MB, at the end of `StoreCommand::run`
and `nix-instantiate`, and in `~EvalState`

Daemon round trips for the real system in write mode, counted with strace
against the system's 2.35.6 daemon: 39,877 with B1, 4,000 with A4. What
is left is source copies (2374 `AddToStore`), 1,367 `IsValidPath` (about
140 of them the chunks above) and 219 `QueryPathInfo`. Getting under 200
needs the source copies batched too, which A4 does not cover

Checks for A4: T0 `-s all` as above, a minimal NixOS with a fresh zlib
overlay written through the daemon (same drv path as base, closure of
4171 paths valid), and probes for IFD, `readFile` of a fresh drv with and
without context, `builtins.storePath` and `nix-instantiate` output that
exists after exit

### 2026-11-18 on beryllium: eval cache for target 1 (A1, A3)

`nix eval /etc/nixos#nixosConfigurations.beryllium.config.system.build.toplevel.drvPath`
now answers from the eval cache when nothing changed. Before, it evaluated
in full on every run (9-17 s): `nix eval` never read the cache, and a dirty
tree with a submodule had no fingerprint

| Build | Hit, instructions | Hit, wall |
|---|---|---|
| A1 (`fe-a1-evalcache~4`) | 273 M | |
| A1 and A3 | 157 M | 28.5 ms on `/etc/nixos`, 31.2 ms on a copy |
| `nix eval --expr 1`, same build | 81 M | 18.4 ms |

Wall times are hyperfine, 30 runs, at load 5. Instructions are
`perf stat -e instructions:u -r 10`. A3 took out `git status` (49 M) and
the libgit2 and OpenSSL init (46 M), which a ref name check in the lock
file's GitHub inputs pulled in

Where the 157 M go: ld.so 45% (A2's), lockFlake 18% (lock file JSON 5%),
the import prefetcher's two threads parsing what flake.nix imports 8%
(wasted on a hit), the workdir record 5%, the eval cache's SQLite 4%

How it was checked:

- Hits print what `--no-eval-cache` prints on stdout. On stderr they drop
  the evaluation's traces and warnings, as upstream's cache does. A cached
  failure evaluates again and prints the same error as the base build
- Edits on a copy of `/etc/nixos` (`edit-tests.sh` in the agent's scratch
  directory): appending to a module, a same-size edit of a module, a
  same-size edit of flake.lock and an edit in `secrets` with
  `?submodules=1` are misses, and the next answer equals a fresh eval.
  Without `submodules=1` the submodule is an empty directory to the flake,
  so an edit there is a hit and still equals a fresh eval
- `tests/functional/flakes/eval-cache.sh` gained cases for strings, paths,
  big integers, Booleans, a deleted `.drv`, dirty edits with the same size
  and mtime, `touch`, `git stash`, branch switches and submodule updates
- Suites flakes, git and git-hashing, lang, the fetchGit tests and the
  fetchers, expr and flake unit tests: 52 passed, 0 failed, lang-gc
  skipped. T0 quick suites against base: 0 of 500 differ

### 2026-11-18 on beryllium: build path (A6)

Full survey in `bench/BUILD-PATH.md`, probes in `bench/scripts/a6/`. The
97 M instructions per trivial local build were the host's cachix
post-build hook: Nix itself spends about 2.6 M user instructions and
10 ms of kernel time per build on a chroot store, the hook about 87 M.
A spooling hook for `/etc/nixos` is proposed in
`bench/a6-proposals/cachix-spool.patch`, not applied

- Substituters are asked for a path at once: `nix build --dry-run` with
  5 misses over 4 substituters went from 1165-1358 ms to 622-806 ms
- The post-build hook gets its environment through `execvpe`: 244.2 M to
  231.6 M instructions for 13 builds with an `exit 0` hook
- `loadDerivation` reads from the store it found the drv in: 384.4 M to
  373.5 M for a build with 800 built inputs

Tests: `meson test --suite main --suite ca --suite libstore-tests`,
152 passed, 4 failed, 6 skipped. The 3 HTTPS store tests in
`nix-store-tests` fail on the session's `HTTPS_PROXY` and pass without
it. `build-remote-trustless-should-fail-0`, `-should-pass-2` and
`-should-pass-3` fail on "File name too long" for a lock file named
after this worktree's path, before any of the changed code runs

### 2026-11-18 on beryllium: start-up (A2)

`static-nix-libraries` links the Nix libraries into `nix` from static
archives, which the libraries build next to the shared ones under
`default_library=both`. The packaging turns both on for Linux. C++
plugins that link the shared libraries then fail with "Already registered
store with name 'Dummy Store'". C API plugins still load

| Build | `--version` | `--expr 1` | cache hit | `--version` wall | hit wall |
|---|---|---|---|---|---|
| A1 (`fe-a1-evalcache`) | 79.9 M | 81.2 M | 231.9 M | 15.2 ms | 34.1 ms |
| A2, shared | 70.7 M | 71.9 M | 228.8 M | 13.4 ms | 34.0 ms |
| A2, static | 42.8 M | 44.0 M | 200.6 M | 11.0 ms | 30.8 ms |

The cache hit is `nix eval` of the system's drvPath on a clean
`/etc/nixos`. Instructions are `perf stat -e instructions:u -r 20`, wall
times hyperfine with 60 and 40 runs at load 7. These are dev builds,
whose libraries lack the `-Bsymbolic-functions` the packaging adds. The
packaged `nix-cli` (release, LTO) gains less:

| `nix-cli` package | `--version` | `--expr 1` | cache hit | `--version` wall | hit wall |
|---|---|---|---|---|---|
| `withStaticNixLibraries = false` | 53.8 M | 55.0 M | 208.6 M | 12.8 ms | 31.9 ms |
| static | 41.5 M | 42.7 M | 198.2 M | 10.9 ms | 29.5 ms |

The packaged static nix has the same T0 results (0 of 500 differ). Its
link needs the whole-archive dependencies in dependency order: with
`nix-fetchers-whole-archive` after `nix-expr-whole-archive`, ld.bfd
marked `libnixfetchers.so` as needed, both copies ran their static
initialisers, and `nix --version` crashed on exit

- Static linking cut the symbol relocations ld.so does at start from
  37.3k to 17.6k. ld.so is still 70% of `--version`. The rest bind the
  system libraries: the aws-c stack for S3 4.3k, libstdc++ 2.7k, curl
  and what it pulls in 2.3k, krb5 1.3k, libxml2 0.9k
- The earlier `-Bsymbolic-functions` trial (37,251 relocations) measured
  the wrong libraries: the dev shell puts `outputs/out/lib` first in the
  RUNPATH. With its own libraries it bound 25.3k
- GC_init read `/proc/self/maps` through pthread_getattr_np(), about 2 M.
  Setting flags built their descriptions with boost::format, about 0.5 M.
  Packed relative relocations saved 0.8 M
- On a clean `/etc/nixos` the hit opens the repo with libgit2
  (`getAccessorFromWorkdir`), and libgit2's OpenSSL init loads the CA
  bundle: 23% of the hit's samples. A3 measured 157 M on a dirty tree
- Not done: `computeLevels()` (libcpuid, 1.1 M) runs for the default of
  `extra-platforms`; a cheaper CPU check could compute other levels on
  odd CPUs. Making the setting flags lazy would save most of the
  remaining 2 M in `NixArgs`

Tests: lang, the util, store, fetchers, expr and flake unit tests and the
plugins suite pass on the shared build. On the static build the full
`meson test` gives 297 passed, 3 failed, 8 skipped. The 3 are
`build-remote-trustless-should-{pass-2,pass-3,fail-0}`, which fail the
same way on the shared build ("File name too long" for a lock file
path). T0 quick suites against base: 0 of 500 differ. The system
drvPath is the same as base's with `--no-eval-cache`

## Left to do

- Decide on mimalloc eager commit, which is most of the memory the
  pool threads cost
- Time the goto parser against the recursive one on a quiet machine
- Find what the prefetch threads keep: 38 MB for the first one on `hello`
- Keep the lexer's location and output pointer in registers across the
  inner loop; `adjust` and `emit` load and store them every token
- Shrink the parser's symbol cache or its entries: the probe misses
  cache on 37% of its samples once the table outgrows L2
- Build and test NEON under qemu-aarch64
- Decide on the cachix spool hook (`bench/a6-proposals/cachix-spool.patch`)
- Kernel time per sandboxed build (10 ms) and per input path (0.07-0.13
  ms) need kernel profiles, which `perf_event_paranoid` 2 rules out
