#!/usr/bin/env python
"""Differential fuzzing of the Nix parser.

Mutates files from a corpus list, parses every mutant with an old and a new
parse-bench binary, and reports the mutants whose AST dump or error differs

usage: fuzz.py <corpus-list> <out-dir> <old-bench> <new-bench> [count] [seed]
"""
import os
import random
import subprocess
import sys

SNIPPETS = [
    "{", "}", "(", ")", "[", "]", ";", ":", "=", ",", ".", "?", "@", "...",
    "${", '"', "''", "'''", "''$", "''\\n", "or", "in", "let", "if", "then",
    "else", "rec", "inherit", "with", "assert", "//", "++", "==", "!=", "<",
    "<=", ">", ">=", "->", "&&", "||", "!", "-", "+", "*", "/", "x", "x'",
    "_a-b", "1", "0", "01", "1.5", ".5", "1.", "1e5", "1e999", "9999999999999999999999",
    "./p", "./p/", "../q/r", "a/b", "a/${x}", "/abs", "~/h", "~/${x}", "<p>",
    "<p/q>", "a:b", "http://x.y/z", "#c\n", "/* c */", "/** d */", "/**/",
    "\\", "$", "'", "~", "`", "%", "^", "&", "|", "\x00", "\xc3\xa9", "\r",
    "\r\n", "\n", " ", "\t", "__curPos", "x: x", "{ a, b ? 1, ... }: a",
    "{ a = 1; }", "inherit (x) y;", '"${x}"', "''${x}''", "f or [ ]",
]


def mutate(rng, data):
    n = len(data)
    op = rng.randrange(6)
    if n == 0:
        op = 2
    if op == 0:
        return data[: rng.randrange(n + 1)]
    if op == 1:
        i = rng.randrange(n)
        return data[:i] + data[i + rng.randint(1, 20):]
    if op == 2:
        i = rng.randrange(n + 1)
        return data[:i] + rng.choice(SNIPPETS).encode("latin-1", "replace") + data[i:]
    if op == 3:
        i = rng.randrange(n)
        j = min(n, i + rng.randint(1, 40))
        return data[:j] + data[i:j] + data[j:]
    if op == 4:
        i = rng.randrange(n)
        j = rng.randrange(n)
        return data[:i] + data[j : j + rng.randint(1, 10)] + data[i:]
    i = rng.randrange(n)
    return data[:i] + bytes([rng.randrange(256)]) + data[i + 1 :]


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


def main():
    corpus, out, old, new = sys.argv[1:5]
    count = int(sys.argv[5]) if len(sys.argv) > 5 else 2000
    seed = int(sys.argv[6]) if len(sys.argv) > 6 else 1
    rng = random.Random(seed)
    files = [p for p in open(corpus).read().split("\n") if p]
    small = [p for p in files if os.path.getsize(p) < 20000]
    os.makedirs(out, exist_ok=True)
    names = []
    for k in range(count):
        data = open(rng.choice(small), "rb").read()
        for _ in range(rng.randint(1, 3)):
            data = mutate(rng, data)
        name = os.path.join(out, f"m{seed}-{k}.nix")
        with open(name, "wb") as f:
            f.write(data)
        names.append(name)
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
    print(f"{count} mutants, {errors} rejected by the old parser, {len(bad)} differ")
    for n in bad[:20]:
        print("---", n)
        print("old:", dumps["old"].get(n, b"<missing>")[:400].decode("utf-8", "replace"))
        print("new:", dumps["new"].get(n, b"<missing>")[:400].decode("utf-8", "replace"))


main()
