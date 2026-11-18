# Build path survey (A6), partial

Where a `nixos-rebuild switch` that adds one cached package spends time
outside the main evaluation. Made on beryllium on 2026-11-18 at load 70 to
124 on 32 threads, so wall times are ranges and only instruction counts
compare. The survey was paused before the synthetic sandbox runs and the
in-process profile finished; see `HANDOFF-a6-buildpath.md`

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
| 13 trivial chained builds in-process | 1.27-1.28 G instr, 0.88-1.24 s CPU | about 97 M instructions a build, not profiled yet |

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
  toplevel 1132, dbus-1 765, the rest under 150. Cost per mount not
  measured yet (`instructions:k` is unavailable at `perf_event_paranoid` 2)
- `substituters` lists `https://cache.nixos.org` and `https://cache.nixos.org/`.
  Both open, since `getDefaultSubstituters` dedups before the trailing
  slash is stripped. The second lookup hits the first's negative disk-cache
  entry, so it costs a SQLite query and is not worth a change
- `queryMissing` and the worker both read and parse the input derivations
  of each rebuilt derivation (about 800 for this edit). Not measured apart

## Fixes, ranked so far

| Fix | Where | Gain | Risk to identical behaviour |
|---|---|---|---|
| Build `toplevel` and `nixos-rebuild` in one `nix build`, then re-exec with `--store-path`, or `--no-reexec` | nixos-rebuild | 2.1-2.8 s and one Python start (200 ms CPU) | the new script no longer drives the build, only activation |
| Replace the per-build cachix client in the hook, for example `cachix watch-store` or a plain socket write | `/etc/nixos/modules/cachix.nix` | 13 × (60-80 M instr plus a round trip), on the chain's critical path | none in Nix, owner's config |
| `allowSubstitutes = false` on `activate` | nixpkgs | the 830 ms of misses | none for local builds, it is never in a cache |
| Query substituters for one path concurrently, take the first hit by priority | `misc.cc` | sum of misses becomes the slowest one | extra requests to lower-priority caches, same result. Needs an owner decision |
| Bind the input closure with fewer mounts | libstore sandbox | unknown until measured | large change, stop and report first |
| Per-build in-process cost of about 97 M instructions | libstore | unknown until profiled | depends on the fix |
| Nix startup 67 M × 3 and dirty-tree fetch 187 M × 2 | A2, A1 | 200 M and 370 M instructions | covered by those items |

No Nix code was changed yet
