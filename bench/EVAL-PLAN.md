# Evaluator speed-up plan

Survey of the evaluator on the `fast-eval` branch (base `fast-parser`, Nix
2.36.0 master), made on beryllium on 2026-11-18. It holds the measurements,
a ranked list of work items, the batches they can run in, and a test plan.
No evaluator code was changed

The owner's targets come first:

1. A do-nothing eval of the real system takes about 50 ms:
   `nix eval /etc/nixos#nixosConfigurations.beryllium.config.system.build.toplevel.drvPath`
2. A `nixos-rebuild switch` that adds one cached package costs about 100 ms
   of Nix's time

Where they stand today, and what I think is reachable:

| | today | reachable | by |
|---|---|---|---|
| Target 1, unchanged config | 13-17 s wall | 20-50 ms | A1, A2, A3. The existing eval cache answers in 215 M instructions (50 ms CPU) when it applies, and here it does not apply |
| Target 2, eval after an edit | 13-17 s wall, 9-10 s CPU | 5-6 s with the low-risk items, 4-4.5 s with the rewrites | A4, B items, C1-C3 |
| Target 2, eval after an edit | same | 50-200 ms, unproven | A5, incremental evaluation, a research track |
| Target 2, the rest of the switch | 2.1-2.8 s second eval, 50-80 ms per local build, 0.85 s substituter lookups per substitutable path | not surveyed | A6 |

A faster interpreter does not reach either target. The full eval can get
about 2.2 times faster without parallelism. Target 1 needs a cache that
answers without evaluating. Target 2 needs evaluation that redoes only what
the edit changed. Adding `pkgs.hello` to the system changes 15 of 17,929
derivations

## Measurements

The machine was busy with other sessions the whole time (load average 40 to
107 on 32 threads, balanced power profile). Wall times are ranges and are
worse than on a quiet machine. Instruction counts (`perf stat -e
instructions:u`) and profile percentages do not move with load, so compare
those

Setup used below:

```sh
W=~/projects/worktrees/nix/fast-eval
NIX=$W/outputs/out/bin/nix            # bench/scripts/build.sh dev 24
O3=$W/outputs/o3                      # bench/scripts/build.sh o3 16, run with LD_LIBRARY_PATH=$O3/lib
NP=/nix/store/mf1n715j8c2pvz6cwzajhhfbmjqanmi7-source
PERF=/nix/store/hln3gq5s0pajpqlwcffqadzxglqfhx29-perf-linux-7.3.4/bin/perf
HF=/nix/store/kzg6alm5cpb735v0xxchyihhbfbg7ffh-hyperfine-2.0.0/bin/hyperfine
SYS='/etc/nixos#nixosConfigurations.beryllium.config.system.build.toplevel.drvPath'
XF='--extra-experimental-features nix-command --extra-experimental-features flakes'
```

The perf and hyperfine paths in the task brief do not exist on beryllium.
`perf_event_paranoid` is 2, so recording needs `-e cycles:u
--user-callchains -g`, and `-m 32` when other perf sessions hold the mlock
budget. `/etc/nixos` was only read. Its tree is dirty and another session
edited it during the survey, so the drv hash moved between runs

### Target 1 baseline

| Command | Wall | CPU | Notes |
|---|---|---|---|
| `nix eval --no-write-lock-file $SYS`, system nix 2.35.6 | 15.2-17.3 s (first run 35.8 s) | 12.4-12.6 s user, 1.6-1.8 s sys | 1.38 GB peak |
| same, branch dev build | 13.1-15.4 s | 11.0-12.2 s user | 72.2 G instructions |
| same with `--read-only`, branch dev | 8.9-9.5 s | 10.1-10.5 s user | 71.9 G instructions |
| same with `--read-only`, branch o3 | | | 69.7 G instructions |
| same with `--read-only`, system nix | 8.8-9.0 s | 9.7-9.9 s user | 69.6 G instructions |
| `nix eval 'git+file:///etc/nixos?rev=<HEAD>#…drvPath'`, clean tree, repeated | 11.0-11.5 s | 9.6-9.8 s user | no cache hit on any run |
| `nix build --no-link --dry-run 'git+file:///etc/nixos?rev=<HEAD>#…toplevel'`, second run on | 80-98 ms | 50-55 ms | 215 M instructions, eval cache hit |
| `nix path-info --derivation` of the same | 63-65 ms | 51 ms | 212 M instructions |
| `nix flake metadata /etc/nixos` | 48-68 ms | | 269 M instructions (dev) |
| `nix eval $XF --read-only --expr 1` | 19-23 ms | | 81 M (dev), 69 M (o3), 68 M (system) |
| `nix --version` | 18-23 ms | | 80 M (dev) |

The branch's parser does not show on this workload. The o3 branch and system
nix (Bison) are within 0.1% of each other in instructions. Parsing is 4.9%
of the CPU here, most of it already on the prefetch threads

Why the no-op eval takes 13-17 s:

- Nothing is cached. `nix eval` goes through `InstallableFlake::toValue`,
  which calls `getCursor()->forceValue()` and never reads the eval cache
  (`src/libcmd/installable-flake.cc:144`). `nix build` and `nix path-info`
  read it
