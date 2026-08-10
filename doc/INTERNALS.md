# linearr internals

The parts of this project that matter when you are changing it rather than
using it: where the code lives, the rules it already follows, how to build and
test it, and how it behaves on Windows. Split out of README.md, which is for
somebody deciding whether to use the program.

## Layout

```
c/                the C sources and headers; nothing else lives here
  main.c          CLI front end: options, one case or a stream, print the result
  process.c/.h    THE slot: score a case (process), fit from a CSV (process_train)
  los.c/.h        the model: the schema, the coefficient tables, predict + trim
  regress.c/.h    ordinary least squares by accumulated normal equations
  qr.c/.h         the same fit by Givens rotations, without squaring (--qr)
  diag.c/.h       reads the residuals: curvature, and error that grows
  canon.c/.h      NIST reference datasets and their certified values
  csv.c/.h        bounded CSV: read a line, split it in place
  resolve.c/.h    find a data file: the current directory, then beside the binary
  common.c/.h     safe primitives: die(), debug(), xmalloc(), xstrdup()
  utils.c/.h      bounded string helpers (rtrim/ltrim)
  hash.c/.h       generic string -> void* hash table (backs the groups)
  constants.h     buffer sizes (the TERM ceiling lives in regress.h / los.h)
  tests.c         in-place unit tests (make ut)
java/             the second implementation; Regress.java mirrors regress.c
example/          all the data: anscombe.csv (the textbook quartet), the NIST
                  sets, routes.csv (why groups exist), the example model, and
                  the files the teaching sections run
bench/            the other languages' versions, and the coefficient comparator
tests/cli.sh      black-box tests: the binary through a shell and a pty (make cliut)
scripts/scale.sh  measures the memory claim, and fits every group to check it
scripts/bench.sh  the same fit in C, Java, Python, awk and R, answers checked first
scripts/java-check.sh  fits every example with both implementations and diffs
scripts/r-check.sh     the same against R's lm(), which uses a different method
scripts/readme-check.py runs every transcript in this file and diffs it
scripts/hooks/    pre-push: the sanitizers, before anything reaches the remote
Makefile          the build
```

Code under `c/` and `java/`, data at the top level, and no configuration
directory: every setting is an option.


## Style

The rules the code already follows, so new code matches:

- **C99, warning-free.** Clean under `-std=c99 -Wall -Wextra`; a warning is a
  defect. `make pedantic` is the stricter gate.
- **A pinned term is marked.** A coefficient the data could not identify is
  written as 0, and so is an estimated no-effect. The fit therefore
  emits a `# pinned <group>: constant ... collinear ...` line beside the row, so
  the distinction survives a redirect. It is a comment, so the table still reads
  straight back into the scorer.
- **Stack first; the heap is enumerated.** Rows are processed one at a time
  into fixed-size buffers; nothing on the row path allocates. Three things do
  allocate, each bounded by the model and never by the data, and each freed on
  every path: the coefficient table (`los.c`), the group index (`hash.c`), and
  one accumulator per group while `-t` fits them all (`process.c`). That is the
  complete list, and it is kept short enough to check.
- **Bounded strings only.** `snprintf` always; never `strcpy`/`strcat`/`sprintf`,
  except the checked copy into a fixed buffer, where the guard sits on the
  line above and returns rather than truncating. Sizes come from `constants.h`.
- **Checked allocation.** `xmalloc`/`xstrdup` never return NULL.
- **Functions, not macros.** `die`, `debug`, `xmalloc` are functions, so
  they type-check and are greppable.
- **Modules return, the CLI exits.** A module returns `0`/`-1`; only `main`
  terminates. Single exit via `goto cleanup` where a function holds a file.
- **One concept per file**, `static` for anything module-private,
  `const`-correct, `size_t` for sizes.
- **Tests in place.** `make ut` is wired; a feature ships with a `CHECK`. What a
  unit test structurally cannot reach (a terminal on stdin, exit codes, which
  stream a message went to) belongs in `tests/cli.sh`.
- **Sanitizer-clean.** `make ut-asan` and `make ut-ubsan` before tagging; CI and
  the pre-push hook run both.
