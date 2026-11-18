# Handoff: A2, start in 5 ms

Branch `fe-a2-startup` (its own worktree), rebased on `fe-a1-evalcache`
(9d00a384e). Nothing runs, nothing is uncommitted. Results are in
`bench/NOTES.md`, "start-up (A2)"

## Commits

- `fe-a2-startup~9` curl_global_init eagerly only on macOS
- `fe-a2-startup~8` global regexes compiled on first use (C++ API change)
- `fe-a2-startup~7` `AbstractSetting::description()` on demand (C++ API change)
- `fe-a2-startup~6` this file
- `fe-a2-startup~5` the libraries export a static archive, a whole-archive
  dependency and pkg-config file under `default_library=both`
- `fe-a2-startup~4` `nix:static-nix-libraries`, off by default; the C++
  plugin test is skipped when it is on
- `fe-a2-startup~3` packaging turns it on for Linux (`withStaticNixLibraries`)
- `fe-a2-startup~2` flag descriptions without boost::format
- `fe-a2-startup~1` Boehm gets the main stack bottom from `__libc_stack_end`
- `fe-a2-startup` packed relative relocations in `nix`

## Build dirs

- `build`: shared, `bench/scripts/build.sh dev`
- `build-static`: the same plus `-Ddefault_library=both
  -Dnix:static-nix-libraries=true`, installed in `outputs/static`. The
  script is `build-static.sh` in the agent's scratch dir
- A fresh `build.sh dev` now configures static too, since the dev shell
  takes its flags from the packaging

## Left

- ld.so is still 70% of `nix --version`: the system libraries. The aws-c
  stack alone is a quarter of the bindings. Static system libraries need
  static builds of curl, OpenSSL, the aws stack and others in packaging
- `computeLevels()` (libcpuid, 1.1 M) and the setting flags (2 M) remain
- A clean `/etc/nixos` hit opens the repo with libgit2 (A1/A3's area)

## Gotchas

- The whole-archive dependencies must stay in dependency order in
  `src/nix/meson.build`, or ld.bfd marks a shared Nix library as needed
  and the static initialisers run twice. Check `readelf -d bin/nix`
- Unset `HTTPS_PROXY` for the libstore https tests