- For `nix build` the cache is off too on this tree. `/etc/nixos` is dirty
  and has a submodule (`secrets`), and `GitInputScheme::getFingerprint`
  returns nothing for a dirty tree with submodules
  (`src/libfetchers/git.cc:1146`, the `submodules.empty()` test). `nix build --dry-run /etc/nixos#…toplevel`
  evaluated in full on each of three runs (12.4-14.2 s user, 22-45 s wall
  with substituter queries for the unbuilt paths)
- 4-5 s of the wall time is the client waiting for the daemon.
  `Store::writeDerivation` does `addTempRoot` and `isValidPath` per
  derivation (`src/libstore/derivations.cc:118`). `strace -f -c` on the
  eval shows 44,825 `write` and 40,188 `read` calls for 17,929 derivations,
  two round trips each. `--read-only` removes them, which is the 13-15 s
  against 9 s above
- The rest is evaluation: 9-10 s of CPU on the main thread plus GC marker
  threads
- Fetching and locking is small next to that and large next to 50 ms. Up to
  the flake's outputs (`nix eval /etc/nixos#nixosConfigurations --apply
  builtins.attrNames`) costs 0.95 G instructions and 206 ms of CPU, with
  1508 `openat2` and 1827 `getdents64` calls to walk and hash the dirty
  tree. The lock file has 93 inputs
- Process start is 69 M instructions in the o3 build and 67% of them are in
  `ld.so` (`do_lookup_x` 37%). The binary loads 77 shared libraries and
  binds them at start. `LD_DEBUG=statistics` reports 37,256 relocations for
  the dev build

Where the evaluation goes, by attribute (dev build, `--read-only`,
instructions):

| Attribute of `nixosConfigurations.beryllium` | Instructions | Share |
|---|---|---|
| `config.networking.hostName` | 3.43 G | 4.8% |
| `pkgs.hello.drvPath` | 4.08 G | 5.7% |
| `config.system.path.drvPath` | 35.2 G | 49% |
| `config.system.build.etc.drvPath` | 70.6 G | 98% |
| `config.system.build.toplevel.drvPath` | 71.9 G | 100% |
| same with `GC_DONT_GC=1` | 62.2 G | 86.6% |

By part of nixpkgs, from Nix's own profiler (`--eval-profiler flamegraph
--eval-profiler-frequency 999`, 6689 samples, bucketed with
`bench/scripts/nixprof-domains.py`). It samples at function calls, so treat
it as a rough split:

| Innermost domain | Share |
|---|---|
| `mkDerivation` and `check-meta` (includes `derivationStrict`) | 39.6% |
| package expressions under `pkgs/` | 34.0% |
| module system (`lib/modules.nix`, `types.nix`, `options.nix`) | 18.0% |
| user flake in `/etc/nixos` (3.7% is one package's `importCargoLock`) | 4.1% |
| NixOS module bodies | 2.3% |
| flake glue, home-manager | 2.0% |

So the module system is about a fifth of the eval and packages are three
quarters. Reusing the package set alone would leave about 2.3 s

Counters for the same eval (`NIX_SHOW_STATS=1`, system nix): 8.08 M
function calls, 4.16 M primop calls, 14.0 M thunks, 13.4 M thunks avoided,
5.23 M attribute selects, 1.72 M `//` copying 21.6 M values, 0.65 M list
concatenations, 22.4 M values (358 MB), 9.42 M envs (212 MB), 3.78 M
attribute sets with 39.7 M elements (726 MB), 138,666 symbols, 1.54 GB
allocated, 5 GC cycles, 17,929 derivations of which 12,604 use
`__structuredAttrs`, 71.8 MB of `.drv` text

### Target 2 baseline

What `nixos-rebuild switch --flake` runs (nixos-rebuild-ng,
`pkgs/by-name/ni/nixos-rebuild-ng/src/nixos_rebuild/`), and what each step
costs now:

| Step | Measured | Notes |
|---|---|---|
| `nixos-rebuild` start | 181-277 ms for `--help` | Python, twice because of the re-exec |
| `nix build …config.system.build.nixos-rebuild` for the re-exec | 2.1-2.8 s wall, 1.2-1.3 s CPU | a second, partial eval. `--no-reexec` skips it |
| `nix build …toplevel`, eval | 13-17 s wall as a user | as root the store is local and the 36k round trips become SQLite queries, not measured |
| realise, when the path is built | 51-131 ms for `nix build --no-link <drv>^out` | |
| realise, per rebuilt local derivation | 90-131 ms end to end for a trivial `preferLocalBuild` derivation, 39-49 ms of it eval and start | 50-80 ms of sandbox and registration per build |
| realise, per substitutable path that is in no cache | 935-1009 ms for a trivial derivation | 0.85 s of narinfo lookups on three substituters. 169-188 ms with `--option substituters ''` |
| set the profile | 50-114 ms for `nix build --profile <scratch>` | `nix-env --set` not run |
| `switch-to-configuration switch` | not measured | outside Nix, needs root |

The edit itself, measured with `extendModules` in one process:

```sh
nix eval --no-write-lock-file --json '/etc/nixos#nixosConfigurations.beryllium' --apply 'c: {
  a = c.config.system.build.toplevel.drvPath;
  b = (c.extendModules { modules = [ ({ pkgs, ... }: { environment.systemPackages = [ pkgs.hello ]; }) ]; }).config.system.build.toplevel.drvPath; }'
```

