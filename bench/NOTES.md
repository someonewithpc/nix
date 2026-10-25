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
- `nixos.nix` is the minimal NixOS system used for timing
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
- ASan+UBSan was clean before the goto parser. It has not been rerun on the
  goto parser yet

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

## Left to do

- Rerun ASan+UBSan on the goto parser
- O3+LTO numbers (`build.sh o3` in this worktree and in a master checkout)
- Time the goto parser against the recursive one on a quiet machine
- The io_uring loader with a huge-page buffer, if it is still wanted given
  the numbers above
- Build and test NEON under qemu-aarch64
