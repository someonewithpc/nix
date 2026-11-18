# Build path survey (A6)

Where a `nixos-rebuild switch` that adds one cached package spends time
outside the main evaluation. Made on beryllium on 2026-11-18. The first
half ran at load 70 to 124 on 32 threads, the profile at load 6 to 25, so
wall times are ranges and only instruction counts compare

System Nix and daemon are 2.35.6. In-process numbers use the fast-eval dev
build (2.36.0) on a chroot store (`local?root=...`), which runs the same
sandbox code as the daemon without root

## What a switch runs

From nixos-rebuild-ng 26.11 (`services.py`, `nix.py`):

1. Python start, 648 M instructions and 198 ms CPU for `--help`
2. Re-exec: `nix build <flake>#...config.system.build.nixos-rebuild --no-link`,
   a second, partial eval (2.1-2.8 s wall in EVAL-PLAN.md), then Python
   starts again. Only for switch, boot and test, skipped with `--no-reexec`
3. `nix build --print-out-paths <flake>#...toplevel --no-link`: flake
   fetch, eval, `queryMissing`, realise
4. `test -f <toplevel>/nixos-version`, then `nix-env -p /nix/var/nix/profiles/system --set`
5. `systemd-run --version`, `test -d /run/systemd/system`, then
   `systemd-run ... switch-to-configuration switch`, not measured (needs root)

Three Nix processes start. Each costs 67 M instructions (about 30 ms CPU)
before doing any work, the same as `nix --version`. Setting a profile with
`nix build --profile` also measures 67 M, so step 4 is all startup

## Measured, hello variant

Adding `pkgs.hello` gives 13 local builds and 1 substitution
(`bench/a6-logs/hello-realise.txt`, 8 variant drvs in `variants.json`):

| Piece | Measured | Notes |
|---|---|---|
| Flake fetch of a dirty git tree | 254 M instr, 95-113 ms CPU for `nix flake metadata` | 187 M above startup, paid by both evals |
| `queryMissing` in the daemon | 1.88 s wall | 830 ms of it is three narinfo misses for `activate`, one after the other (241, 264, 324 ms) |
| Substitute hello | 272 ms wall | 74 KiB NAR |
| 13 local builds | 39.6 s wall at load 85, 35.3 s summed | `system-path` alone 20 s wall |
| Post-build hook, 13 runs | 46-2303 ms each, 6.3 s summed | runs after each build, see below |
| Trivial build through the daemon | 118 ms to build start, 65 ms build, 98-1042 ms hook | 6 runs at load 124 |
| 13 trivial chained builds in-process | 1.27-1.28 G instr, 0.88-1.24 s CPU | about 97 M instructions a build, almost all of it the host's post-build hook, see below |

## Findings

- Only `activate` among the 14 new derivations allows substitutes. It is
  `writeShellApplication` with `preferLocalBuild = true` overridden but not
  `allowSubstitutes` (`nixos/modules/system/activation/activatable-system.nix`).
  So every switch pays a narinfo miss per substituter for it.
  `querySubstitutablePathInfosAsync` tries substituters in turn, so the
  misses add up
- The post-build hook is the host's cachix enqueue script: bash (8 M
  instructions), `timeout` (2.7 M), the cachix client (49-68 M) and a socket
  round trip. Nix has run the hook asynchronously since 2.35.0
  (e4c4f920d), but the goal holds its output locks and only finishes after
  the hook, so each level of the build chain waits for it
- The sandbox makes one `open_tree` and `move_mount` per path in the input
  closure (`linux-derivation-builder.cc`, `doBind`). The input closures of
  the 14 builds add up to about 12,000 paths: system-units 2278,
  system-path 2116, etc 2074, home-manager unit 1791, user-units 1228,
  toplevel 1132, dbus-1 765, the rest under 150. Each costs 0.07 to
  0.13 ms of kernel time, see "Input width"
- `substituters` lists `https://cache.nixos.org` and `https://cache.nixos.org/`.
  Both open, since `getDefaultSubstituters` dedups before the trailing
  slash is stripped. The second lookup hits the first's negative disk-cache
  entry, so it costs a SQLite query and is not worth a change
- `queryMissing` and the worker both read and parse the input derivations
  of each rebuilt derivation (about 800 for this edit). Not measured apart

## The 97 M instructions per trivial build

The earlier in-process runs read `/etc/nix/nix.conf`, so the host's
post-build hook ran after each of the 13 builds. That hook is the cost.
It also enqueued chroot-store paths on the real cachix daemon, whose
socket is world-writable; the runs below set `post-build-hook` and
`substituters` empty. Commands and scripts are in `bench/scripts/a6/`;
the 13-deep chain is `chain.nix`, built from its instantiated top drv
with `nix build --store local?root=$S/st "$d^*"`

| Run | Instructions | CPU |
|---|---|---|
| `nix --version` | 80.0 M | |
| No-op build of the built chain | 81.6 M | 32-57 ms |
| 13 chained builds, no hook | 114.8 M | 167-236 ms (34-39 ms user, 142-145 ms sys) |
| 13 builds, hook is `bash` running `exit 0` | 244.2 M | |
| 13 builds, the proposed dash spool hook | 140.6 M | 137-146 ms |
| cachix client alone, socket unreachable | 74.3 M | 26-40 ms |

So a trivial local build costs Nix about 2.6 M user instructions and
10 ms of kernel time, 14 ms wall. The current hook adds about 87 M
instructions per build: the cachix client (74 M before it even talks to
the socket), bash (8-9 M, most of it glibc loading gconv modules for
`en_US.UTF-8`; 2.2 M with an empty environment), `timeout` (2.7 M) and
about 1.6 M in Nix to fork the hook. The hook is on the critical path:
the goal waits for it while holding the output locks, so each level of a
build chain waits for it

