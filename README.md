# linearr: least squares in C, as simple as the method actually is

### It reports the three ways the fit can mislead you, and the memory does not grow with the number of rows

Ordinary least squares (OLS) as a command-line program. Reads a CSV and returns
the coefficients; reads a case and returns a prediction.
It requires a C compiler and `make`, and nothing else: no LAPACK, no BLAS, no
GSL, no third-party header of any kind. The training file is read one row at a
time and each row is forgotten, so the number of rows does not affect how much
memory the fit uses. The number of GROUPS does, and
[`--footprint`](#scale) says by how much.

    make
    ./linearr -t mydata.csv > model.csv       # fit every group in one pass
    ./linearr -c model.csv A x=3              # score a case against it

## What least squares is

**What it does.** Ordinary least squares (OLS), which is linear regression
fitted by minimising the sum of squared residuals. You have rows: some
measurements, and a number you care about. It finds the straight line through
them that leaves the smallest total error, counting each miss squared. The
coefficients are how much each measurement moves the answer. The method is
Legendre and Gauss, around 1805, and it still fits wherever the relationship is
close to linear.

In three situations the fit succeeds and the answer is not what it looks like:
the data cannot tell two columns apart, no residual freedom is left, or the
arithmetic has run out of digits. The program reports all three.
[`doc/NUMERICS.md`](doc/NUMERICS.md) works through each with the example data.

## Anscombe's quartet

All four in one pass; the residual check names set II and says nothing about
III and IV, which it cannot see.
[Details](doc/NUMERICS.md#the-textbook-case-anscombes-quartet).

## What it is, and what it is not

Three things it does that a small OLS implementation usually does not, each
demonstrated in its own section below:

- **The arithmetic is checkable.** It reproduces the NIST reference values for
  Norris and Longley to eleven digits, and Wampler1's exact quintic to nine
  under the default solver and ten under `--qr`. It agrees with R's `lm()` to
  1e-6 or better on every example, and to 1e-11 on most of them. Both run in
  `make check`, and `lm()` solves by a different method, so the agreement is
  evidence and not a tautology.
  [Checked against answers somebody else certified](doc/NUMERICS.md#checked-against-answers-somebody-else-certified)
- **It reads the residuals.** R2 and a residual SD are averages over the
  residuals and cannot see structure in them. This names the term whose square
  explains what is left, tests the fitted value for a missing interaction, and
  tests whether the error grows with the prediction.
  [Where the model is wrong](#where-the-model-is-wrong)
- **Memory is bounded by the model.** Rows are read one at a time and
  forgotten, so a 40 MB file and a 40 GB file cost the same. Memory scales with
  the number of GROUPS, one accumulator each, and `--footprint` prints the
  figure for a given shape. [Scale](#scale)

It is not a general statistics package. What it leaves out, and what to use
instead, is [Where this is the right tool, and where it is
not](#where-this-is-the-right-tool-and-where-it-is-not).

## Fit and score

Fit a model from your own data and score against it, with no configuration file
anywhere. A training row is `group,value,<one column per term>`, and the header
names the terms:

    $ cat example/simple-train.csv
    # example/simple-train.csv: nothing medical about it, minutes on the road as a
    # function of distance and stops, two terms instead of twenty-four. The point of
    # this file is that the program never had to change to fit it: the header names
    # the terms, so this IS the whole schema.
    #
    #   MINUTES = 5 + 2.5*km + 1.5*stops
    #
    group,minutes,km,stops
    A,5.0,0,0
    A,30.0,10,0
    A,9.5,0,3
    A,34.5,10,3
    A,19.0,5,1
    A,58.0,20,2
    A,18.5,3,4
    B,12.0,0,0
    B,22.0,4,0
    B,18.0,0,4
    B,28.0,4,4
    B,20.0,2,2
    B,34.0,7,3

    $ ./linearr -t example/simple-train.csv > model.csv
    reading: column 1 is the group, 'minutes' is the value being predicted, and the other 2 columns are terms. Use -y NAME if that is the wrong column
    fit: 2 groups, 13 rows, worst R2=1.0000, worst resid SD<5.17e-07, least df=3, worst cond=1.03 (normal equations)

    $ cat model.csv
    # response: minutes
    group,intercept,km,stops
    A,5,2.5,1.5
    B,12,2.5,1.5

    $ ./linearr -c model.csv --no-trim A km=10 stops=3
    A prediction=34.5000

Those three columns were the whole schema; it ships as
`example/simple-train.csv` if you want to run it as it stands.

`-t` fits **every group in the file**, one line each, in a single pass. Standard
output is a complete coefficient file and standard error is the commentary, so
the redirect above is the workflow.

**Which column is the value.** Column 2 unless you say otherwise, and nothing
in the data can say which column you meant. A file written as
`site,dose,age,response` therefore fits `dose` from `age` and `response`, prints
plausible coefficients and exits 0. Name the column instead, and the `reading:`
line reports what it took. Asking `simple-train.csv` for the wrong thing on
purpose, predicting stops from minutes and distance:

    $ ./linearr -t example/simple-train.csv -y stops
    reading: column 1 is the group, 'stops' is the value being predicted, and the other 2 columns are terms
    fit: 2 groups, 13 rows, worst R2=1.0000, worst resid SD<5.346e-08, least df=3, worst cond=57.6 (normal equations)
    # response: stops
    group,intercept,minutes,km
    A,-3.33333333333,0.666666666667,-1.66666666667
    B,-8,0.666666666667,-1.66666666667

Column 1 stays the group, the named column becomes the value, and every other
column is a term in the order it appears. Without `-y` the `reading:` line ends
with the remedy, so the mistake is visible on the first run rather than in the
numbers.

The rest of this section uses `example/coefficients.csv`, which is the 24-term
model described under [The example data](#the-example-data-and-what-each-file-is-for):
the shape of something that ran in production, rather than a book exercise. It
is here because scoring is where width shows. Two terms can be typed; twenty-four
is where naming them matters, where a trim table exists, and where `--terms` stops
being a convenience.

Ask what a model expects:

    $ ./linearr --terms -c example/coefficients.csv
    24 terms and 12 groups in example/coefficients.csv
        1  Cardioversion
        2  Cell_saver
      ...
       17  icu_indicator
      ...

Score by naming the terms that are not zero; everything else is 0:

    $ ./linearr -c example/coefficients.csv --trim example/trim_additions.csv 001 Cardioversion=1 icu_indicator=1
    001 prediction=19.9611 trim=46.5

The same case as a row, every term in the table's column order. This is the form
read from stdin, so a file of cases round trips through a pipeline:

    $ ./linearr -c example/coefficients.csv --trim example/trim_additions.csv "001,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0"
    001 prediction=19.9611 trim=46.5

    $ ./linearr -c example/coefficients.csv --trim example/trim_additions.csv < example/cases.csv
    001 prediction=19.9611 trim=46.5

At two terms the row form is fine. At two hundred it is unusable, which is why
the named form exists and is what the rest of this README uses.

Files named with `-c`, `--trim` and `-t` are looked for in the current
directory first, then beside the program, so an installed `linearr` finds the
example data from anywhere and your own file still wins where you have one.

**`-c` is required for scoring.** There is no default table and no search for
one. Which model produced a number is part of the number, so it is named rather
than found by convention: the same command in two directories cannot quietly
answer from two different models.

When something is wrong, the message says what:

    $ ./linearr -c example/coefficients.csv --trim example/trim_additions.csv 001 nosuchterm=1
    cannot score group '001': no term 'nosuchterm' in example/coefficients.csv; run --terms to list them

`./linearr -h` prints the options; `-d` traces to stderr.

## Groups

**A group is one fitted line.** Rows sharing a group code are fitted together
and get their own coefficients, so one file and one pass produce one model per
subset instead of splitting the file and running the program once per part.
With a single group this is plain least squares.

    $ ./linearr -t example/routes.csv
    reading: column 1 is the group, 'minutes' is the value being predicted, and the other 2 columns are terms. Use -y NAME if that is the wrong column
    fit: 3 groups, 18 rows, worst R2=1.0000, worst resid SD<6.994e-07, least df=3, worst cond=1.02 (normal equations)
    # response: minutes
    group,intercept,km,stops
    city,5,3,2
    suburb,4,2,1.5
    highway,8,1,5

Three minutes per kilometre in the city, one on the highway. Pooling the same
eighteen rows with `-g '*'` gives one line that describes none of them.

Read next in [`doc/FORMATS.md`](doc/FORMATS.md): the layout of all three files
(group first, terms last, only the middle differs), and every input the reader
refuses with the message each one gives.

## Options

| option | default | meaning |
| --- | --- | --- |
| `-c FILE` | *(required to score)* | the fitted model: `group,intercept,<one column per term>` |
| `--trim FILE` | *(none)* | `group,trim_addition`. Without it the trim point is the prediction |
| `--no-trim` | | says the same thing explicitly |
| `--scale N` | 4 | decimal places the prediction is rounded to, 0 to 9 |
| `--trim-scale N` | 1 | decimal places the trim point is rounded to, 0 to 9 |

A scale outside 0 to 9 is an error rather than a silent fallback to the
default.

How the printed numbers are rounded, and why the rounding is part of the answer
rather than presentation: [`doc/FORMATS.md`](doc/FORMATS.md#rounding).

## The example data, and what each file is for

`example/` holds three kinds of file with three different purposes, and they are
not interchangeable.

**Published sets, with answers computed by somebody else.** These are the ones a
statistician already knows, and their point is that you do not have to take this
project's word for anything.

| file | what it is | why it is here |
| --- | --- | --- |
| `anscombe.csv` | Anscombe's quartet, 1973 | four sets with identical summaries and nothing else in common. The standard demonstration that a fitted line and an R2 do not describe a dataset |
| `norris.csv` | NIST StRD Norris | the easy certified case: one term, an almost exact fit. If this is wrong, something ordinary is broken |
| `longley.csv` | NIST StRD Longley, 1967 | published because the regression programs of the day returned as few as two correct digits on it. The standard hard case |
| `wampler1.csv` | NIST StRD Wampler1 | an exact quintic, so any departure from 1 is the solver's own error. This is the file that separates the two solvers |

The worked example is hospital length of stay: a prediction per case-mix group,
plus that group's *trim point*, the day count past which a stay stops being
typical. That is the shape the data below has; the program has no idea what a
hospital is.

**One model that was actually deployed.** `coefficients.csv`, `trim_additions.csv`,
`train.csv` and `cases.csv` are the 24-term shape of a length-of-stay model the
author ran in production in 2011, with the terms kept and the data replaced.
Its purpose is different from the sets above and it is not a substitute for
them: it shows the program at a width and a shape that came from a real
problem rather than from a book, including a trim table, twelve groups, and
term names that came from the problem. The numbers in it are **generated**,
chosen so that fitting `train.csv` returns exactly the coefficients in
`coefficients.csv`.

**Two plain examples**, which are the ones to start from. `simple-train.csv` is
the smallest fit worth showing, minutes on the road against distance and stops.
`routes.csv` is the same terms over three kinds of route, and is why groups
exist.

**Four that fit successfully and are wrong anyway.** These are not failing
cases and they do not exit non-zero: each one produces a coefficient table, and
the point of each is the warning printed beside it. They are the worked
examples for the checks this program exists to run.

| file | what the fit reports |
| --- | --- |
| `together.csv` | two columns the data cannot tell apart |
| `three-rows.csv` | a line with no residual degrees of freedom |
| `nearly-the-same.csv` | a design whose trailing digits are noise, and what `--qr` does about it |
| `curve.csv` | a parabola fitted with a straight line |

**Two the program refuses**, which are the only files here that exit non-zero.
`gaps.csv` has an empty field and `semicolons.csv` is semicolon-separated; both
exist so the [refusal messages](doc/FORMATS.md#what-it-will-not-read) can be shown rather than
described.

No real data is distributed with this project. Point `-c` at your own table, or
produce one with `-t`, before any number here is worth reading.

## Where the model is wrong

R2 and a residual SD are averages over the residuals, so neither can see
structure in them. With `--residuals` the second pass runs three checks: each
term against a curve, the prediction against a missing interaction, and the
size of the error against the size of the prediction. Each names what it found
and where.

    warning: in group II the residuals still depend on x after the line is
    subtracted (t=-2219.2). A straight line is probably the wrong shape in that
    term; consider adding its square as a column.

Read next in [`doc/DIAGNOSTICS.md`](doc/DIAGNOSTICS.md): what each check
computes, the thresholds and why they are what they are, what to do about a
warning, and where the checks are wrong.

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
prediction intervals, p-values. None of that is here. The residual standard
deviation is reported, as `resid SD=` in the fit summary: the typical distance
between the fit and the rows it was fitted to, in the response's own units. It
is an in-sample figure and a floor, not an estimate of the error on a new row. R2 is
a ratio and does not give it. The worked example involves a modelling choice
that should be stated: length of stay is a skewed,
non-negative, count-like response, and unweighted OLS on raw days is not the
standard treatment for it (a log transform or a Gamma GLM is). Nothing stops
this tool predicting a negative stay. `scikit-learn` and `statsmodels` do all of it well, and GSL
(`gsl_multifit_linear`) or LAPACK (`dgels`) give you a fitted line in C with more
numerical machinery behind it than this has.

**One numerical caveat**, now with a remedy in the box. The default solver
accumulates `X'X`, which squares the condition number of the design, so a badly
scaled or near-collinear problem loses roughly twice the digits it needs to. For
indicator columns and modestly scaled data that is usually not the limiting
factor, and `cond=` says when it is. `--qr` solves the same fit without squaring
anything and remains streaming; see [Two solvers](#two-solvers).

## Two solvers

The default accumulates `X'X`, which bounds the memory and squares the
condition number of the design. `--qr` rotates each row into a triangular
factor instead, one row at a time, and squares nothing. It is still streaming.
On `example/nearly-the-same.csv`, where two columns differ in the sixth decimal
and the answer is `1 + 2*x1 + 3*x2`:

    normal equations   A,1.00000000001,2.00002262993,2.99997737008
    --qr               A,1,1.99999999974,3.00000000026

Five correct digits against ten. The fit summary reports `cond=`; when it is
large, `--qr` is the one to use.

**Checked against answers computed elsewhere.** Norris and Longley come back to
eleven digits against the NIST certified values; `lm()` agrees to 1e-11 on most
examples and 1e-6 on all of them, and solves by a different method, so the
agreement is evidence rather than the same arithmetic twice. `make check` runs
both. [Details](doc/NUMERICS.md).

## Scale

**Memory is bounded by the model, not by the data.** A row is folded into the
cross-products and dropped, so the tenth row and the ten-billionth cost the
same space. Measured: 2,000,000 rows by 8 terms in 0.42 s using 2.4 MB, and
10,000,000 in 2.04 s using the same 2.4 MB.

What memory does scale with is the number of GROUPS, one accumulator each, and
`--footprint` prints the figure for a given shape rather than leaving you to
trust a sentence:

    $ ./linearr --footprint 24 400000
    24 terms, 400000 groups

    fitting, -t, one accumulator per group
      per group   8456 bytes
      in total    3.15 GB

    scoring, a loaded coefficient table
      per group   2064 bytes
      in total    787.4 MB

    The scoring figure does not move with the term count: the
    coefficient array is sized at this build's ceiling of 256, so a
    small model pays for a large one. The fitting figure does move.

    Neither depends on the number of ROWS:
    the same figures cover a thousand rows and a trillion.

At 4.9 million rows a second, a billion rows is three and a half minutes and
ten trillion is 24 days; R stops at about 200 million because the frame runs
out of memory, not the clock.

Read next in [`doc/BENCHMARKS.md`](doc/BENCHMARKS.md): the full extrapolation
beside a streaming Python and R, and the machine every timing came from. A
long run reports progress once a minute, and where the residual SD is a bound
rather than a value the summary prints `<` instead of `=`; both are in
[`doc/NUMERICS.md`](doc/NUMERICS.md).

## The same job in other languages

The same fit written the way each language does it when allowed to stream: read
a line, update a fixed accumulator, forget the row. Every implementation's
coefficients are checked against linearr's before any time is printed.

| implementation | time | peak RSS |
| --- | --- | --- |
| linearr (C) | 0.11s | 2.5 MB |
| Java | 0.36s | 104 MB |
| R, `read.csv` + `lm()` | 1.32s | 126 MB |
| Python, streaming | 2.76s | 10 MB |
| awk | 11.75s | 5.6 MB |

500,000 rows, 8 terms, 50 groups, on one laptop core. Ten times the rows leaves
the streaming figures flat and multiplies the materialising ones, which is
where the difference stops being about speed: at a billion rows R needs about
200 GB and linearr holds 2.4 MB.

Full tables, the ten-million-row measurements, the extrapolation to 10 trillion
and the machine they were taken on: [`doc/BENCHMARKS.md`](doc/BENCHMARKS.md).

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

The problem itself has not changed in the interval: a table of coefficients, a
stream of rows to apply them to, and often a machine on which installing a
scientific stack is inconvenient or not permitted.

One type of the author's work in industry was this: taking models written by
scientists in SAS, R and Matlab and turning them into C, or into plain Java
without frameworks, so that they could run where the original could not. The
view behind that work, and behind this program, is that computation belongs as
close to the processor and to the memory it touches as the problem allows;
distance from it costs time, energy and hardware that a straight line does not
need.

The second view is about shape rather than language: data of any size should be
read as a stream, so that its size stops being a design question, at the cost of
one pass and one core and a second pass for anything needing a second look.

That is a claim about implementation, not about tools. **This does not replace
Python, R, SAS or Matlab, and is not trying to.** Those are where a model should
be explored, chosen, tested and argued about, and they have decades of
statistics behind them that this has not. What this offers is a small, checkable
implementation of one method, useful in three places: as something to check an
implementation against, since it reproduces the NIST certified values to eleven
digits (see [Checked against answers somebody else
certified](doc/NUMERICS.md#checked-against-answers-somebody-else-certified)); on embedded and
small ARM targets where no interpreter is going to be installed; and in cloud
batch work, where the memory a process holds is what it costs.

The same work can be done for other tools. If you have a model or a numeric
routine that runs in Python, R, SAS or Matlab and needs to run somewhere none
of those can be installed, it can be ported to C the way this was: no
dependencies, memory bounded by the model rather than the data, and the
arithmetic checked against the original before anything is trusted. Open an
issue on [this repository](https://github.com/Anode1/linearr/issues) to reach
the author.

### The limitations of that, stated

One core and one stream: no threading, no sharding, no restart from a partial
fit, and at 4.9 million rows a second the cost is reading text rather than the
arithmetic, so a second core would buy more than a faster solver.
`--residuals` reads the file a second time and needs a real file rather than a
pipe.

What it costs in memory is in [Scale](#scale), and what it does not do at all
is in [Where this is the right tool](#where-this-is-the-right-tool-and-where-it-is-not)
and [What it will not read](doc/FORMATS.md#what-it-will-not-read).

### The original term set

The 24 terms in `example/coefficients.csv` are the production model's, and they are
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

## Platforms, and reporting a bug

Built and tested on Linux and macOS on every push
(`.github/workflows/sanitizers.yml` runs the suite plus AddressSanitizer and
UndefinedBehaviorSanitizer on both). BSD should work and is untested.

**Binaries.** Tagging `v*` builds linux-x86_64, linux-arm64, macos-arm64 and
windows-x86_64 and attaches each to the GitHub release with a SHA-256
(`.github/workflows/release.yml`). Each one is built, unit-tested and made to
fit Longley on its own runner before it is uploaded. `sh scripts/dist.sh` makes
the same bundle locally.

**Only the Linux build has been run by the author.** The workflow's Linux path
was executed step by step; the macOS, arm64 and Windows paths have not run yet
and are untested until the first release. The Windows binary in particular:
`x86_64-w64-mingw32-gcc` compiles every source with no warnings, but a
cross-compiled binary cannot be executed on the machine that built it, which is
why the workflow builds Windows on a Windows runner instead. Treat the first
tagged release as the test.

    make install                       # /usr/local
    make install PREFIX=$HOME/.local   # somewhere you own
    make install DESTDIR=/tmp/stage    # staged, for a package
    make uninstall

The binary goes in `bin`, the example data in `share/linearr/example`, and the
program looks in the current directory, then beside itself, then
`<bindir>/../share/linearr`. A symlink into a `bin` directory works too; the
link is resolved before it looks beside itself.

`./linearr --version` says which build you have.

Bugs and findings: open an issue with the exact command, the input that
reproduces it, and what you expected. A reproduction that fits in a shell
snippet is worth more than a description.

## The rest of it

This file is what you need to decide whether the program is any use to you.
The detail sits beside it, and nothing was dropped in the move:

| document | what is in it |
| --- | --- |
| [`doc/NUMERICS.md`](doc/NUMERICS.md) | the three ways a fit misleads, worked through; Anscombe's quartet; what each solver does and where each loses digits; the certified figures term by term; what a long run costs in accuracy |
| [`doc/FORMATS.md`](doc/FORMATS.md) | the three files, what a group is, every input the reader refuses and the message it gives, and how the printed numbers are rounded |
| [`doc/DIAGNOSTICS.md`](doc/DIAGNOSTICS.md) | what each residual check computes, its threshold, what to do about a warning, and where the checks are wrong |
| [`doc/BENCHMARKS.md`](doc/BENCHMARKS.md) | the full language comparison, the ten-million-row measurements, the extrapolation, and the machine they came from |
| [`doc/INTERNALS.md`](doc/INTERNALS.md) | the source layout, the style rules, every `make` target and what each gate catches, the build ceilings and stack figures, Windows, and how to cut a release |
| [`CHANGELOG.md`](CHANGELOG.md) | what shipped in each version |

Every transcript in all of them is run and diffed by `make readme`, so a stale
number in a linked document fails the build exactly as one here does.

## See also

- [ais](https://github.com/Anode1/ais): the associative-memory engine these
  conventions come from.

## License

BSD 2-Clause; see `LICENSE`.