`nix-store -qR` on both gives 15 derivations only in `b`: `hello`, its
tarball, `system-path`, `dbus-1`, `etc`, six unit files, two restart
triggers, `activate` and the toplevel. At least 13 local builds follow an
edit like this, on a chain about five deep, so the build path alone is
several hundred ms of Nix overhead before the builders' own run time

### Profiles

Commands, dev build with frame pointers:

```sh
# real system, 30,129 samples
$PERF record -e cycles:u --user-callchains -g -F 1999 -o sys.data -- $NIX $XF eval --read-only --no-write-lock-file "$SYS"
# minimal NixOS (bench/scripts/nixos.nix), 13,042 samples, 20.0 G instructions
$PERF record -m 64 -e cycles:u --user-callchains -g -F 2999 -o min.data -- $NIX $XF eval --read-only --impure --raw --expr "import ./nixos.nix { nixpkgs = \"$NP\"; }"
# hello, 15 runs, 44,901 samples, 1.12 G instructions a run
NIX_PATH=nixpkgs=$NP $PERF record -m 32 -e cycles:u --user-callchains -g -F 9000 -o hello.data -- bash -c "for i in \$(seq 15); do $NIX $XF eval --read-only --impure --raw --expr '(import <nixpkgs> {}).hello.drvPath' >/dev/null; done"
$PERF script --no-inline -i sys.data -F comm,tid,ip,sym | python bench/scripts/profile-classify.py
```

Share of samples by what the work was for. Generic leaves (malloc, memcpy,
GC allocation, forcing, symbol table, string contexts) are folded into the
nearest frame that names the work:

| Category | real, main thread | real, all threads | minimal NixOS | hello |
|---|---|---|---|---|
| JSON for structured attrs (nlohmann) | 17.3% | 13.5% | 7.0% | 5.7% |
| derivation unparse and hashing | 12.1% | 9.5% | 6.0% | 7.6% |
| `derivationStrict` itself | 4.8% | 3.8% | 2.7% | 3.1% |
| path access (`isAllowed`, `CanonPath`, `lstat`, store copies) | 9.7% | 8.4% | 4.8% | 15.4% |
| `callFunction`, `ExprCall` | 8.9% | 6.9% | 12.5% | 5.9% |
| other `Expr::eval`, `evalBool`, forcing | 6.8% | 5.4% | 6.0% | 4.6% |
| `ExprAttrs`, `ExprLet`, `ExprList`, `ExprWith` | 5.1% | 4.2% | 5.1% | 4.5% |
| `Bindings` iterator, `lexicographicOrder` | 5.0% | 3.9% | 3.0% | 4.8% |
| `ExprVar` | 3.8% | 3.0% | 4.3% | 1.7% |
| `Bindings::get` | 3.4% | 2.7% | 1.9% | 1.7% |
| `ExprSelect`, `ExprOpHasAttr` | 2.3% | 1.8% | 3.1% | 1.0% |
| `ExprConcatStrings`, `//`, `++`, `==` | 4.3% | 3.3% | 3.7% | 4.1% |
| GC mark and sweep | 4.2% | 20.7% | 19.1% | 0.8% |
| parse, lex, prefetch | 2.4% | 4.9% | 10.5% | 16.4% |
| `fromTOML` | 2.1% | 1.6% | 0.3% | |
| `readDir` | | 0.1% | 0.3% | 4.2% (12% with its path and symbol work) |
| regex | 1.0% | 0.8% | 0.7% | 0.5% |
| start-up, exit, not under a named frame | 1.9% | 1.6% | 3.0% | 14.2% |

The main thread is 78% of the real system's samples. GC marker threads are
18.6% and run on idle cores, so marking costs CPU more than wall time. The
stats report 0.33 s of GC in a 9.9 s eval

Self time of the generic leaves on the real system's main thread: malloc
and free 6.9% (half under `derivationStrict`), forcing and type tests
6.6%, memcpy and memcmp 6.2%, string contexts 5.6%, GC allocation 5.2%,
symbol table 3.3%, PLT and `__tls_get_addr` 2.6%

The o3 build has the same shape, so the dev profile is a fair guide. Top
self-time symbols with o3 on the real system (28,806 samples): `GC_mark_from`
7.5%, `GC_push_contents_hdr` 5.5%, `operator new[]` 4.5%, `callFunction`
3.6%, `ExprVar::eval` 3.4%, nlohmann `serializer::dump` 2.7%,
`GC_header_cache_miss` 2.4%, `CanonPath::isAllowed` 2.2%, memmove 2.0%,
memcmp 1.9%, `ExprSelect::eval` 1.7%, `GC_add_to_black_list_normal` 1.4%,
`ExprAttrs::eval` 1.4%, `Bindings::get` 1.4%, symbol table lookup 1.4%,
SHA-256 1.3%. One dev artifact: `getInternalType` is out of line in the dev
build (1.8% plus PLT) and inlined in o3

## What the measurements say

- Under half of the main thread is the interpreter. Building derivations
  (JSON, unparse, hashing, `derivationStrict`) is 34% of it and path checks
  are 10%. The interpreter proper (calls, variables, selects, attribute
  sets, operators, primops) is about 45%. A bytecode VM or a JIT speeds up
  only that part, and most of that part is allocation and attribute set
  work, not dispatch
