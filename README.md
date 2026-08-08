# linearr -- least squares in C, fitted from a CSV and scored one row at a time

A small C99 program that fits `y = b0 + b1*x1 + ... + bp*xp` from a CSV and then
uses the fitted line to score new rows. No dependencies, no runtime to install,
no build framework: a stock C compiler and `make`.

Two properties are the reason it exists rather than being one more linear
regression:

- **The terms are not compiled in.** The header line of your CSV names them, so
  adding a term to the polynomial is adding a column to a file. Nothing to edit,
  nothing to rebuild.
- **Memory is a function of the model, not of the data.** Observations are
  accumulated into the normal equations one row at a time and then forgotten, so
  ten training rows and ten million are fitted in the same memory. Nothing on
  the data path allocates. There is a script that tries to falsify this and
  prints the numbers: see [Scale](#scale).

The worked example is hospital length of stay -- a prediction per case-mix
group, plus that group's *trim point*, the day count past which a stay stops
being typical. That is the shape the example data has; the program has no idea
what a hospital is.

## Build and run

    make            # build ./linearr
    make check      # both test gates -- run this before a commit
    make ut         # the in-place unit tests
    make cliut      # black-box: the binary driven through the shell
    make ut-asan    # the tests under AddressSanitizer
    make ut-ubsan   # the tests under UndefinedBehaviorSanitizer
    make pedantic   # strict warnings (-pedantic -Wshadow -Wstrict-prototypes ...)
    make debug      # -g -O0
    make hooks      # run the sanitizers before every git push
    make clean

Score a case -- a group, then one value per term:

    $ ./linearr "001,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0"
    001 prediction=19.9611 trim=46.5

Score a whole file (it is a filter, so it belongs in a pipeline):

    $ ./linearr < cases.csv > scored.txt

Fit the coefficients from training data -- rows of `GROUP,VALUE,<the terms>`:

    $ ./linearr -t example/train.csv -g 001
    fit: 17 rows, R2=1.0000, 17 terms unidentified and set to 0, df=9
    GROUP,Intercept,Cardioversion,Cell_saver,...
    001,6.4832,6.0308,0.0000,...

Standard output is a complete coefficient file and standard error is the
commentary, so the loop closes with a redirect:

    $ ./linearr -t train.csv -g 001 > conf/coefficients.csv
    $ ./linearr "001,1,0,0,..."          # scored against what you just fitted

`./linearr -h` prints the options; `-d` traces to stderr.

## The terms are yours

`example/simple-train.csv` is the same program with a schema nobody wrote any
code for -- minutes on the road, from distance and stops:

    GROUP,MINUTES,km,stops
    A,5.0,0,0
    A,30.0,10,0
    ...

    $ ./linearr -t example/simple-train.csv -g A
    fit: 7 rows, R2=1.0000, df=4
    GROUP,Intercept,km,stops
    A,5.0000,2.5000,1.5000

Two terms instead of twenty-four, and the only thing that changed was the file.

## Scale

The default build takes **256 terms** and any number of groups. That ceiling is
the only number that decides the program's memory, and you set it at build time:

    make CPPFLAGS='-DREGRESS_MAX_VARS=32 -DLOS_MAX_VARS=32'    # ~17 KB of fitter

| ceiling | fitter footprint |
| --- | --- |
| 32 terms | ~17 KB |
| 64 | ~68 KB |
| 128 | ~267 KB |
| 256 (default) | ~1.1 MB |
| 512 | ~4.2 MB |

`scripts/scale.sh` exists to falsify the memory claim rather than repeat it: it
fits the same 200-term model over row counts an order of magnitude apart and
prints peak RSS for each. If those numbers tracked the data, the claim would be
wrong and this section would have to change. Measured on the author's machine:

    $ sh scripts/scale.sh 200 500 10000 100000

    FIT -- the same 200-term model, 10000 rows then 100000:
      rows         seconds  peak RSS (KB)
      10000        0.50     3072
      100000       5.70     2816          <- 10x the data, 4 MB -> 40 MB on disk

    SCORE -- 100000 cases against 500 groups:
      100000       1.42     3072

Time scales with the rows, as it must. Memory does not move. The coefficient
table is the only thing that grows with the problem -- 500 groups x 201 doubles,
about 785 KB, held once -- and it is bounded by the number of groups, never by
the number of cases scored.

## What it does when the data cannot answer

A term the sample cannot identify -- a column that never varies, or one that
duplicates another -- has no least-squares answer, and the normal equations are
singular there. Rather than fail the fit or return a number the data does not
support, such a term is **pinned to exactly 0** and the rest are fitted around
it, and the count is reported. This is the ordinary case with indicator columns:
a group that never sees a given intervention simply has no evidence about it.

The fit summary also reports **df**, the residual degrees of freedom. At zero,
the line passes through every training row by construction and `R2` is 1 no
matter what the data says; the program says so out loud rather than letting a
meaningless 1.0000 read as success.

## Configuration

`system.properties` -- `key = value`, `#` comments. Every key has a built-in
default, so the file may be absent.

| key | meaning |
| --- | --- |
| `coef.file` | the fitted model: `GROUP,Intercept,<one column per term>` |
| `trim.file` | `GROUP,trim_addition`; set it empty for none |
| `predict.scale` | digits the prediction is rounded to (default 4) |
| `trim.scale` | digits the trim point is rounded to (default 1) |

Rounding is half away from zero, not `printf`'s half to even, and it is part of
the answer rather than presentation: the trim point is built on the *rounded*
prediction, because the published figure is what the next step is entitled to
use.

## The example data is synthetic

`conf/coefficients.csv`, `conf/trim_additions.csv` and both files under
`example/` are **made up** -- generated so that fitting `example/train.csv`
returns exactly the coefficients in `conf/coefficients.csv`, which is what makes
the fitter testable against a known answer. They are fitted to nothing and mean
nothing. Point `coef.file` at your own table, or produce one with `-t`, before
any number here is worth reading. No real data is distributed with this project.

## Layout

```
main.c            CLI front end: options, one case or a stream, print the result
process.c/.h      THE slot: score a case (process), fit from a CSV (process_train)
los.c/.h          the model: the schema, the coefficient tables, predict + trim
regress.c/.h      ordinary least squares by accumulated normal equations
csv.c/.h          bounded CSV: read a line, split it in place
common.c/.h       safe primitives: die(), debug(), xmalloc(), xstrdup()
utils.c/.h        bounded string helpers (rtrim/ltrim)
params.c/.h       config: load system.properties, params_get("key")
hash.c/.h         generic string -> void* hash table (backs params and the groups)
constants.h       tunable sizes, all of them
tests.c           in-place unit tests (make ut)
tests/cli.sh      black-box tests: the binary through a shell and a pty (make cliut)
conf/             the example model (synthetic)
example/          two training files with different schemas (synthetic)
scripts/scale.sh  measures the memory claim at 200 terms and 500 groups
scripts/hooks/    pre-push: the sanitizers, before anything reaches the remote
Makefile          the build
```

## Where this is the right tool, and where it is not

**It fits well when:**

- there is no Python and there is not going to be -- an embedded target, a
  locked-down clinical or lab machine, a container you want under a megabyte, a
  build with no package manager;
- the training file is much larger than the machine's memory, and you would
  rather stream it once than hold a matrix of it (a 40 MB file and a 4 MB file
  fit in the same 3 MB -- see [Scale](#scale));
- a C or C++ codebase needs a fit without taking on GSL, LAPACK, or a build
  system to go with them;
- the coefficients are *published* -- a rate, a tariff, an expected value
  someone else's process consumes -- so the rounding and the exact arithmetic
  are part of the contract and have to be reproducible digit for digit;
- scoring is a pipeline stage: one row in, one line out, exit code and stderr
  behaving the way the rest of your shell does;
- you are teaching what a least-squares fit actually is, and want the whole of
  it readable in an afternoon (`regress.c` is about 100 lines).

**Reach for something else when:** you need regularization (ridge, lasso,
elastic net), categorical encoding, missing-value handling, cross-validation, or
inference -- standard errors, confidence intervals, p-values. None of that is
here. `scikit-learn` and `statsmodels` do all of it well, and GSL
(`gsl_multifit_linear`) or LAPACK (`dgels`) give you a fitted line in C with more
numerical machinery behind it than this has.

**One honest numerical caveat.** Accumulating `X'X` and solving it is what makes
the memory bound possible, and it costs conditioning: forming the normal
equations squares the condition number of the design, so a badly scaled or
near-collinear problem loses roughly twice the digits a QR or SVD solve would.
For indicator columns and modestly scaled data -- what this is built for -- it is
not the limiting factor; the pinning above handles the singular cases outright.
If your design is ill-conditioned, use a QR-based fit. A streaming Householder
QR would keep the memory bound and fix the conditioning, and is the obvious next
thing to build here.

## Style

The rules the code already follows, so new code matches:

- **C99, warning-free.** Clean under `-std=c99 -Wall -Wextra`; a warning is a
  defect. `make pedantic` is the stricter gate.
- **Stack first.** Rows are processed one at a time into fixed-size buffers, and
  peak memory is computable by hand from the structs. The one sanctioned heap is
  the coefficient table in `los.c`, bounded by the number of groups and freed on
  every path.
- **Bounded strings only.** `snprintf` always; never `strcpy`/`strcat`/`sprintf`
  -- except the checked copy into a fixed buffer, where the guard sits on the
  line above and returns rather than truncating. Sizes come from `constants.h`.
- **Checked allocation.** `xmalloc`/`xstrdup` never return NULL.
- **Functions, not fragile macros.** `die`, `debug`, `xmalloc` are functions, so
  they type-check and are greppable.
- **Modules return, the CLI exits.** A module returns `0`/`-1`; only `main`
  terminates. Single exit via `goto cleanup` where a function holds a file.
- **One concept per file**, `static` for anything module-private,
  `const`-correct, `size_t` for sizes.
- **Tests in place.** `make ut` is wired; a feature ships with a `CHECK`. What a
  unit test structurally cannot reach -- a terminal on stdin, exit codes, which
  stream a message went to -- belongs in `tests/cli.sh`.
- **Sanitizer-clean.** `make ut-asan` and `make ut-ubsan` before tagging; CI and
  the pre-push hook run both.
- **A comment is a claim.** Header comments, source comments, the Makefile, and
  the usage text go stale exactly like a README. When behaviour changes they move
  with it. See `AGENTS.md`.

The full rationale -- stack-first and bounded-memory to the avionics and
medical-device standard (NASA Power of Ten, MISRA C:2012 rule 21.3) -- is written
up in [ais](https://github.com/Anode1/ais), in
[`doc/dev/STYLE.md`](https://github.com/Anode1/ais/blob/main/doc/dev/STYLE.md).

## See also

- [ais](https://github.com/Anode1/ais) -- the associative-memory engine these
  conventions come from.
- [aisconfig](https://github.com/Anode1/aisconfig) -- the C project template
  this started from.

## License

GNU GPL v2 or later; see `LICENSE`.
