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


# scripts/bench.sh and scripts/scale.sh are deliberately NOT run here. They
# measure a machine: their transcripts carry wall-clock seconds and peak RSS,
# which differ between runs on one box and differ more between boxes, so
# diffing them line by line reports a stale document every time the CPU is
# busy. They were already going undiffed by accident, through a parser that
# stopped at the first blank line; this says so on purpose instead.
#
# The claims inside them that ARE deterministic -- the per-group and total
# memory a shape costs -- are gated separately, by `./linearr --footprint`
# transcripts, which is why those exist as their own blocks.
MEASURES_A_MACHINE = ("sh scripts/bench.sh", "sh scripts/scale.sh")


def runnable(cmd):
    if cmd.startswith(MEASURES_A_MACHINE):
        return False
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
            if not line.strip():
                # A blank line INSIDE a transcript is output, not the end of
                # one. scale.sh and bench.sh both print blank lines between
                # their sections, and ending the block at the first one meant
                # scale.sh was compared for a single line while its forty lines
                # of numbers went undiffed -- and bench.sh, whose output starts
                # after a blank, yielded no expected lines at all and was
                # skipped entirely by the `if not expected` below. Both were
                # displayed as verified and checked by nothing, which is how
                # the --footprint figures in doc/INTERNALS.md came to disagree
                # with the binary in a block labelled "output verbatim".
                #
                # So look past the blanks: if the output resumes at this
                # block's indent and is not a new command, they belong to it.
                j = i
                while j < len(lines) and not lines[j].strip():
                    j += 1
                if (j < len(lines) and lines[j].startswith(indent)
                        and not PROMPT.match(lines[j])):
                    expected.extend([""] * (j - i))
                    i = j
                    continue
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


def main(*paths):
    checked = failed = 0
    for path in paths:
        checked, failed = check_one(path, checked, failed)
    print("readme: %d transcripts checked, %d stale" % (checked, failed))
    return 1 if failed else 0


def check_one(path, checked, failed):
    text = open(os.path.join(ROOT, path)).read()
    for cmd, expected, lineno in blocks(text):
        if not expected:
            continue                             # a command shown without output
        if not runnable(cmd):
            continue                             # only our own commands are run
        checked += 1
        # A transcript holding this checkout's own absolute path passes here and
        # nowhere else, so the gate would go green for the author and red for
        # every clone and for CI. That is worse than an unchecked transcript,
        # because it reads as verified. It happened: a -y example pasted a
        # file-not-found message carrying the author's home directory, and the
        # only machine `make check` passed on was the one it was written on.
        if ROOT in "\n".join(expected):
            failed += 1
            print("  MACHINE-SPECIFIC  %s:%d  $ %s\n"
                  "      the expected output contains this checkout's path (%s),\n"
                  "      so it can only pass here. Use a relative path, or an\n"
                  "      example that does not print one."
                  % (path, lineno, cmd, ROOT))
            continue
        try:
            actual = run(cmd)
        except subprocess.TimeoutExpired:
            print("  TIMEOUT  %s:%d  %s" % (path, lineno, cmd))
            failed += 1
            continue
        # A gate that skips itself where a tool is missing prints that it
        # skipped, and its transcript documents what it prints when the tool
        # IS there. Comparing the two on a machine without R or a JDK fails
        # for the wrong reason, which is how the README came to hold a block
        # that could only pass where R was installed.
        if any("skipped" in a and ("no Rscript" in a or "no JDK" in a
                                   or "not installed" in a) for a in actual):
            print("  SKIPPED  %s:%d  $ %s\n      %s"
                  % (path, lineno, cmd, actual[0]))
            continue
        ok, why = compare(expected, actual)
        if not ok:
            failed += 1
            print("  STALE  %s:%d  $ %s\n      %s" % (path, lineno, cmd, why))
    return checked, failed


if __name__ == "__main__":
    # Every document, not only the README: moving a transcript into doc/ must
    # not quietly move it out of the gate.
    args = sys.argv[1:]
    if not args:
        args = ["README.md"] + sorted(
            os.path.join("doc", f) for f in os.listdir(os.path.join(ROOT, "doc"))
            if f.endswith(".md"))
    sys.exit(main(*args))