- 70% of the derivations use structured attrs. Each one is turned into an
  nlohmann tree and unparsed three times: for the masked hash
  (`fillInOutputPaths`), for the store path (`infoForDerivation`), and for
  the hash kept for dependents (`derivation::masked::hashInput` at the end
  of `derivationStrictInternal`)
- The existing eval cache is 50 ms away from target 1 and misses it for two
  small reasons, listed above
- Attribute sets are 47% of the bytes allocated. Values are 23%, envs 14%

### The branch already differs from upstream in evaluation order

Attribute sets are sorted by symbol id, ids follow interning order, and
`deepSeq`, `==`, `forceValueDeep` and the "unexpected argument" error walk
sets in that order. The parallel parser and the import prefetcher intern
symbols in a different order than upstream's lazy, serial parse

```sh
# 60,000 shuffled attributes, each `kNNNNN = builtins.trace "kNNNNN" 1;`, under deepSeq (2.3 MB)
nix eval --read-only --impure --file big.nix 2>&1 | sha256sum
```

System nix prints the same trace order on each run, and so does the branch
with `NIX_PARSER_THREADS=1 NIX_LEXER_THREADS=1`. The branch with defaults
printed six different orders in six runs. A three-file case shows the
prefetcher alone doing it: `main.nix` imports `a.nix` and `b.nix`, `b.nix`
is forced first and mentions the names in the other order. Upstream prints
`zzz yyy`, the branch `yyy zzz` in 30 of 30 runs, and `zzz yyy` again with
`NIX_PREFETCH_IMPORTS=0`

Item F0 fixes this. The parse cache, the heap image and parallel evaluation
have the same problem and need the same fix

## Big designs, weighed

| Design | Verdict | Expected on the real system | Why |
|---|---|---|---|
| Result cache that answers without evaluating | do first (A1-A3) | target 1: 13-17 s to 20-50 ms | measured 215 M instructions on the existing hit path |
| Persistent parse cache, #173 | do, small (B8) | 2-3% wall, 4.9% CPU. 10% on minimal NixOS, 12-16% on hello | parse share after the fast parser. Its file-local symbol tables are what F0 and A5 need |
| Shapes or hidden classes with inline caches | do after the cheap items (C1) | 5-8% | lookups are 5.7% of the main thread, sets are 47% of allocation and half of a set is names and positions that a shape can share |
| Own heap in place of Boehm | later (C3) | 8-10% wall, 20% CPU | allocation 5.2% and marking 4.2% of the main thread, markers 18.6% of CPU, conservative marking pays for headers and black lists. A5 needs it anyway |
| Variables resolved to frame slots, AST compiled to closures or bytecode | later, modest (C2) | 5-8% | variables are already level and displacement pairs. The gain is fewer virtual calls and Value copies, on 25% of the time |
| Parallel evaluation | owner's call (C4) | 3-6 times less wall on 16 cores, CPU up | 17,929 derivations are mostly independent. Trace order, first error and symbol order can't be kept identical cheaply |
| NaN-boxed 8-byte Values | no | under 5% | Values are 23% of bytes and already 16 bytes. Strings, paths and big lists need two words, so they move behind a pointer. Every `Value` user and the C++ plugin ABI change |
| Baseline JIT | no | under 5% | dispatch is not where the time goes, and a JIT adds a code cache to the 50 ms start |
| Incremental evaluation with a persistent heap | research, gated (A5) | target 2 eval: 50-200 ms if it works | 15 of 17,929 derivations change. Nothing smaller reaches 100 ms |

## Work items, ranked

Effort: S is a day or less of agent time, M a few days, L one to two
weeks, XL more. Gains are shares of main-thread time on the real system
unless said otherwise. They come from the category's measured share times
the part I expect to remove, so they are estimates. "Parallel" says whether
the item can run next to the others of its batch without conflicting edits

### F0. Keep symbol order equal to upstream's

- Gain: none in time. It restores identical behaviour, shown broken above
- Change: prefetch threads and parser pieces intern into a table of their
  own. The main thread interns a file's symbols in order of first
  occurrence when upstream would have parsed the file (at the import), then
  rewrites the ids in the AST and sorts `AttrDefs` and formals. Pieces of
  one file merge in text order. Until that lands, default
  `NIX_PREFETCH_IMPORTS=0` and one parser thread
- Risk: low once done, since the order becomes upstream's by construction
- Effort: M. The stopgap is S
- Files: `src/libexpr/parser.cc`, `lexer.cc`, `eval.cc` (`ImportPrefetcher`),
  `include/nix/expr/symbol-table.hh`, `parser-state.hh`, `nixexpr.hh`
- Depends on: nothing. B8 and A5 build on it
- Parallel: yes, except with B8
- Test: the two reproducers above go into `tests/functional/lang` as
  `eval-okay` cases with `.err.exp`, plus 200 runs of each under T0 with
  default threads. `bench/scripts/check.sh` stays green

### T0. Differential eval harness

- Gain: none. Each item below is tested with it
- Change: `bench/scripts/eval-diff.sh <base-nix> <new-nix>` runs a corpus
  on both and compares stdout, stderr and exit code. Corpus:
  `tests/functional/lang/*.nix` with `--show-trace`, nixpkgs
  `lib/tests/misc.nix` and `lib/tests/modules.sh`, 500 sampled nixpkgs
  attributes including broken and unfree ones for their error text,
  `nix-env -f $NP -qaP --drv-path --out-path`, `hello`, `nixos.nix`, and the
  real system at a pinned revision
  (`git+file:///etc/nixos?rev=<rev>`). Each with and without `--read-only`,
  since the two give different drv hashes. `bench/scripts/eval-bench.sh`
  prints instructions and wall for the three workloads. Build the base once
  with `TREE=<checkout of 5533e016a> bench/scripts/build.sh dev 24`
