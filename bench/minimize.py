#!/usr/bin/env python
"""Shrink a Nix file on which two parse-bench binaries disagree

usage: minimize.py <file> <old-bench> <new-bench>
"""
import os
import subprocess
import sys
import tempfile

src, old, new = sys.argv[1:4]
tmp = tempfile.mkdtemp()
case = os.path.join(tmp, "case.nix")
lst = os.path.join(tmp, "list.txt")
with open(lst, "w") as f:
    f.write(case + "\n")


def result(binary, data):
    with open(case, "wb") as f:
        f.write(data)
    dump = os.path.join(tmp, "dump.txt")
    subprocess.run([binary, lst, "1", "--dump", dump], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   env=dict(os.environ, GC_INITIAL_HEAP_SIZE="256M"))
    return open(dump, "rb").read()


def differs(data):
    return result(old, data) != result(new, data)


data = open(src, "rb").read()
assert differs(data)
chunk = len(data) // 2
while chunk >= 1:
    i = 0
    progress = False
    while i < len(data):
        cand = data[:i] + data[i + chunk:]
        if differs(cand):
            data = cand
            progress = True
        else:
            i += chunk
    if not progress:
        chunk //= 2
print(repr(data))
print("old:", result(old, data).decode("utf-8", "replace"))
print("new:", result(new, data).decode("utf-8", "replace"))
