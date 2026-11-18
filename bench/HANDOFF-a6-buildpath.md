# Handoff: A6 build path survey (paused)

Paused on the coordinator's request because the machine is overloaded.
Worktree `~/projects/worktrees/nix/fe-a6-buildpath`, branch `fe-a6-buildpath`,
based on `fast-eval` (aef921326). Rules in `bench/AGENT-RULES.md`, including
the flock section at its end

## Committed

- `bench/scripts/nix-timeline.py`: `stamp` prefixes `--log-format internal-json`
  lines with ms, `report` lists activities (builds, hooks, narinfo lookups)
- `bench/scripts/a6/`: `triv.nix` (daemon store), `triv2.nix`, `chain.nix`
  (13-deep chain), `wide.nix` (top drv with N built inputs) for a chroot store,
  `inclosure.py` (input closure size per drv)
- `bench/a6-logs/hello-realise.txt` (realise timeline), `variants.json`
  (8 toplevel drvs, one extra package each: hello, figlet, sl, cmatrix,
  toilet, banner, nyancat, cowsay; only hello is realised)
- `bench/BUILD-PATH.md`: the partial survey with numbers and the ranking
- Nothing uncommitted. No Nix code changed. No NOTES.md section yet

## Setup in scratch (lost on reboot, easy to redo)

- `$S=/tmp/claude-1000/-home-hugo-projects-nix/b2cb8e62-361f-4d78-84ec-c4fa14dd3c56/scratchpad/fe-a6-buildpath`
- `$S/nixos`: `git clone /etc/nixos` at 19709e0, clean. `$S/nixos-hello`: same
  plus `hello` in `hosts/beryllium/default.nix` systemPackages
- `$S/st`: chroot store with busybox and the `wide.nix` leaves for width 1
  and 800 already built
- Variant drvs came from one eval (1590 s wall under load, heavy lock):
  `nix eval --json "$S/nixos#nixosConfigurations.beryllium" --apply 'c: map (n: { inherit n; d = (c.extendModules { modules = [ ({ pkgs, ... }: { environment.systemPackages = [ pkgs.${n} ]; }) ]; }).config.system.build.toplevel.drvPath; }) [ "hello" ... ]'`

## Next steps

1. Synthetic sandbox cost: `wide.nix` top with width 1 vs 800 under
   `perf stat -e instructions:u,task-clock` (heavy lock). Width 1 gave
   167 M instr, 155 ms. Width 800 not run
2. Profile the 13-deep chain in-process (`perf record -e cycles:u
   --user-callchains -g`, see EVAL-PLAN.md) to find what the 97 M
   instructions per trivial build are. Small safe fixes likely live here
3. Realise 2-3 more variants from `variants.json` for spread, one with the
   build lock only (`flock .../build.lock nix build --max-jobs 4 --cores 3 "$d^*"`)
4. Measure the re-exec eval on the dirty `$S/nixos-hello`:
   `nix build "$S/nixos-hello#nixosConfigurations.beryllium.config.system.build.nixos-rebuild" --no-link`
5. Ask the owner: concurrent substituter lookups (behaviour change in
   request pattern), the cachix hook, the nixpkgs `activate` fix
6. Add the NOTES.md result section, then finish BUILD-PATH.md

## Gotchas

- Never run `nix-env` (owner rule); `nix build --profile <scratch>` stands in
  for `nix-env --set`. No root, so the daemon is only timed by wall clock
- A clean clone at HEAD hits the eval cache (0.27 s), a dirty tree does not
- The first base build waited 5 s with no build activity, likely on another
  session's lock for the same drv. Use fresh variants
- `flock` wait counts in the wall time of a pipeline; the timeline's first
  stamp then sits at about 1,476 s
- `instructions:k` is unsupported (`perf_event_paranoid` 2), so kernel mount
  cost needs task-clock or sys time
- HTTPS goes through `HTTPS_PROXY`, which adds to narinfo round trips