- Effort: M. Files: `bench/scripts/` only. Parallel: yes

### A1. Make the eval cache answer target 1

- Gain: 13-17 s to about 80-100 ms wall on a busy machine (215 M
  instructions measured for `nix build --dry-run` on a clean revision)
- Change, three parts:
  1. `nix eval` on a flake attribute reads the cursor first and prints
     cached strings, bools, ints and string lists. For a string with
     context outside `--read-only`, check that the context paths are valid,
     as `AttrCursor::forceDerivation` does, and evaluate if not
  2. Fingerprint dirty trees that have submodules: recurse into each
     submodule's head and dirty files in `GitInputScheme::getFingerprint`
  3. Record what the evaluation printed (`builtins.trace`, `warn`,
     evaluator warnings) next to the cached attribute and print it again on
     a hit. Record it only when the attribute was the first thing a fresh
     `EvalState` evaluated, otherwise mark the entry as not replayable and
     evaluate
- Risk: medium. Upstream prints nothing on a cache hit, so part 3 differs
  from upstream-with-cache and equals upstream-without-cache. That choice
  is the owner's (see decisions). The cache key must cover what pure eval
  can see: flake fingerprint, lock file, `system`, store dir, experimental
  features, eval settings, the binary's build id
- Effort: M
- Files: `src/nix/eval.cc`, `src/libcmd/installable-flake.cc`,
  `src/libexpr/eval-cache.cc`, `include/nix/expr/eval-cache.hh`,
  `src/libfetchers/git.cc`, `src/libflake/flake.cc`, `src/libutil/logging.cc`
- Depends on: nothing. Parallel: yes
- Test: `tests/functional/flakes/eval-cache.sh` plus new cases. Run twice
  and diff stdout and stderr against `--no-eval-cache`, for a clean tree, a
  dirty tree, a dirty submodule, a config that traces and one that throws.
  Edit a file, edit a submodule file, change `--system` and check each is a
  miss. Delete the cached `.drv` with `nix-store --delete` and check the
  eval runs again

### B1. Build each derivation's text once

- Gain: 15-20% (JSON 17.3%, unparse and hashing 12.1%, plus the malloc
  under them). 8% on hello and minimal NixOS
- Change: turn structured attrs into their JSON string once and keep it
  with the derivation. Write the ATerm once into a reused buffer, with the
  output paths and input hashes as holes, so the masked hash, the final
  text and the dependents' hash come from one pass with patched ranges.
  Go from `Value` to JSON text without an nlohmann tree where nothing reads
  the tree. Parse `derivationOptionsFromStructuredAttrs` once per
  derivation
- Risk: low to medium. The `.drv` bytes must not change. nlohmann's `dump`
  defines key order (sorted, byte-wise), float text, escapes and the error
  on invalid UTF-8, and the writer has to match it byte for byte
- Effort: L
- Files: `src/libstore/derivations.cc`, `parsed-derivations.cc`,
  `include/nix/store/derivation/*.hh`, `src/libexpr/primops.cc`
  (`derivationStrictInternal`), `value-to-json.cc`
- Depends on: nothing. Parallel: yes. A4 touches the same tail of
  `derivationStrictInternal`, so the same agent does A4 next
- Test: `libstore-tests` derivation golden files, T0 (equal top-level drv
  path means every `.drv` in the closure is byte-identical, check it in
  write mode and with `nix-env -qa --drv-path` over nixpkgs), a fuzz test
  that feeds random JSON values through the old and new writer, and
  `builtins.toJSON` on floats, large ints and invalid UTF-8

### A4. Stop the round trip per derivation

- Gain: 4-5 s of wall for a user eval that writes derivations (36k round
  trips). As root it saves 36k SQLite queries, not measured
- Change: queue `addTempRoot` and the validity check, send them in batches
  (`AddTempRoots` and `QueryValidPaths` exist), and write the missing
  derivations in dependency order. Flush before anything reads the store:
  import from derivation, `readFile` on a store path, a build, the end of
  the eval
- Risk: low to medium. A store error now surfaces at the flush. A
  derivation not yet written can't be collected, and source paths get their
  temp roots when added, so the GC window does not grow
- Effort: M
- Files: `src/libstore/derivations.cc` (`writeDerivation`),
  `remote-store.cc`, `store-api.cc`, `src/libexpr/primops.cc`, `eval.cc`
- Depends on: B1 (same code). Parallel: no, after B1 in the same worktree
- Test: functional suite, T0 in write mode with `nix-store -qR` and
  `sha256sum` of the closure's `.drv` files, an IFD case, `strace -c` shows
  under 200 round trips on the real system, a run against the 2.35 daemon

### A2. Start in 5 ms

- Gain: 46 M of 69 M start-up instructions, about 13 of 18 ms per process.
  A switch starts at least three nix processes
