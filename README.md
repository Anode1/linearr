# linearr: fit a line to your data, in C, and keep the memory flat

Give it a CSV, get back the coefficients. Give it a case, get back a prediction.
No dependencies, no runtime to install, no build framework: a stock C compiler
and `make`. It streams the training file one row at a time, so a ten-row fit and
a ten-million-row fit cost the same memory.

    make
    ./linearr -t mydata.csv > model.csv       # fit every group in one pass
    ./linearr -c model.csv --no-trim A x=3    # score a case against it

## Build and run

    make            # build ./linearr
    make check      # both test gates; run this before a commit
    make ut         # the in-place unit tests
    make cliut      # black-box: the binary driven through the shell
    make ut-asan    # the tests under AddressSanitizer
    make ut-ubsan   # the tests under UndefinedBehaviorSanitizer
    make pedantic   # strict warnings (-pedantic -Wshadow -Wstrict-prototypes ...)
    make debug      # -g -O0
    make hooks      # run the sanitizers before every git push
    make clean

Fit a model from your own data and score against it, with no configuration file
anywhere. A training row is `GROUP,VALUE,<one column per term>`, and the header
names the terms:

    $ cat mine.csv
    GROUP,MINUTES,km,stops
    A,5.0,0,0
    A,30.0,10,0
    A,9.5,0,3
    A,34.5,10,3
    A,19.0,5,1
    A,58.0,20,2
    A,18.5,3,4

    $ ./linearr -t mine.csv > model.csv
    fit: 1 group, 7 rows, least df=4, worst cond=1

    $ cat model.csv
    GROUP,Intercept,km,stops
    A,4.999999999999999,2.5,1.4999999999999998

    $ ./linearr -c model.csv --no-trim A km=10 stops=3
    A prediction=34.5000 trim=34.5

`-t` fits **every group in the file**, one line each, in a single pass. Standard
output is a complete coefficient file and standard error is the commentary, so
the redirect above is the whole workflow.

Ask what a model expects:

    $ ./linearr --terms -c conf/coefficients.csv
    24 terms and 12 groups in conf/coefficients.csv
        1  Cardioversion
        2  Cell_saver
      ...
       17  icu_indicator
      ...

Score by naming the terms that are not zero; everything else is 0:

    $ ./linearr 001 Cardioversion=1 icu_indicator=1
    001 prediction=19.9611 trim=46.5

The same case as a row, every term in the table's column order. This is the form
read from stdin, so a file of cases round trips through a pipeline:

    $ ./linearr "001,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0"
    001 prediction=19.9611 trim=46.5

    $ printf '001,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0\n' > cases.csv
    $ ./linearr < cases.csv
    001 prediction=19.9611 trim=46.5

At two terms the row form is fine. At two hundred it is unusable, which is why
the named form exists and is what the rest of this README uses.