From `perf record -e instructions:u -c 400000` over 5 chains (the period
has to stay above about 160 k: `perf_event_max_sample_rate` is 31000, and
a smaller period throttles and undercounts), per build:

| Where | Instructions |
|---|---|
| Worker goals in the client: derivation and building goals, `registerOutputs`, SQLite | 0.9 M |
| Forked child before exec, almost all `setupSeccomp` building the filter with libseccomp | 0.7 M |
| busybox builder | under 0.1 M |
| Hook fork in Nix: `replaceEnv` 0.8 M (fixed, see below), `toKeyValue` for `NIX_CONFIG` 0.6 M | 1.6 M |

Of the 80 M startup, 70 M is the dynamic loader relocating the dev
build's libraries (`_dl_relocate_object`), which is A2's subject

Kernel time is 10 ms a build and cannot be split further here
(`instructions:k` and kernel callchains need `perf_event_paranoid` below
2). `strace -f -c` on the chain shows per build 2 clones (one with the
sandbox namespaces, 1.4 ms each), 1 `pivot_root` (0.7 ms), 18
`open_tree` and `move_mount` pairs, 7 mounts, about 150 `openat` and 41
`unlinkat` (chroot teardown)

## Input width

`wide.nix` builds a top derivation with 1 or 800 already built inputs,
3 runs each:

| Inputs | Instructions | User | Sys |
|---|---|---|---|
| 1 | 84.1 M | 14-21 ms | 26-37 ms |
| 800 | 384.4 M (373.5 M after the loadDerivation fix) | 64-94 ms | 82-137 ms |

An already built input costs about 360 k user instructions and 0.07 to
0.13 ms of kernel time. The kernel part is the bind mount per closure
path. The user part is spread over goal setup: per input derivation,
counted with gdb breakpoints, 8 `isValidPath` calls (each a SQLite query,
6416 in all), 2 `queryPartialDerivationOutputMap`, 1 read and parse of
the drv, and `queryMissing` walking the same inputs before the worker
does. `isValidPath` alone is 59 M of the 300 M. `Store::isValidPath`
caches negative answers but not positive ones (a FIXME upstream)

## Daemon against in-process

The in-process store runs the same `DerivationBuildingGoal`, sandbox and
`registerOutputs` code as the daemon. Differences:

- The daemon is 2.35.6 and cannot be replaced, so none of the fixes reach
  a switch on this host until nix-daemon runs a Nix that has them
- Through the daemon, the client sends `queryMissing` and `buildPaths`
  over the socket and the daemon streams logs back. The earlier daemon
  runs measured 118 ms from start to build start per trivial build at
  load 124, by wall clock only (no root, so no perf on the daemon)
- The root daemon takes a build user lock per build and runs the
  builder under that uid; the root-less store maps uid 1000 in a user
  namespace. The chroot store's real store dir differs from `/nix/store`,
  which changes only bind mount sources
- The daemon's hook inherits the daemon's environment, not a login
  shell's; bash's locale cost depends on `LANG` there

## Fixes made

| Commit | Gain | Behaviour |
|---|---|---|
| Query substituters for a path concurrently (`misc.cc`) | dry run with 5 misses over 4 substituters: 1165-1358 ms to 622-806 ms | same results; lower-priority caches get requests the loop would have skipped; debug lines come in a different order |
| Pass the hook environment to `execvpe` | 1 M instructions per hook run | same environment, same order |
| Read the derivation from the store found to hold it | 13.6 k instructions per input drv, 384.4 M to 373.5 M at width 800 | same |

The substituter change helps `queryMissing`, which is where the three
`activate` misses (830 ms) were. One side effect: a
lower-priority cache that is down now gets asked even when a higher one
answered, so its "disabling binary cache" warning can show up where it
did not before

## Fixes, ranked

| Fix | Where | Gain | Risk to identical behaviour |
|---|---|---|---|
| Spool hook with batched push, `bench/a6-proposals/cachix-spool.patch` | `/etc/nixos/modules/cachix.nix` | about 85 M instructions and 50-2300 ms per local build, off the critical path | none in Nix. Paths reach cachix a few seconds later, in batches |
| Build `toplevel` and `nixos-rebuild` in one `nix build`, then re-exec with `--store-path`, or `--no-reexec` | nixos-rebuild | 2.1-2.8 s and one Python start (200 ms CPU) | the new script no longer drives the build, only activation |
| `allowSubstitutes = false` on `activate` | nixpkgs | the 830 ms of misses, or the slowest one after the concurrent lookups | none for local builds, it is never in a cache |
| Bind the input closure with fewer mounts | libstore sandbox | 0.07-0.13 ms kernel per input path, 0.8-1.6 s over the 12,000 paths of a switch | large: other store paths must stay invisible to the builder |
| Cache positive `isValidPath` answers for the worker's lifetime | libstore | about 59 M of the 300 M at width 800 | a path deleted by GC during the build would still count as valid, as `queryPathInfo`'s cache already allows |
| Share derivation reads and validity between `queryMissing` and the worker | libstore | part of the 75 M `printMissing` share at width 800 | medium, two code paths with different error handling |
| Build the seccomp filter once per process and load the BPF in the child | `linux-derivation-builder.cc` | 0.7 M instructions per build, about 0.2 ms | low, but needs a check that the exported program matches |
| Compute `NIX_CONFIG` for the hook once per worker | `derivation-building-goal.cc` | 0.6 M per hook run | settings changed mid-build would no longer show up |
| Nix startup 80 M × 3 and dirty-tree fetch 187 M × 2 | A2, A1 | 240 M and 370 M instructions | covered by those items |