- Change: link the nix libraries into the `nix` binary statically for the
  installed build, or keep them shared with `-Bsymbolic-functions`,
  `-fno-plt`, hidden visibility for inlines and lazy binding. Then defer
  what start-up does for commands that don't need it: OpenSSL and libgit2
  init, regex compiles in static initialisers, settings docs through
  `stripIndentation`
- Risk: low for behaviour. Plugins that `dlopen` and expect the shared
  libraries need the shared build kept as an option
- Effort: M
- Files: `meson.build`, `src/*/meson.build`, `packaging/`, static
  initialisers in `libutil`, `libstore`, `libflake`
- Parallel: yes
- Test: full functional and unit suites on the static build, a plugin
  loaded through `plugin-files`, `perf stat -r 20 nix --version` under 25 M
  instructions

### B2. Cheap path checks

- Gain: 6-8%. 5% on hello
- Change: `CanonPath::isAllowed` walks a `std::set<CanonPath>` once per
  ancestor on each access (2.2% self in o3). Cache the verdict per
  directory and keep allowed prefixes in a hash set. Key the
  `copyPathToStore` and `lstat` caches by an interned path, since
  `CanonPath::operator<=>` on `/nix/store/…` strings is 2.1%. `"${./x}"`
  goes through `fetchToStore` on each coercion (2.3% under
  `coerceToString`), so remember the result per `(accessor, path)`
- Risk: low. A denied path must raise the same error at the same access
- Effort: M
- Files: `src/libutil/canon-path.cc`, `src/libfetchers/filtering-source-accessor.cc`,
  `fetch-to-store.cc`, `src/libutil/posix-source-accessor.cc`,
  `src/libexpr/eval.cc` (`copyPathToStore`, `allowPath`), `paths.cc`
- Parallel: yes
- Test: `tests/functional/restricted.sh`, `pure-eval.sh`, the flake suite,
  T0, a test that allows a path mid-eval and reads below it

### A5. Incremental evaluation: measure first (S0)

- Gain: unknown until measured. It is the only route to target 2's eval
- Change, stage S0 only: a throwaway build that logs, for each forced
  thunk, its expression, the thunks it forced, and its cost. Run it on the
  real system and on the `hello` edit, and compute offline how many thunks
  the edit dirties, how many dependency edges there are, and the memory
  they need. No evaluator changes are merged
- Go on to S1-S5 (under "Research track") only if the dirty set costs under
  50 ms to redo and the edges fit in about twice today's heap
- Risk: none, nothing ships. Effort: M
- Files: a scratch worktree off `fast-eval`. Parallel: yes

### A3. Trim the hit path to 20 ms

- Gain: from about 170 M instructions after A2 to 60-90 M
- Change: remember the tree's fingerprint and store path against the git
  head, the index stat and the stat of each dirty file, the way git's index
  does, and rehash a file whose mtime is not older than the record. Keep
  the parsed lock (93 inputs, 55 KB) keyed by its content hash. Build
  `EvalState` and open the store only on a miss. Check the cached drv with
  one daemon call on a connection opened late
- Risk: low to medium. A file changed without a size, mtime, ctime or inode
  change inside one timestamp tick is missed, as in git
- Effort: M
- Files: `src/libfetchers/git.cc`, `git-utils.cc`, `fetch-to-store.cc`,
  `src/libflake/flake.cc`, `lockfile.cc`, `src/libcmd/installable-flake.cc`
- Depends on: A1. Parallel: yes
- Test: the A1 cases, plus touch-without-change, change-within-the-same-
  second, `git stash`, branch switch, submodule update

### B3. Plain iteration over attribute sets

- Gain: 3-4% (iterator 5.0%, 2.0% of it under `//`)
- Change: `Bindings::iterator` carries a heap of eight cursors even for a
  set with one layer. Give unlayered sets a pointer range. For layered
  sets, merge with a linear scan over at most eight cursors, and flatten
  once when a set is walked in full. In `ExprOpUpdate::eval` merge from raw
  spans
- Risk: low. Order and duplicates stay as they are
- Effort: S to M
- Files: `src/libexpr/include/nix/expr/attr-set.hh`, `attr-set.cc`,
  `eval.cc` (`ExprOpUpdate::eval`)
- Parallel: yes. C1 comes after it
- Test: `libexpr-tests` (the `Bindings` and dynamic-attrs tests), lang
  tests, T0, a property test comparing old and new iteration over random
  layer stacks

### B4. String contexts without a parse and print per string

- Gain: about 3% (string contexts are 5.6% of main-thread self time)
- Change: `copyContext` parses each element into a variant and inserts it
  into a `std::set`, and `mkString` prints it back. Carry contexts as
  sorted arrays of interned strings, merge arrays on concatenation, and
  parse only where a typed element is needed (`derivationStrict`,
  `getContext`, store path coercion)
- Risk: low to medium. Element order in the final context must stay the
  set's order, and parse errors must surface where they do now
- Effort: L, many callers take `NixStringContext &`
- Files: `src/libexpr/value/context.cc`, `include/nix/expr/value/context.hh`,
  `eval.cc` (`coerceToString`, `ExprConcatStrings`, `copyContext`),
  `primops.cc`, `primops/context.cc`
- Depends on: after B1 and B2, which touch the same functions. Parallel:
  with B5-B9
- Test: lang tests on contexts, `libexpr-tests` `value/context.cc`, T0,
  `builtins.getContext`, `appendContext`, `unsafeDiscardOutputDependency`
  round trips

