#!/usr/bin/env python3
"""readme-check.py: run every command the README shows and diff its output.

Three separate reviews found stale transcripts in this README, and the third
found that fixes reported as done had never landed: the edits were made with
blind string replacement, none of them matched, and nobody checked. Prose can be
proof-read; a transcript has to be executed.

Every block of the form

        $ ./linearr ...
        <expected output>

is run from the repository root and compared line by line with what the program
prints. A line consisting only of "..." skips to the end of the block, for
transcripts that are deliberately abridged. Exit status is non-zero if any block
disagrees, so `make readme` is a gate and not a report.

Blocks run in the order they appear in the file, in one directory, so a block
may read a file an earlier block wrote: the README shows `--residuals r.csv`
and then sorts r.csv, and both are checked. The commands run are the program's
own and the few shell tools named in FOLLOW_ON; anything else in a transcript is
shown but not executed, since running arbitrary text out of a document is not
something a build should do.

Usage: python3 scripts/readme-check.py [README.md]
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROMPT = re.compile(r"^(\s*)\$ (.+)$")

# Commands that read what an earlier block wrote. Deliberately short.
FOLLOW_ON = ("sort ", "cat ", "head ", "wc ")


def runnable(cmd):
    return (cmd.startswith("./linearr") or cmd.startswith("sh scripts/")
            or cmd.startswith(FOLLOW_ON))


def blocks(text):
    """Yield (command, [expected lines], line number) for each transcript."""
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        m = PROMPT.match(lines[i])
        if not m:
            i += 1
            continue
        indent, cmd = m.group(1), m.group(2).strip()
        start = i + 1
        i += 1
        expected = []
        while i < len(lines):
            line = lines[i]
            if not line.strip():                 # blank ends the block
                break
            if PROMPT.match(line):               # next command ends it
                break
            if not line.startswith(indent):      # dedent ends it
                break
            expected.append(line[len(indent):])
            i += 1
        yield cmd, expected, start


def run(cmd):
    p = subprocess.run(cmd, shell=True, cwd=ROOT, capture_output=True, text=True,
                       timeout=300)
    # The program writes its summary to stderr and its table to stdout, and the
    # README shows them interleaved as a terminal would.
    return (p.stderr + p.stdout).rstrip("\n").split("\n")


def compare(expected, actual):
    """True if actual matches expected; '...' skips the rest of the block."""
    for n, want in enumerate(expected):
        if want.strip() == "...":
            return True, None
        if n >= len(actual):
            return False, "expected %d lines, got %d" % (len(expected), len(actual))
        if want.rstrip() != actual[n].rstrip():
            return False, "line %d:\n      README: %s\n      actual: %s" % (
                n + 1, want.rstrip(), actual[n].rstrip())
    return True, None


def main(path):
    text = open(os.path.join(ROOT, path)).read()
    checked = failed = 0
    for cmd, expected, lineno in blocks(text):
        if not expected:
            continue                             # a command shown without output
        if not runnable(cmd):
            continue                             # only our own commands are run
        checked += 1
        try:
            actual = run(cmd)
        except subprocess.TimeoutExpired:
            print("  TIMEOUT  %s:%d  %s" % (path, lineno, cmd))
            failed += 1
            continue
        ok, why = compare(expected, actual)
        if not ok:
            failed += 1
            print("  STALE  %s:%d  $ %s\n      %s" % (path, lineno, cmd, why))
    print("readme: %d transcripts checked, %d stale" % (checked, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "README.md"))
