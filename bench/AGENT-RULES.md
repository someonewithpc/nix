# Rules for agents working on fast-eval items

- Work only in your own worktree `~/projects/worktrees/nix/fe-<item>`, on branch `fe-<item>`, which starts at `fast-eval`. Read `bench/EVAL-PLAN.md` (your item, "Test plan shared by the items") and `bench/NOTES.md` ("How it is tested") first
- Behaviour must stay identical to upstream Nix: results, errors, traces, warnings, laziness and evaluation order. When in doubt, test against the `base` worktree build (`~/projects/worktrees/nix/base/outputs/out`)
- Build with `bench/scripts/build.sh dev 4`: 4 jobs only, since about six agents build at once. ccache is shared, so the first build is faster than it looks. One build of your own at a time
- The machine is loaded by other agents. Judge speed by instructions (`perf stat -e instructions:u`) and profile shares, with interleaved runs, not one wall-clock run. perf and hyperfine paths are in EVAL-PLAN.md
- Run `tests/functional/lang` (and the unit tests for what you touch) before committing. Report "N passed, M failed" and name failures
- Commit one concern per commit with `WIP: ...` subjects as on the branch. Commits sign automatically; never disable signing. Add no Co-authored-by or agent trailers, and check `git log -1 --format=%B` after committing. No trailing periods on list items, comments or commit paragraphs. Minimal comments
- Don't push, don't touch other worktrees or `/etc/nixos` (read only), never kill processes you didn't start, don't change system settings or the power profile, never search `/`, `/nix/store` or home broadly
- Scratch goes in `/tmp/claude-1000/-home-hugo-projects-nix/b2cb8e62-361f-4d78-84ec-c4fa14dd3c56/scratchpad/fe-<item>/`
- Add a short result section to `bench/NOTES.md` in your worktree (numbers, commands) as its own commit
- Stop and report if the item turns out much larger than the plan says, or needs a decision from the owner
- Final report under 25 lines: commits (as `fe-<item>~N` refs), numbers, test results, open questions

## Sharing the machine (added after start, overrides the build rule above)

- The machine is overloaded (load over 100). Builds take turns: run every build as `flock /tmp/claude-1000/fe-locks/build.lock bench/scripts/build.sh <kind> 12`, never without the lock. Other compiles (`ninja`, `meson test`) take the same lock
- Heavy runs take their own lock: `flock /tmp/claude-1000/fe-locks/heavy.lock <cmd>`. That covers full-system evals, `tests/functional` suites, perf runs and nixpkgs/NixOS closure comparisons. Light probes (a single small eval) need no lock
- Hold a lock only for the command itself, never across thinking or editing. Release it before waiting on anything else
- Prefer instruction counts to wall time; wall time is meaningless under this load
