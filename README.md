# linearr: least squares in C, as simple as the method actually is

### It reports the three ways the fit can mislead you, and the memory does not grow with the number of rows

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

## The example data

Three kinds of file in `example/`, and they are not interchangeable. The
published sets, whose answers somebody else computed: **Anscombe's quartet**,
and **Norris**, **Longley** and **Wampler1** from the NIST reference datasets.
One model that was actually deployed: the 24-term length-of-stay shape from
2011, terms kept and numbers generated. And six built to fail in one specific
way each, which the teaching sections use.

No real data is distributed with this project.
[Details](doc/FORMATS.md#the-example-data-and-what-each-file-is-for).

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

| implementation | time | peak memory |
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

The author, trained in physics and computer science, built a least-squares
length-of-stay predictor for industry in 2011. It went into production and was
maintained by other people. This is a C implementation of the same arithmetic,
written from scratch, generalised so the terms come from your file, and
carrying none of the original data.

Two views behind it: computation belongs as close to the processor as the
problem allows, and data of any size should be read as a stream, so its size
stops being a design question.

**This does not replace Python, R, SAS or Matlab.** Those are where a model is
explored, chosen and argued about. This is one method, small enough to read and
checkable against published answers, for the places those cannot go.

One core, one stream, no restart from a partial fit, and at 4.9 million rows a
second the cost is reading text rather than the arithmetic.

Read next in [`doc/ORIGIN.md`](doc/ORIGIN.md): what that work was, the same
porting offered for other tools, the limitations in full, and the 24-term model
the example data is shaped from.

## The original term set

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
| [`doc/FORMATS.md`](doc/FORMATS.md) | the three files, groups, fitting and scoring in full, every input refused and its message, the rounding, and what each example file is for |
| [`doc/ORIGIN.md`](doc/ORIGIN.md) | where this came from, the same porting offered for other tools, what it will not do, and the original 24-term model |
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

