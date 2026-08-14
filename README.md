# linearr: least squares in C, as simple as the method actually is

### It reports three of the ways the fit can mislead you, and the memory is O(1) in the number of rows

Ordinary least squares (OLS) as a command-line program. Reads a CSV and returns
the coefficients; reads a case and returns a prediction.

**This is a rewrite of something that ran in production.** The author built a
least-squares length-of-stay predictor for industry in 2011; it was deployed and
maintained by other people. This is the same arithmetic in C, written from
scratch, generalised so the terms come from your file, carrying none of the
original data. [Where it came from](#origin).

It requires a C compiler and `make`, and nothing else: no LAPACK, no BLAS, no
GSL, no third-party header of any kind. The training file is read one row at a
time and each row is forgotten, so the number of rows does not affect how much
memory the fit uses. The number of GROUPS does, and
[`--footprint`](#scale) says by how much.

    make
    ./linearr -t mydata.csv > model.csv       # fit every group in one pass
    ./linearr -c model.csv A x=3              # score a case against it

## What least squares is

Ordinary least squares (OLS) is linear regression
fitted by minimising the sum of squared residuals. You have rows: some
measurements, and a number you care about. It finds the straight line through
them that leaves the smallest total error, counting each miss squared. The
coefficients are how much each measurement moves the answer. The method is
Legendre and Gauss, around 1805, and it still fits wherever the relationship is
close to linear. (For relationships that are not, see
[When a line is the wrong shape](#when-a-line-is-the-wrong-shape) below.)

With one term, the whole method streams in five lines: hold a count, two means
and two sums, fold each row into them, and divide at the end.

    n += 1
    dx = x - mx;         dy = y - my
    mx += dx/n;          my += dy/n
    sxx += dx*(x - mx);  sxy += dx*(y - my)
    slope = sxy/sxx;     intercept = my - slope*mx

No row is kept, which is where the memory bound comes from, and those five
lines agree with this program on Anscombe's first set to every digit it prints.
Three things turn them into the 160 of `c/regress.c`: `p` terms make the last
line a `p`x`p` solve instead of a division, a term the data cannot identify has
to be pinned and reported rather than silently zeroed, and no value reaches a
coefficient unchecked.

In three situations the fit succeeds and the answer is not what it looks like:
the data cannot tell two columns apart, no residual freedom is left, or the
arithmetic has run out of digits. The program reports all three. They are not
the only ways a regression misleads -- leverage, an omitted variable, rows that
are not independent and a prediction outside the range the data covers are all
real and none of them is checked here -- but these three the program can see
from what it holds, so it says them.
[`doc/NUMERICS.md`](doc/NUMERICS.md) works through each with the example data.

## Anscombe's quartet

All four in one pass; the residual check names set II and says nothing about
III and IV, which it cannot see.
[Details](doc/NUMERICS.md#the-textbook-case-anscombes-quartet).

## Three things it does

Three things a small OLS implementation usually does not, each demonstrated in
its own section below:

- **The arithmetic is checkable.** It reproduces the NIST reference values for
  Norris and Longley to eleven digits, and Wampler1's exact quintic to eight
  under the default solver and nine under `--qr`. Against R's `lm()`, which
  solves by a different method, `--qr` agrees to 1e-11 on most examples and
  1e-6 on all of them, and that is what `make check` validates it against. The
  default solver is measured in the same table and deliberately not validated: on
  `nearly-the-same.csv`, the file that exists to show what squaring costs, it
  reaches only 1.1e-05.
  [Checked against answers somebody else certified](doc/NUMERICS.md#checked-against-answers-somebody-else-certified)
- **It reads the residuals.** R2 and a residual SD are summaries of the
  residuals and cannot see structure in them. This names the term whose square
  explains what is left, tests the fitted value for a missing interaction, and
  tests whether the error grows with the prediction.
  [Where the model is wrong](#where-the-model-is-wrong)
- **Memory is bounded by the model.** Rows are read one at a time and
  forgotten, so a 40 MB file and a 40 GB file cost the same. Memory scales with
  the number of GROUPS, one accumulator each, and `--footprint` prints the
  figure for a given shape. [Scale](#scale)

It is not a general statistics package. What it leaves out, and what to use
instead, is in [Suitability and limits](#suitability-and-limits).

## Fit and score

Two commands. Standard output is a complete coefficient file and standard
error is the commentary, so the redirect is the whole workflow:

    $ ./linearr -t example/simple-train.csv > model.csv
    reading: column 1 is the group, 'minutes' is the value being predicted, and the other 2 columns are terms. Use -y NAME if that is the wrong column
    fit: 2 groups, 13 rows, worst R2=1.0000, worst resid SD<5.17e-07, least df=3, worst cond=1.03 (normal equations)

    $ ./linearr -c model.csv A km=10 stops=3
    A prediction=34.5000

`-t` fits every group in one pass. `-c` names the model to score against and is
required: there is no default table and no search for one, because which model
produced a number is part of the number.

**Column 2 is the value being predicted**, unless `-y NAME` says otherwise.
Nothing in the data can say which column you meant, so a file written in
another order fits the wrong column and exits 0. The `reading:` line above says
what was taken, and ends with the remedy when the column was not named.

Read next in [`doc/FORMATS.md`](doc/FORMATS.md): the case forms, `--terms`,
where files are looked for, and what every error message means.

## Groups

A group is one fitted line. Rows sharing a group code are fitted together
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

How the printed numbers are rounded, and why the rounding is part of the
answer: [`doc/FORMATS.md`](doc/FORMATS.md#rounding).

## The example data

Three kinds of file in `example/`, and they are not interchangeable. The
published sets, whose answers somebody else computed: **Anscombe's quartet**,
and **Norris**, **Longley** and **Wampler1** from the NIST reference datasets.
One model that was actually deployed: the 24-term length-of-stay shape from
2011, terms kept and numbers generated. And six built to fail in one specific
way each, which the teaching sections use.

No real data is distributed with this project.
[Details](doc/FORMATS.md#the-example-data).

## When a line is the wrong shape

`--residuals` tells you the line is wrong. What to do next, in order, and the first two answers
are still this program:

**1. If it names a term, add that term's square and refit.** Still closed form, still exact,
still coefficients you can read. On measured comparisons this reaches the noise floor and beats
a neural network on the same data. It is the step most people skip.

**2. If it says the residuals depend on the prediction itself, some pair of terms interacts and
the check cannot say which.** With p terms there are p(p−1)/2 candidate pairs: one at two terms,
276 at twenty-four. If a domain expert can name the pair, add the product as a column and you are
back in closed form — measured on a length-of-stay shape, that reached 1.07 against a noise floor
of 1.0, where the line alone scored 1.75.

**3. Only if nobody can name it, fit a curve.**
[bpnn](https://github.com/Anode1/bpnn) is the companion for that: same CSV layout, same per-group
fitting, same fit-then-score split, a backpropagation network in place of the line. It reports
what it cannot tell you — the spread over refits, whether a case is outside the range it was
fitted on, and how much of the variance it explains. `scripts/escalate.sh` in that repository runs
this program first and escalates only on this program's own diagnostic.

**Where this leaves you with nothing, stated plainly.** On a table of many binary indicators, a
pairwise interaction can be present and this program's residual check stays *silent* — the effect
lives in the few rows carrying both indicators and is too small for the check to see. Measured: 24
procedure indicators at 18% prevalence, an interaction worth 9 days, and neither the check here
nor a network found it, while the line handed the pair reached the floor at once. If you suspect
an interaction and the check says nothing, that is not evidence there is none.

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

## Suitability and limits

It fits well when:

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
  it readable in an afternoon (`regress.c` is about 160 lines of code, and
  [What least squares is](#what-least-squares-is) has the five of them that are
  the method).

Reach for something else (or reach the author :) when you need regularization (ridge, lasso,
elastic net), categorical encoding, imputation, cross-validation,
weighted least squares, or inference: standard errors, confidence intervals,
prediction intervals, p-values. None of that is here. An empty field or an `NA`
is refused with the row and the term named, rather than filled in or quietly
dropped, which is a refusal and not a treatment of missing data. The residual
standard deviation is reported, as `resid SD=` in the fit summary: `sqrt(rss/df)`,
in the response's own units. Under a correctly specified model that estimates
the error SD, and it is the figure to quote — but it carries none of the
uncertainty in the coefficients themselves, so the error on a new row is larger
on average, and under the wrong shape it estimates nothing at all. R2 is a ratio
and does not give it either. The worked example involves a modelling choice that
should be stated: length of stay is a skewed, non-negative, count-like response,
and unweighted OLS on raw days is not the standard treatment for it. OLS stays
consistent for the mean if that mean really is linear; what fails is the
constant-variance assumption, the support (nothing stops this tool predicting a
negative stay), and any inference. A Gamma GLM with a log link is the usual
answer. A log transform is the other one, and it has a trap this tool cannot
help with: `exp(fitted)` is a median, not a mean, so publishing an expected
length of stay from a log fit needs a smearing correction — which matters here,
because the case this program is built for is a coefficient somebody else's
process consumes as an expected value. `scikit-learn` and `statsmodels` do all of it well, and GSL
(`gsl_multifit_linear`) or LAPACK (`dgels`) give you a fitted line in C with more
numerical machinery behind it than this has.

One numerical caveat, now with a remedy in the box. The default solver
accumulates cross-products and solves them, which squares a condition number,
so a badly scaled or near-collinear problem loses roughly twice the digits it
needs to. For indicator columns and modestly scaled data that is usually not
the limiting factor, and `cond=` says when it is. `--qr` solves the same fit
without squaring anything and remains streaming; see
[Two solvers](#two-solvers).

## Two solvers

The default accumulates the centered co-moments and equilibrates them before
solving, which bounds the memory and squares a condition number. The condition
number that matters is the one of the centered, column-scaled design, not of the
raw one. Longley's raw design has a condition number of 4.9e9, whose
square is past what a double can carry at all, and eleven digits still come
back; centered and scaled it is 110, whose square costs about four digits of
the sixteen there are. `--qr` rotates each row into a triangular factor
instead, one row at a time, and squares nothing. It is still streaming. On
`example/nearly-the-same.csv`, where two columns differ in the sixth decimal
and the answer is `1 + 2*x1 + 3*x2`:

    normal equations   A,1.00000000001,2.00002262993,2.99997737008
    --qr               A,1,1.99999999974,3.00000000026

Five correct digits against ten. The fit summary reports `cond=`; when it is
large, `--qr` is the one to use.

![Both solvers against R's lm(), one row per example file: seven files identical
under both, and three where the normal equations fall behind
`--qr`](doc/img/lm-agreement.svg)

Checked against answers computed elsewhere: Norris and Longley come back to
eleven digits against the NIST certified values. Against `lm()`, which solves
by a different method, `--qr` agrees to 1e-11 on most examples and 1e-6 on all
of them; the default is measured beside it and reaches 1.1e-05 on the file
above, which is the same five digits, said again. `make check` runs both, and
validates the `--qr` column. [Details](doc/NUMERICS.md).

## Scale

Memory is bounded by the model, not by the data. A row is folded into the
cross-products and dropped, so the tenth row and the ten-billionth cost the
same space. Measured on an idle machine, best of five: 2,000,000 rows by 8
terms over 200 groups in 0.50 s using 2.7 MB, and 10,000,000 in 2.45 s using
the same 2.7 MB.

What memory does scale with is the number of GROUPS, one accumulator each, and
`--footprint` prints the figure for a given shape:

    $ ./linearr --footprint 24 400000
    24 terms, 400000 groups

    fitting, -t, one accumulator per group
      per group   8704 bytes
      in total    3.24 GB

    scoring, a loaded coefficient table
      per group   2064 bytes
      in total    787.4 MB

    The scoring figure does not move with the term count: the
    coefficient array is sized at this build's ceiling of 256, so a
    small model pays for a large one. The fitting figure does move.

    Neither depends on the number of ROWS, which is the point:
    the same figures cover a thousand rows and a trillion.

At 4.1 million rows a second, a billion rows is four minutes and ten trillion
is 28 days; R stops at about 340 million because the frame runs out of memory,
not the clock.

![Peak memory against rows: linearr flat at 2.5 MB and 2.4 MB over a tenfold
increase, R's frame rising from 126 MB to 937 MB](doc/img/memory-scaling.svg)

Read next in [`doc/BENCHMARKS.md`](doc/BENCHMARKS.md): the full extrapolation
beside a streaming Python and R, and the machine every timing came from. A
long run reports progress once a minute, and where the residual SD is a bound
rather than a value the summary prints `<` instead of `=`; both are in
[`doc/NUMERICS.md`](doc/NUMERICS.md).

## The same job in other languages

The same fit written the way each language does it when allowed to stream: read
a line, update a fixed accumulator, forget the row. Every implementation's
coefficients are checked against linearr's before any time is printed.

| implementation | time | peak memory |
| --- | --- | --- |
| linearr (C) | 0.11s | 2.5 MB |
| Java | 0.36s | 104 MB |
| R, `read.csv` + `lm()` | 1.32s | 126 MB |
| Python, streaming | 2.76s | 10 MB |
| awk | 11.75s | 5.6 MB |

500,000 rows, 8 terms, 50 groups, on one laptop core. Ten times the rows leaves
the streaming figures flat and multiplies the in-memory ones, which is
where the difference stops being about speed: at a billion rows R needs about
180 GB and linearr holds 2.4 MB.

The R row is `read.csv` plus `lm()` because that is what people write, not
because R cannot stream. `bench/fit-stream.R` reads the same file in chunks,
returns identical coefficients, and holds 120 MB at 500,000 rows and 121 MB at
5,000,000: flat, like the others, and 50 MB of it is an interpreter that has
read nothing yet. Both R rows are measured in
[`doc/BENCHMARKS.md`](doc/BENCHMARKS.md).

Full tables, the ten-million-row measurements, the extrapolation to 10 trillion
and the machine they were taken on: [`doc/BENCHMARKS.md`](doc/BENCHMARKS.md).

## Origin

The predictor named at the top was one of several statistical tools the author,
trained in physics and computer science and a professional software developer,
has built for scientists in industry. Much of that work was this same job:
taking a model written in SAS, R or Matlab and turning it into C or Java, so it could
run where the original could not.

**This does not replace Python, R, SAS or Matlab.** Those are where a model is
explored, chosen and argued about. This is one method, small enough to read and
checkable against published answers, for the places those cannot go.

Read next in [`doc/ORIGIN.md`](doc/ORIGIN.md): what that work was, the same
porting offered for other tools, the limitations in full, and the 24-term model
the example data is shaped from.

## Platforms, install and bugs

Built and tested on Linux and macOS on every push
(`.github/workflows/sanitizers.yml` runs the suite plus AddressSanitizer and
UndefinedBehaviorSanitizer on both). BSD should work and is untested.

Tagging `v*` builds linux-x86_64, linux-arm64, macos-arm64 and
windows-x86_64 and attaches each to the GitHub release with a SHA-256
(`.github/workflows/release.yml`). Each one is built, unit-tested and made to
fit Longley on its own runner before it is uploaded. `sh scripts/dist.sh` makes
the same bundle locally.

**Every platform passes its tests in CI; only Linux has been used by hand.**
Each release runs the unit suite and a Longley fit on its own runner before the
binary is uploaded, and every platform above has done so since v0.4.3. What no
runner covers is ordinary use: the author works on Linux, so the macOS, arm64
and Windows binaries have never been driven by a person. The Windows one is
built on a Windows runner rather than cross-compiled, because
`x86_64-w64-mingw32-gcc` compiles every source with no warnings and a
cross-compiled binary still cannot be executed on the machine that built it,
and "it links" is not "it works".

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

## Documents

This file is what you need to decide whether the program is any use to you.
The detail sits beside it:

| document | what is in it |
| --- | --- |
| [`doc/NUMERICS.md`](doc/NUMERICS.md) | three ways a fit misleads, worked through; Anscombe's quartet; what each solver does and where each loses digits; the certified figures term by term; what a long run costs in accuracy |
| [`doc/FORMATS.md`](doc/FORMATS.md) | the three files, groups, fitting and scoring in full, every input refused and its message, the rounding, and what each example file is for |
| [`doc/ORIGIN.md`](doc/ORIGIN.md) | where this came from, the same porting offered for other tools, what it will not do, and the original 24-term model |
| [`doc/DIAGNOSTICS.md`](doc/DIAGNOSTICS.md) | what each residual check computes, its threshold, what to do about a warning, and where the checks are wrong |
| [`doc/BENCHMARKS.md`](doc/BENCHMARKS.md) | the full language comparison, the ten-million-row measurements, the extrapolation, and the machine they came from |
| [`doc/INTERNALS.md`](doc/INTERNALS.md) | the source layout, the style rules, every `make` target and what each check catches, the build ceilings and stack figures, Windows, and how to cut a release |
| [`doc/PROSE.md`](doc/PROSE.md) | how these documents are written |
| [`CHANGELOG.md`](CHANGELOG.md) | what shipped in each version |

Every transcript in all of them is run and diffed by `make readme`, so a stale
number in a linked document fails the build exactly as one here does.

## See also

- [ais](https://github.com/Anode1/ais): the associative-memory engine these
  conventions come from.

## License

BSD 2-Clause; see `LICENSE`.