### B6. Small interpreter fixes

- Gain: 4-6% together
- Change:
  - match formals against arguments by merging two sorted ranges instead of
    a binary search per formal (`callFunction`)
  - `ExprLambda::maybeThunk` returns the closure, which cannot fail, so no
    thunk is made and forced later. An empty `{ }` returns a shared value
  - free lists from `GC_malloc_many` for envs of 2 to 8 slots, as size 1
    has now
  - allocation caches as initial-exec TLS or `EvalMemory` members, not
    `thread_local shared_ptr` behind `__tls_get_addr` (0.8%)
  - build libexpr with `-fno-plt` and hidden inline visibility (PLT stubs
    and `__tls_get_addr` are 2.6% of main-thread self time)
  - skip the `Finally` and hook tests in `callFunction` when no profiler is
    attached
- Risk: low. The lambda change alters `nrThunks` and `nrAvoided` in
  `NIX_SHOW_STATS`, nothing else
- Effort: M
- Files: `src/libexpr/eval.cc` (`callFunction`, `maybeThunk`),
  `include/nix/expr/eval-inline.hh`, `nixexpr.hh`, `meson.build`
- Parallel: yes within its batch. C2 comes after it
- Test: lang tests, `error_traces.cc`, T0 with `--show-trace` on the
  failing corpus (missing and unexpected arguments in particular)

### B5. Symbol table on the eval path

- Gain: about 2%. More on hello, where symbols are 9% of self time
- Change: the table is a locked concurrent set, and `removeAttrs`,
  `listToAttrs`, `readDir` and `derivationStrict` create or look up
  symbols per call. Put the parser's thread-local cache in front for the
  evaluating thread and skip the lock when one thread is interning
- Risk: low, ids must come out in the same order
- Effort: S to M. Files: `include/nix/expr/symbol-table.hh`, `primops.cc`
- Depends on: F0. Parallel: yes
- Test: T0, the F0 reproducers

### B8. Persistent parse cache (#173)

- Gain: 2-3% wall on the real system, 8% on minimal NixOS, 12% on hello
- Change: store each file's AST keyed by content hash and build id, with
  file-local symbols and positions, and replay the interning on load as F0
  defines it. Fall back to parsing on a mismatch. Positions and doc
  comments must come back equal
- Risk: medium. A stale or corrupt entry changes results, so entries carry
  a checksum and the build id
- Effort: L
- Files: `src/libexpr/parser.cc`, `eval.cc` (`evalFile`, `parseExprFromFile`,
  `ImportPrefetcher`), a new `ast-cache.cc`, `nixexpr.hh`
- Depends on: F0. Parallel: yes, not with F0
- Test: `bench/parse-bench --dump` hashes with a cold and a warm cache,
  the fuzzer with the cache on, T0 with a cold and a warm cache, error
  positions on the failing corpus

### B7. `readDir` without a stat and a string per entry

- Gain: 8-10% on hello, under 1% on the real system
- Change: read with `getdents64`, take the type from `d_type`, intern names
  in one pass, build the result sorted
- Risk: low. Effort: S. Files: `src/libexpr/primops.cc` (`prim_readDir`),
  `src/libutil/posix-source-accessor.cc`. Parallel: yes
- Test: `readDir` lang tests, a directory with a symlink, a FIFO and an
  unknown `d_type`, T0

### B9. Collect less

- Gain: 3-5% wall, 13% of instructions (`GC_DONT_GC` saves 9.7 G)
- Change: raise the first collection threshold when memory allows, so an
  eval under about 2 GB does not collect. The heap grows to what was
  allocated, 1.54 GB here against 1.06 GB now
- Risk: none for results. Effort: S. Files: `src/libexpr/eval-gc.cc`.
  Parallel: yes
- Test: `lang-gc.sh`, peak RSS on the three workloads, a run under
  `systemd-run -p MemoryMax=1G`

### C1. Shapes for attribute sets

- Gain: 5-8%, and 20% less allocation
- Change: split `Bindings` into a shared shape (sorted names and
  positions) and a per-set array of value pointers. Sets from the same
  `ExprAttrs`, and the results of `mapAttrs` and of `//` with a constant
  right side, share shapes. `ExprSelect` and formals matching cache
  `(shape, index)` per site. Keep the symbol-sorted order
- Risk: medium. `Attr` is used in about 300 places and in
  `src/libexpr-c/nix_api_value.cc`. `unsafeGetAttrPos` needs per-set
  positions where two sets with equal names differ in them
- Effort: XL. Files: `attr-set.hh` and its users across `libexpr`,
  `libexpr-c`, `libcmd`, `nix`
- Depends on: B3. Parallel: no, alone in batch 3
- Test: full suites, T0, the C API tests, `unsafeGetAttrPos` cases, peak
  RSS

### C3. Own heap for values

- Gain: 8-10% wall, about 20% CPU
- Change: bump allocation in regions for `Value`, `Env`, `Bindings`, lists
  and strings, with precise tracing from the evaluator's roots, or no
  collection below a limit. Boehm stays for what the C API and plugins
  allocate
- Risk: high. Roots live on the C++ stack and in containers with
  `traceable_allocator` across the code base, and a missed root is a
  use-after-free that shows up rarely
- Effort: XL. Depends on: C1. Parallel: no
- Test: ASan and a GC stress mode that collects on each allocation over
  the lang tests and T0

