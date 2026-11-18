#!/usr/bin/env python
# Usage: nix ... --log-format internal-json -v 2>&1 | nix-timeline.py stamp > log.jsonl
#        nix-timeline.py report log.jsonl
# stamp prefixes each @nix line with a monotonic time in ms; report prints
# each activity with its start, end and duration, grouped by type
import json
import sys
import time

TYPES = {0: "unknown", 100: "copyPath", 101: "fileTransfer", 102: "realise",
         103: "copyPaths", 104: "builds", 105: "build", 106: "optimiseStore",
         107: "verifyPaths", 108: "substitute", 109: "queryPathInfo",
         110: "postBuildHook", 111: "buildWaiting", 112: "fetchTree"}


def stamp():
    t0 = time.monotonic()
    for line in sys.stdin.buffer:
        sys.stdout.write("%.3f %s" % ((time.monotonic() - t0) * 1000,
                                      line.decode(errors="replace")))
        sys.stdout.flush()


def report(path):
    acts = {}
    last = 0.0
    for line in open(path):
        ts, _, rest = line.partition(" ")
        last = float(ts)
        if not rest.startswith("@nix "):
            continue
        ev = json.loads(rest[5:])
        if ev["action"] == "start":
            acts[ev["id"]] = [float(ts), None, ev.get("type", 0), ev.get("text", ""),
                              ev.get("fields", [])]
        elif ev["action"] == "stop" and ev["id"] in acts:
            acts[ev["id"]][1] = float(ts)
    rows = sorted(acts.values(), key=lambda a: a[0])
    for start, end, ty, text, fields in rows:
        if ty in (0, 102, 104, 103):
            continue
        end = last if end is None else end
        what = text or " ".join(str(f) for f in fields)
        print("%9.1f %9.1f %8.1f  %-14s %s" % (start, end, end - start,
                                              TYPES.get(ty, ty), what[:110]))
    tot = {}
    for start, end, ty, _, _ in rows:
        end = last if end is None else end
        n, d = tot.get(ty, (0, 0.0))
        tot[ty] = (n + 1, d + end - start)
    print("\ntotal %.1f ms" % last)
    for ty, (n, d) in sorted(tot.items()):
        print("  %-14s %4d activities, %9.1f ms summed" % (TYPES.get(ty, ty), n, d))


if __name__ == "__main__":
    if sys.argv[1] == "stamp":
        stamp()
    else:
        report(sys.argv[2])