- **Discarded return values carry a `(void)` cast** (MISRA 17.7). `main` checks
  `ferror(stdout)` once at the end because the individual writes are unchecked;
  the casts record that this was intended.
- **A comment is a claim.** Header comments, source comments, the Makefile, and
  the usage text go stale exactly like a README. When behaviour changes they move
  with it. See `AGENTS.md`.

The full rationale, stack-first and bounded-memory to the avionics and
medical-device standard (NASA Power of Ten, MISRA C:2012 rule 21.3), is written
up in [ais](https://github.com/Anode1/ais), in
[`doc/dev/STYLE.md`](https://github.com/Anode1/ais/blob/main/doc/dev/STYLE.md).

## Build and test

    make            # build ./linearr
    make check      # all five gates below; run this before a commit
    make ut         # unit tests, in place
    make cliut      # the binary driven through a shell and a pty
    make readme     # every transcript in this file, run and diffed
    make java       # both implementations fit every example; the output is diffed
    make r          # the same against R's lm(), which uses a different method
    make ut-asan    # the tests under AddressSanitizer
    make ut-ubsan   # the tests under UndefinedBehaviorSanitizer
    make pedantic   # strict warnings (-pedantic -Wshadow -Wstrict-prototypes ...)
    make debug      # -g -O0
    make hooks      # run every gate and both sanitizers before each git push
    make clean

Each gate exists because something got past the others.

`java` and `r` skip themselves where there is no JDK or no R, and say so rather
than failing. That is deliberate, and it has a consequence worth knowing: CI
runs on machines without R, so a green badge does not mean `lm()` was consulted.
The log says `R: no Rscript, skipped`. `make hooks` is where that gap closes,
on a machine that has R.

## Windows

The program is C99 plus a few POSIX functions. There are two routes, and the
first is the one to take.

**WSL, Microsoft's built-in Linux.** Nothing here is modified for it: it is the
same build this README describes, because WSL is Linux.

    1. Open PowerShell as Administrator and run:  wsl --install
    2. Restart, then choose a username and password when Ubuntu starts.
    3. In the Ubuntu window:
         sudo apt update && sudo apt install -y build-essential git
    4. git clone https://github.com/Anode1/linearr && cd linearr && make
    5. ./linearr -t example/routes.csv

Your Windows files are visible from inside WSL under `/mnt/c`, so a spreadsheet
at `C:\Users\you\data.csv` is `/mnt/c/Users/you/data.csv`. WSL needs
administrator rights to install, which some managed machines do not give.

**MSYS2 or Cygwin**, if WSL is not available: install either, add their `gcc`
and `make` packages, and run `make` in their terminal. Both produce a program
that needs their DLL beside it. MSYS2's MINGW64 shell produces a standalone
`.exe`; that build is compiled and linked here but has not been run, so treat it
as untested rather than supported.

There is no Visual Studio build. `getopt_long` would have to be bundled and the
`Makefile` replaced, and `tests/cli.sh` cannot run without a shell, so a build
that skipped it would ship with the black-box tests unrun.

**Saving CSV from Excel.** Choose plain "CSV", not "CSV UTF-8": the UTF-8 form
begins with three invisible bytes. They are skipped now, but older exports of
your own files may already carry them elsewhere. If your Excel writes semicolons
rather than commas, which it does wherever the comma is the decimal mark, save
as comma-separated; this program reads commas only, and says so if it finds
semicolons. Line endings are not a problem: CRLF files are read correctly.

## Cutting a release

The version lives in ONE place: the git tag. `make` stamps it in with
`git describe --tags`, `--version` prints it, and `scripts/dist.sh` reads it
back off the binary rather than out of a source file. A build outside a
checkout reports `0.0.0-dev`. Nothing has to be edited to bump it, and nothing
can be tagged at one version while carrying another, which is what a hand-
edited `#define` allowed.

    sh scripts/release-notes.sh 0.5.0 --write   # scaffold from the git log
    $EDITOR CHANGELOG.md                        # curate it
    git commit -am "Version 0.5.0"
    git push
    git tag -a v0.5.0 -m "0.5.0" && git push origin v0.5.0

The tag is what triggers `.github/workflows/release.yml`: four platforms built,
unit-tested, made to fit Longley on their own runner, packaged with a SHA-256
and attached to the release. The body is the `CHANGELOG.md` section for that
version.

The scaffolder is borrowed from the kul project. Commit subjects are not
release notes and the generated entry needs editing, but generating it first
means the entry records what happened rather than what somebody remembered.

## Build ceilings, stack and footprint

The default build takes **256 terms** and any number of groups. That ceiling is
what decides the fitter's footprint, and you set it at build time:

    make CPPFLAGS='-DREGRESS_MAX_VARS=32 -DLOS_MAX_VARS=32'

| ceiling | one group holds | note |
| --- | --- | --- |
| 32 terms | 13.1 KB | |
| 64 | 41.9 KB | |
| 128 | 147 KB | |
| 256 (default) | 550 KB | |
| 510 | 2.1 MB | the maximum; needs a rebuild to measure, and above it raise `CSV_MAX_FIELDS` too |

Those are `./linearr --footprint N 1`, which is the figure the program
allocates rather than one written down beside it. An earlier version of this
table was about twice each of them, because it counted the fitter's matrix
twice.

Three things that table does **not** cover:

- The fitter's matrices live in **static storage, not on the stack**, so the
  ceiling costs no stack at all and cannot overflow one.
- The **stack** requirement is about **190 KB**, and it is the line buffers in
  `constants.h`, which are derived from the ceiling rather than fixed. Measured
  at the default: it runs under `ulimit -s 192` and fails under 160. Setting the
  ceiling is all a small target needs, and it pays twice: a
  `-DLOS_MAX_VARS=32 -DREGRESS_MAX_VARS=32` build runs under `ulimit -s 64`.
- Fitting **every** group in one pass holds one accumulator per group. That is
  the fitter, the coefficients and the residual-check block, and `linearr
  --footprint TERMS GROUPS` prints it: 8.4 MB for 580 groups of 35 terms, which
  `scripts/scale.sh` then measures at 8.5 MB. It is still never a function of
  how many rows you feed it.

`scripts/scale.sh` exists to falsify the memory claim rather than repeat it: it
fits the same model over row counts an order of magnitude apart and prints peak
RSS for each. If those numbers tracked the data, the claim would be wrong and
this section would have to change. Run at the shape the original production
model had (35 terms, 580 groups), output verbatim:

    $ sh scripts/scale.sh 35 580 10000 100000
    linearr scale check: 35 terms, 580 groups

    generating training data (10000 and 100000 rows) ... done (816K, 8.0M)
    FIT: the same 35-term model, 10000 rows then 100000:
      rows         seconds  peak RSS (KB)
      10000        0.01 2432
      100000       0.12 2432
      ^ RSS should be flat: 10x the data, the same memory.

    generating 100000 rows spread over 580 groups ... done
    FIT ALL: the same rows, one line per group:
      groups       seconds  peak RSS (KB)
      1            0.12 2432
      580          0.14 11136
      ^ this one is NOT flat, and should not be: the difference is the
        per-group figure below, times 580.

    generating a 580-group table and cases ... done
    SCORE: 100000 cases against 580 groups:
      cases        seconds  peak RSS (KB)
      100000       0.07 3584

    Memory grows with the GROUPS and not with the rows. What that costs here:
      35 terms, 580 groups

      fitting, -t, one accumulator per group
        per group   15232 bytes
        in total    8.4 MB

      scoring, a loaded coefficient table
        per group   2064 bytes
        in total    1.1 MB

      The scoring figure does not move with the term count: the
      coefficient array is sized at this build's ceiling of 256, so a
      small model pays for a large one. The fitting figure does move.

      Neither depends on the number of ROWS:
      the same figures cover a thousand rows and a trillion.

Ten times the data, the same memory: that is the first pair of rows. The second
pair is the part a memory claim usually omits. Fitting 580 groups instead of one
costs 8.5 MB more, and the per-group figure printed underneath predicts 8.4 MB,
so the script measures the claim rather than restating it. Time scales with the
number of rows, memory with the number of groups.
