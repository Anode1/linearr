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
