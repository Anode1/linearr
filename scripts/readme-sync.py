#!/usr/bin/env python3
"""readme-sync.py: rewrite every README transcript from what the program prints.

The companion to readme-check.py. The check refuses stale output; this produces
the correct output, so that fixing a transcript is never again a matter of
retyping numbers by hand. Three reviews found stale transcripts here and the
third found that hand-made corrections had silently not applied at all.

    python3 scripts/readme-sync.py            # rewrite README.md in place
    python3 scripts/readme-check.py           # then verify

Blocks whose expected output contains a line of "..." are left alone: those are
deliberately abridged and a machine cannot know where to cut.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from importlib import import_module
chk = import_module("readme-check")


def main(path):
    full = os.path.join(chk.ROOT, path)
    text = open(full).read()
    lines = text.split("\n")
    edits = []

    for cmd, expected, start in chk.blocks(text):
        if not expected:
            continue
        if not chk.runnable(cmd):
            continue
        if any(e.strip() == "..." for e in expected):
            continue                              # abridged on purpose
        actual = chk.run(cmd)
        ok, _ = chk.compare(expected, actual)
        if ok:
            continue
        indent = " " * (len(lines[start - 1]) - len(lines[start - 1].lstrip()))
        edits.append((start, len(expected), [indent + a for a in actual]))

    for start, n, new in reversed(edits):         # back to front, so indices hold
        lines[start:start + n] = new
    open(full, "w").write("\n".join(lines))
    print("readme-sync: rewrote %d transcript%s" % (len(edits), "" if len(edits) == 1 else "s"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "README.md"))
