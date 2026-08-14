# How the documents are written

`AGENTS.md` says a comment is a claim and goes stale like a README. This file is
the other half of that: how the claims are phrased.

`make readme` already keeps every transcript honest by running it. Nothing keeps
the prose around the transcripts honest, and it had drifted into one voice
across seven documents, with the same fact stated three ways in three files.

## Bold a term, not a sentence

The reference documents use a definition-list style, and it works:

    **No quoting.** Fields are split on commas and nothing else.
    **CRLF is fine.** A file saved on Windows reads normally.

Those are terms. What had grown beside them was a sentence in bold, standing in
for a topic sentence:

    before   **A file written in another column order does not fail.** It fits, ...
    after    A file written in another column order does not fail. It fits, ...

If the bold text is a complete assertion, it is a sentence and wants no bold.
If it names the thing the paragraph is about, keep it.

## Titles name, they do not argue

`# File formats, groups and refusals`, not `..., and what the reader refuses`.
`## Two solvers`, not `## Two solvers, and how accurate each one is`. The
argument belongs in the first line of the section, where a reader who chose the
section will actually read it.

## Cut the decorative contrast

`X, not Y` and `rather than` carry weight when the wrong reading is one a reader
would otherwise take: *the goal is the second column, not the first* earns it,
because getting that wrong fits cleanly and exits 0. `--footprint prints the
figure rather than leaving you to trust a sentence` does not; the second half
only flatters the first. Cut those.

## One home per fact

The condition number that matters is the centered, column-scaled one. That was
corrected in the README, and `doc/NUMERICS.md` went on saying "the default
solves `X'X`" for a year afterwards, which is precisely the failure `AGENTS.md`
records under *when you correct a claim, grep for the other places that make
it*. A figure belongs in the document that owns it; the others link.

Test counts are the same hazard. `AGENTS.md` carried 118, 144 and 145 in three
paragraphs describing one suite that now runs 348. Where a number is there to
tell a story about the past, say so; where it describes the present, take it
from `make check`.

## Sections stop when the information stops

No closing maxim. If the last sentence of a section generalises instead of
informing, delete it.

## Registers differ

`README.md` persuades. `doc/FORMATS.md`, `doc/NUMERICS.md`, `doc/BENCHMARKS.md`
and `doc/DIAGNOSTICS.md` are references and should read flatter and duller than
it does. If every document in the repository is equally interesting, they read
as one essay written in one sitting.
