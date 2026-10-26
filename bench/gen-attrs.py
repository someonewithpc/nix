#!/usr/bin/env python
# gen.py <out-dir> <count> <old-bench> <new-bench> [seed]: random attribute
# sets built from a small pool of names, so that paths merge, sets nest and
# duplicates happen; compare the dumps of both parsers
import os
import random
import subprocess
import sys


out, count, old, new = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
seed = int(sys.argv[5]) if len(sys.argv) > 5 else 1
rng = random.Random(seed)

pool = ["a", "b", "c", "x", "y", "z", "name", "src", "version", "meta", "or", "__overrides", '"q"', '"${"d"}"']


def name():
    return rng.choice(pool)


def value(depth):
    r = rng.random()
    if depth < 3 and r < 0.35:
        return attrs(depth + 1)
    if r < 0.45:
        return "let " + binds(depth + 1, rng.randint(1, 4)) + " in x"
    return str(rng.randint(0, 9))


def binds(depth, n):
    parts = []
    for _ in range(n):
        r = rng.random()
        if r < 0.1:
            parts.append("inherit " + " ".join(name().strip('"') for _ in range(rng.randint(1, 3))) + ";")
        elif r < 0.18:
            parts.append("inherit (s) " + " ".join(name().strip('"') for _ in range(rng.randint(1, 3))) + ";")
        else:
            path = ".".join(name() for _ in range(rng.choice([1, 1, 1, 2, 2, 3])))
            parts.append(f"{path} = {value(depth)};")
    return " ".join(parts)


def attrs(depth):
    big = rng.random() < 0.05
    n = rng.randint(30, 120) if big else rng.randint(0, 6)
    if big:
        # Mostly distinct names, so a big set gets past the merge threshold
        global pool
        saved = pool
        pool = saved + [f"k{i}" for i in range(200)]
        b = binds(depth, n)
        pool = saved
    else:
        b = binds(depth, n)
    return ("rec " if rng.random() < 0.2 else "") + "{ " + b + " }"



def parse_dump(path):
    entries = {}
    cur = None
    buf = []
    with open(path, "rb") as f:
        for line in f:
            if line.startswith(b"=== "):
                if cur is not None:
                    entries[cur] = b"".join(buf)
                cur = line[4:].rstrip(b"\n").decode()
                buf = []
            else:
                buf.append(line)
    if cur is not None:
        entries[cur] = b"".join(buf)
    return entries


os.makedirs(out, exist_ok=True)
names = []
for k in range(count):
    p = os.path.join(out, f"g{seed}-{k}.nix")
    with open(p, "w") as f:
        f.write(attrs(0) + "\n")
    names.append(p)
lst = os.path.join(out, f"list-{seed}.txt")
with open(lst, "w") as f:
    f.write("\n".join(names) + "\n")


env = dict(os.environ, GC_INITIAL_HEAP_SIZE="1G")
dumps = {}
for tag, binary in (("old", old), ("new", new)):
    dump = os.path.join(out, f"dump-{tag}-{seed}.txt")
    subprocess.run([binary, lst, "1", "--dump", dump], env=env, check=True, stdout=subprocess.DEVNULL)
    dumps[tag] = parse_dump(dump)
bad = [n for n in names if dumps["old"].get(n) != dumps["new"].get(n)]
errors = sum(1 for n in names if dumps["old"].get(n, b"").startswith(b"ERROR"))
print(f"{count} files, {errors} rejected by the old parser, {len(bad)} differ")
for n in bad[:10]:
    print("---", n)
    print("old:", dumps["old"].get(n, b"<missing>")[:300].decode("utf-8", "replace"))
    print("new:", dumps["new"].get(n, b"<missing>")[:300].decode("utf-8", "replace"))