Files (`-c`, `--trim`, `coef.file`, `trim.file`, and `-t`'s argument) are
looked for in the current directory first, then beside the program, so an
installed `linearr` works from anywhere and your own table still wins where you
have one.

When something is wrong, the message says what:

    $ ./linearr 001 nosuchterm=1
    cannot score group '001': no term 'nosuchterm' in conf/coefficients.csv; run --terms to list them

`./linearr -h` prints the options; `-d` traces to stderr.

## Where this is the right tool, and where it is not

**It fits well when:**

- there is no Python and there is not going to be: an embedded target, a
  locked-down clinical or lab machine, a container you want under a megabyte, a
  build with no package manager;
- the training file is much larger than the machine's memory, and you would
  rather stream it once than hold a matrix of it (a 40 MB file and a 4 MB file
  fit in the same 3 MB; see [Scale](#scale));
- a C or C++ codebase needs a fit without taking on GSL, LAPACK, or a build
  system to go with them;
- the coefficients are *published* (a rate, a tariff, an expected value
  someone else's process consumes), so the rounding and the exact arithmetic
  are part of the contract and have to be reproducible digit for digit;
- scoring is a pipeline stage: one row in, one line out, exit code and stderr
  behaving the way the rest of your shell does;
- you are teaching what a least-squares fit actually is, and want the whole of
  it readable in an afternoon (`regress.c` is about 150 lines of code).

**Reach for something else when:** you need regularization (ridge, lasso,
elastic net), categorical encoding, missing-value handling, cross-validation,
weighted least squares, or inference: standard errors, confidence intervals,
prediction intervals, p-values. None of that is here, and neither is the
residual standard error a prediction consumer usually wants next. The worked
example is also a modelling choice worth naming: length of stay is a skewed,
non-negative, count-like response, and unweighted OLS on raw days is not the
standard treatment for it (a log transform or a Gamma GLM is). Nothing stops
this tool predicting a negative stay. `scikit-learn` and `statsmodels` do all of it well, and GSL
(`gsl_multifit_linear`) or LAPACK (`dgels`) give you a fitted line in C with more
numerical machinery behind it than this has.

**One numerical caveat.** Accumulating `X'X` and solving it is what makes
the memory bound possible, and it costs conditioning: forming the normal
equations squares the condition number of the design, so a badly scaled or
near-collinear problem loses roughly twice the digits a QR or SVD solve would.
For indicator columns and modestly scaled data (what this is built for) it is
not the limiting factor; the pinning above handles the singular cases outright.
If your design is ill-conditioned, use a QR-based fit. A streaming Householder
QR would keep the memory bound and fix the conditioning, and is the obvious next
thing to build here.

## The two things that make it different

- **The terms are not compiled in.** The header line of your CSV names them, so
  adding a term to the polynomial is adding a column to a file. Nothing to edit,
  nothing to rebuild; the same binary fits a 24-term model and a 2-term one.
- **Memory is a function of the model, not of the data.** Observations are
  accumulated into centered cross-products one row at a time and then forgotten.
  Nothing on the row path allocates. There is a script that tries to falsify
  this and prints the numbers: see [Scale](#scale).

The worked example is hospital length of stay: a prediction per case-mix
group, plus that group's *trim point*, the day count past which a stay stops
being typical. That is the shape the example data has; the program has no idea
what a hospital is.

## All of least squares, in three short pieces

Part of the point of this project is to show how little there is to a
least-squares fit once the libraries are out of the way. The model is a
polynomial in the terms, and predicting is one line of C:

    double y = m->intercept;
    for (i = 0; i < nvars; i++) y += m->b[i] * c->x[i];

Fitting is choosing the `b` that makes the squared error over the training rows
as small as it can be. Differentiate that error with respect to each `b` and set
the result to zero, and you get the *normal equations*, `(X'X) b = X'y`: one
equation per term, and no more. The useful thing about them is that `X'X` and
`X'y` are **sums over rows**, so a row contributes its piece and is never needed
again. That is why training is a loop with nothing accumulating in it but the
model:

    while ((n = csv_next(fp, line, sizeof line)) > 0) {   /* process.c, checks elided */
        los_parse_training(line, &c, &los);
        regress_add(&r, c.x, los);        /* fold this row in, then forget it */
    }
    regress_solve(&r, fit_beta, fit_scratch, &f);

`regress_add` is the sum, kept in centered form so that a large mean cannot
swamp a small variance. `regress_solve` scales the system so its diagonal is all
ones, runs Gauss-Jordan with partial pivoting, and scales the answer back. Any
term the data cannot identify shows up there as a pivot that is not there, and
is pinned to 0 rather than guessed at.

That is the whole method: a sum you can take one row at a time, and a small
dense solve at the end. `regress.c` is about 150 lines of code, 234 with the
comments, and is meant to be read in one sitting.

## The terms are yours

`example/simple-train.csv` is the same program with a schema nobody wrote any
code for: minutes on the road, from distance and stops:

    GROUP,MINUTES,km,stops
    A,5.0,0,0
    A,30.0,10,0
    ...

    $ ./linearr -t example/simple-train.csv -g A
    fit: 7 rows, R2=1.0000, df=4, cond=1
    GROUP,Intercept,km,stops
    A,4.999999999999999,2.5,1.4999999999999998

Two terms instead of twenty-four, and the only thing that changed was the file.

## What it does when the data cannot answer

A term the sample cannot identify (a column that never varies, or one that
duplicates another) has no least-squares answer, and the normal equations are
singular there. Rather than fail the fit or return a number the data does not
support, such a term is **pinned to exactly 0** and the rest are fitted around
it, and the count is reported. This is the ordinary case with indicator columns:
a group that never sees a given intervention simply has no evidence about it.

The fit summary also reports **df**, the residual degrees of freedom. At zero,
the line passes through every training row by construction and `R2` is 1 no
matter what the data says; the program says so out loud rather than letting a
meaningless 1.0000 read as success.

## Configuration

`system.properties`: `key = value`, `#` comments. Every key has a built-in
default, so the file may be absent.

| key | default | meaning |
| --- | --- | --- |
| `coef.file` | `conf/coefficients.csv` | the fitted model: `GROUP,Intercept,<one column per term>`. `-c` overrides it |
| `trim.file` | `conf/trim_additions.csv` | `GROUP,trim_addition`. Set it empty, or pass `--no-trim`, for none |
| `predict.scale` | 4 | digits the prediction is rounded to |
| `trim.scale` | 1 | digits the trim point is rounded to |

With no trim table the trim point simply equals the prediction; it is not a
separate quantity that failed to load. A trim file *named* in the config and
unreadable is an error; the built-in default merely being absent is not.

**Coefficients are written at full precision**, in the shortest form that reads
back as the same double, so `A,5,2.5,1.5` and `A,4.9999999999999991,...` are
both exactly what was fitted. `predict.scale` governs the prediction, not the
model: rounding coefficients to four decimals would write any effect below 5e-5
as zero and publish a different model from the one that was fitted.

Rounding is half away from zero, not `printf`'s half to even, and it is part of
the answer rather than presentation: the trim point is built on the *rounded*
prediction, because the published figure is what the next step is entitled to
use.

## Scale

The default build takes **256 terms** and any number of groups. That ceiling is
what decides the fitter's footprint, and you set it at build time:

    make CPPFLAGS='-DREGRESS_MAX_VARS=32 -DLOS_MAX_VARS=32'

| ceiling | fitter matrices | note |
| --- | --- | --- |
| 32 terms | ~17 KB | |
| 64 | ~68 KB | |
| 128 | ~266 KB | |
| 256 (default) | ~1.06 MB | |
| 510 | ~4.2 MB | the maximum; above this raise `CSV_MAX_FIELDS` too |

Three things that table does **not** cover:

- The fitter's matrices live in **static storage, not on the stack**, so the
  ceiling costs no stack at all and cannot overflow one.
- The **stack** requirement is about **350 KB** and comes from the line buffers
  in `constants.h`, not from the ceiling. Measured: it runs under
  `ulimit -s 352` and fails under 336. Shrink `CSV_LINE_MAX`, `MAX_INPUT` and
  `MAX_OUTPUT` together with the ceiling on a small target.
- Fitting **every** group in one pass holds one accumulator per group, so that
  path costs `groups x terms^2`, about 6 MB for 580 groups of 35 terms. It is
  still never a function of how many rows you feed it.

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
      10000        0.02 2432
      100000       0.24 2432
      ^ RSS should be flat: 10x the data, the same memory.

    generating a 580-group table and cases ... done
    SCORE: 100000 cases against 580 groups:
      cases        seconds  peak RSS (KB)
      100000       0.16 3712

    The coefficient table is the only thing that grows with the problem:
      580 groups x (35 + 1) doubles = about 163 KB, held once.

Ten times the data, the same memory. Time scales with the rows, as it must.

## The example data is synthetic

`conf/coefficients.csv`, `conf/trim_additions.csv` and both files under
`example/` are **made up**, generated so that fitting `example/train.csv`
returns exactly the coefficients in `conf/coefficients.csv`, which is what makes
the fitter testable against a known answer. They are fitted to nothing and mean
nothing. Point `coef.file` at your own table, or produce one with `-t`, before
any number here is worth reading. No real data is distributed with this project.

## Origin

The model here is not a textbook exercise. The author, formally trained in
physics and computer science, built a least-squares length-of-stay predictor
for industry in 2011: it went into production, ran against real caseloads, and
was read and maintained by other people. Releasing a C implementation of it as
open source was the intention at the time, and there was never time for it. This
is that implementation: written from scratch around the same arithmetic,
generalised so the terms come from your file instead of being fixed in the
source, and carrying none of the original data. The coefficients shipped here
are synthetic, and real tables belong to whoever owns the data and produced the
coefficients.

That background is why the numerics are written out rather than delegated, and
why the places where least squares stops being trustworthy (a design the data
cannot identify, a fit with no degrees of freedom left, the conditioning cost of
normal equations) are stated in this README instead of left for a reader to
discover.

Fifteen years is a long detour, but the shape of the problem has not changed:
somebody has a table of coefficients, a stream of rows, and a machine that
should not need a Python installation to multiply them together.

### The original term set

The 24 terms in `conf/coefficients.csv` are the production model's, and they are
a subset of it. The original carried **35**, over 579 groups (the scale check
above runs at a round 580). The eleven left out are recorded here, in the order
the original used them, so that a future hospital length-of-stay implementation
does not have to rediscover the schema:

    1-14   Cardioversion, Cell_saver, Chemotherapy, Dialysis,
           Heart_resuscitation, Mech_vent_ge_96_hours, mech_vent, Feeding_tube,
           Paracentesis, Parenteral_nutrition, Pleurocentesis, Radiotherapy,
           Tracheostomy, Vascular_access_device      # what was done
    15     lso_outlier                               # omitted here
    16-19  multiple_ie_2ormore, multiple_ie_3ormore, icu_indicator, hc_dad
    20-23  age_under1, age_under18, age_60plus, age_80plus
    24     obs_gt40
    25-27  p75_flag, p90_flag, p95_flag              # omitted here
    28-34  status_11 .. status_17                    # omitted here
    35     ooh

The eleven are `lso_outlier`, the three percentile flags, and the seven
discharge-status indicators. They are properties of how the stay ended rather
than of what was done during it, and none of them means anything without the
trimming rules that set their thresholds, so an example table meant to be
fitted and checked against a known answer is better off without them. A second
table held the other side of the same split and used a parallel set:
`non_lso_outlier`, `p10_flag`, `p25_flag`, `status_01 .. status_07`, 34 terms
over the same groups, with the 24 above common to both.

Only the names and their order are recorded here. The coefficients that went
with them were production values and are not in this repository; see *The
example data is synthetic* above. Anyone reimplementing this fits their own.

## Layout

```
main.c            CLI front end: options, one case or a stream, print the result
process.c/.h      THE slot: score a case (process), fit from a CSV (process_train)
los.c/.h          the model: the schema, the coefficient tables, predict + trim
regress.c/.h      ordinary least squares by accumulated normal equations
csv.c/.h          bounded CSV: read a line, split it in place
resolve.c/.h      find a data file: the current directory, then beside the binary
common.c/.h       safe primitives: die(), debug(), xmalloc(), xstrdup()
utils.c/.h        bounded string helpers (rtrim/ltrim)
params.c/.h       config: load system.properties, params_get("key")
hash.c/.h         generic string -> void* hash table (backs params and the groups)
constants.h       buffer sizes (the TERM ceiling lives in regress.h / los.h)
tests.c           in-place unit tests (make ut)
tests/cli.sh      black-box tests: the binary through a shell and a pty (make cliut)
conf/             the example model (synthetic)
example/          two training files with different schemas (synthetic)
scripts/scale.sh  measures the memory claim at 200 terms and 500 groups
scripts/hooks/    pre-push: the sanitizers, before anything reaches the remote
Makefile          the build
```

## Style

The rules the code already follows, so new code matches:

- **C99, warning-free.** Clean under `-std=c99 -Wall -Wextra`; a warning is a
  defect. `make pedantic` is the stricter gate.
- **A pinned term is marked, not laundered.** A coefficient the data could not
  identify is written as 0, and so is an estimated no-effect. The fit therefore
  emits a `# pinned <group>: constant ... collinear ...` line beside the row, so
  the distinction survives a redirect. It is a comment, so the table still reads
  straight back into the scorer.
- **Stack first, and count the heap out loud.** Rows are processed one at a time
  into fixed-size buffers; nothing on the row path allocates. Three things do
  allocate, each bounded by the model or the config and never by the data, and
  each freed on every path: the coefficient table (`los.c`), the config table
  (`params.c`), and one accumulator per group while `-t` fits them all
  (`process.c`). Three is the whole list, and it is meant to stay countable.
- **Bounded strings only.** `snprintf` always; never `strcpy`/`strcat`/`sprintf`,
  except the checked copy into a fixed buffer, where the guard sits on the
  line above and returns rather than truncating. Sizes come from `constants.h`.
- **Checked allocation.** `xmalloc`/`xstrdup` never return NULL.
- **Functions, not fragile macros.** `die`, `debug`, `xmalloc` are functions, so
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
- **A comment is a claim.** Header comments, source comments, the Makefile, and
  the usage text go stale exactly like a README. When behaviour changes they move
  with it. See `AGENTS.md`.

The full rationale, stack-first and bounded-memory to the avionics and
medical-device standard (NASA Power of Ten, MISRA C:2012 rule 21.3), is written
up in [ais](https://github.com/Anode1/ais), in
[`doc/dev/STYLE.md`](https://github.com/Anode1/ais/blob/main/doc/dev/STYLE.md).

## Platforms, and reporting a bug

Built and tested on Linux and macOS on every push (`.github/workflows/sanitizers.yml`
runs the suite plus AddressSanitizer and UndefinedBehaviorSanitizer on both).
BSD should work and is untested. There is no Windows build.

    make install                       # /usr/local
    make install PREFIX=$HOME/.local   # somewhere you own
    make install DESTDIR=/tmp/stage    # staged, for a package
    make uninstall

The binary goes in `bin`, the example tables in `share/linearr/conf`, and the
program looks in the current directory, then beside itself, then
`<bindir>/../share/linearr`. A symlink into a `bin` directory works too; the
link is resolved before it looks beside itself.

`./linearr --version` says which build you have.

Bugs and findings: open an issue with the exact command, the input that
reproduces it, and what you expected. A reproduction that fits in a shell
snippet is worth more than a description.

## See also

- [ais](https://github.com/Anode1/ais): the associative-memory engine these
  conventions come from.

## License

GNU GPL v2 or later; see `LICENSE`.

