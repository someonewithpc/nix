# perf.py <label> <nix-prefix> <hello|nixos|sys>: instructions:u and peak RSS of one
# run. The result goes to out-<label>-<workload>.txt in the current directory
import resource, subprocess, sys, os
PERF = "/nix/store/hln3gq5s0pajpqlwcffqadzxglqfhx29-perf-linux-7.3.4/bin/perf"
NP = "/nix/store/mf1n715j8c2pvz6cwzajhhfbmjqanmi7-source"
W = os.path.dirname(os.path.abspath(__file__))
label, p, wl = sys.argv[1:4]
nix = [p + "/bin/nix", "--extra-experimental-features", "nix-command flakes", "eval", "--read-only", "--no-eval-cache", "--raw"]
cmd = {
    "hello": nix + ["-f", NP, "hello.drvPath"],
    "nixos": nix + ["--impure", "--expr", f'import {W}/../scripts/nixos.nix {{ nixpkgs = "{NP}"; }}'],
    "sys": nix + ["git+file:///etc/nixos?rev=19709e05b7ef8658a1a37186f36abc91c917ea3e#nixosConfigurations.beryllium.config.system.build.toplevel.drvPath"],
}[wl]
env = dict(os.environ, LD_LIBRARY_PATH=p + "/lib")
with open(f"out-{label}-{wl}.txt", "w") as out:
    r = subprocess.run([PERF, "stat", "-x,", "-e", "instructions:u", "--"] + cmd, stdout=out, stderr=subprocess.PIPE, text=True, env=env)
if r.returncode:
    print(r.stderr[-2000:]); sys.exit(1)
ins = [l for l in r.stderr.splitlines() if "instructions" in l][0].split(",")[0]
rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
print(f"{label} {wl} {int(ins)/1e9:.3f}G {rss/1024:.0f}MB", flush=True)