### C2. Compiled expressions with frame slots

- Gain: 5-8%
- Change: lower the AST after `bindVars` to a compact form with resolved
  slots and no virtual `eval`, one loop or closure per node kind, and
  values returned in registers. Positions and trace text stay tied to the
  same nodes
- Risk: medium to high. Error traces, `--debugger` frames and the stack
  depth limit come from the current recursion
- Effort: XL. Depends on: B6, C1. Parallel: no, it rewrites `eval.cc`
- Test: full suites, `error_traces.cc`, T0 with `--show-trace`, deep
  recursion cases for the stack overflow message

### C4. Parallel evaluation

- Gain: 3-6 times less wall on 16 cores for derivation-heavy evals
- Risk: high for identical behaviour. Trace order, which error comes
  first, symbol order and the infinite recursion check depend on one
  thread forcing in order. Buffering traces per thunk and replaying them
  in sequential order is possible and shares machinery with A5
- Effort: XL. Only on the owner's say
- Files: `eval.cc`, `eval-inline.hh`, `value.hh`, `primops.cc`,
  `symbol-table.hh`. Depends on: F0, C3. Parallel: no
- Test: T0 run 100 times per case with stderr compared, TSan
  (`build.sh tsan`) on the lang tests and the minimal NixOS

### A6. Survey the build path

- Why: after an edit, 13 local builds at 50-80 ms each and 0.85 s of
  substituter lookups per uncached path dwarf a 100 ms budget, and none of
  it is in libexpr
- Effort: M, a survey like this one, with root for the daemon profile

### Research track: incremental evaluation (A5, S1 to S5)

Only after S0 says go. The design is demand-driven recomputation with
early cutoff, as in Adapton or Salsa, on a heap that outlives the process:

- S1: stable expression ids, file identity by input and relative path,
  from B8's AST cache
- S2: the evaluator's own heap (C3), saved to and mapped from a file
- S3: reuse of subgraphs that depend on immutable inputs only (`lib`,
  parsed files)
- S4: thunks keep their expression and env after forcing and record what
  they forced. After a file changes, dirty thunks are redone on demand in
  one process, and a thunk whose inputs come out equal is not redone. Each
  result is compared against a fresh eval
- S5: the same across processes
- What must hold: trace and warning output replayed in from-scratch order,
  pointer identity as in a fresh eval (it decides `==` on functions),
  symbol order per F0, failed thunks rethrown as recorded
- Kill criteria: S0 as above. S4 fails if the first eval gets more than
  1.5 times slower or a differential test finds an order we can't
  reproduce

## Order and batches

Batches hold items that separate agents can do at once in separate
worktrees (`~/projects/worktrees/nix/<name>`), each rebased onto
`fast-eval` when done. `eval.cc` is shared by several items, in different
functions, named above

- Batch 0, two agents: T0. F0
- Batch 1, six agents: A1. B1 then A4. A2. B2. B3. A5-S0
- Batch 2, seven agents: A3. B4. B5. B6. B7. B8. B9
- Batch 3, one at a time: C1, then C3, then C2
- Any time, separate: A6. C4 and A5 S1-S5 on the owner's decision

Expected after each batch, real system:

| | Target 1 | Eval after an edit, user |
|---|---|---|
| today | 13-17 s | 13-17 s wall, 9-10 s CPU |
| batch 1 | 80-100 ms | 6-7 s wall, 6.5-7 s CPU |
| batch 2 | 20-50 ms | 5-6 s |
| batch 3 | 20-50 ms | 4-4.5 s |

## Test plan shared by the items

- Unit and functional suites in the devshell: `meson test -C build`. The
  evaluator's are `nix-functional-tests` (`lang.sh` with the
  `tests/functional/lang` golden files, `lang-gc.sh`), `libexpr-tests`
  (`error_traces.cc`, `primops.cc`) and `libstore-tests` (derivation golden
  files)
- T0's `eval-diff.sh` against the base build, in read-only and write mode.
  The end-to-end check is an equal top-level drv path for hello, the
  minimal NixOS and the pinned real system, and an equal `nix-env -qaP
  --drv-path --out-path` listing
- `bench/scripts/check.sh` and the parser fuzzer for items that touch the
  front end
- ASan and UBSan (`build.sh asan`) on the lang tests and the minimal NixOS
- Report instructions from `perf stat -e instructions:u` for the three
  workloads before and after, since wall times on this machine move with
  load

## Decisions for the owner

1. The branch's parallel parser and prefetcher change trace and error
   order (F0). Turn them off by default until fixed, or accept the
   difference for now
2. On an eval cache hit, upstream prints no traces or warnings. A1 can
   replay them (equal to an uncached eval) or stay silent (equal to
   upstream with its cache)
3. Target 1 through a stat-based fingerprint (A3) trusts size, mtime,
   ctime and inode as git does. Without it the floor is about 80 ms
4. Target 2 needs incremental evaluation (A5), which is research. Run S0,
   a few days, and decide on the numbers
5. Does "Nix's time" in target 2 include the daemon's per-build overhead
   and substituter lookups? If so, commission A6
6. Parallel evaluation (C4) can't keep trace and error order identical
   cheaply. In or out
7. `nixos-rebuild` evaluates twice and starts Python twice. `--no-reexec`
   saves 2.1-2.8 s today and is outside Nix
