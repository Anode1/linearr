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

## The textbook case: Anscombe's quartet

Anscombe's quartet, fitted four at once, which is what groups are for:

    $ ./linearr -t example/anscombe.csv
    reading: column 1 is the group, 'y' is the value being predicted, and the other 1 column is a term. Use -y NAME if that is the wrong column
    fit: 4 groups, 44 rows, worst R2=0.6662, worst resid SD=1.237, least df=9
    # response: y
    group,intercept,x
    I,3.00009090909,0.500090909091
    II,3.00090909091,0.5
    III,3.00245454545,0.499727272727
    IV,3.00172727273,0.499909090909

The same line four times to two decimals, and each set on its own gives
`R2=0.666`, `resid SD=1.24`, `df=9`. Twelve significant digits are printed, so
the small differences between the four are visible here and are not in most
tools.

    $ ./linearr -t example/anscombe.csv --residuals r.csv
    residuals: r.csv
    reading: column 1 is the group, 'y' is the value being predicted, and the other 1 column is a term. Use -y NAME if that is the wrong column
    fit: 4 groups, 44 rows, worst R2=0.6662, worst resid SD=1.237, least df=9
    warning: in group II the residuals still depend on x after the line is subtracted (t=-2219.2). A straight line is probably the wrong shape in that term; consider adding its square as a column.
    # response: y
    group,intercept,x
    I,3.00009090909,0.500090909091
    II,3.00090909091,0.5
    III,3.00245454545,0.499727272727
    IV,3.00172727273,0.499909090909

Set II is named. **Sets III and IV are not, and should not be taken as
passing:** neither is a wrong shape, both are single points with more influence
than the other ten together, and this program has no measure of leverage or
influence to find them with. The silence there is a gap, not approval.

### What the residuals of set II actually look like

The warning is a number. The residuals are the evidence, and set II is the case
every course uses to show what a line cannot do. Fit that set alone and keep
them:

    $ ./linearr -t example/anscombe.csv -g II --residuals r.csv
    residuals: r.csv
    reading: column 1 is the group, 'y' is the value being predicted, and the other 1 column is a term. Use -y NAME if that is the wrong column
    fit: 11 rows, R2=0.6662, resid SD=1.237, df=9
    warning: in group II the residuals still depend on x after the line is subtracted (t=-2219.2). A straight line is probably the wrong shape in that term; consider adding its square as a column.
    # response: y
    group,intercept,x
    II,3.00090909091,0.5

The file has no x column, but with one term the prediction is a straight
increasing function of x, so sorting on it puts the rows in x order:

    $ sort -t, -k3 -g r.csv
    group,observed,predicted,residual
    II,3.1,5.00090909091,-1.90090909091
    II,4.74,5.50090909091,-0.760909090909
    II,6.13,6.00090909091,0.129090909091
    II,7.26,6.50090909091,0.759090909091
    II,8.14,7.00090909091,1.13909090909
    II,8.77,7.50090909091,1.26909090909
    II,9.14,8.00090909091,1.13909090909
    II,9.26,8.50090909091,0.759090909091
    II,9.13,9.00090909091,0.129090909091
    II,8.74,9.50090909091,-0.760909090909
    II,8.1,10.0009090909,-1.90090909091

The last column is an arch: negative at both ends, positive through the middle,
symmetric about the sixth row to every digit printed. That is a parabola with
its line taken away, and it is the shape a residual plot is read for. The eleven
numbers say it without a plot.

Note the sum of that column is zero and its mean is zero, as least squares
guarantees; the residual SD is 1.237 whichever of the four sets you fit. No
summary of these numbers can see the arch. Only their order can, and order is
what a single figure throws away.

The remedy is the one the warning names. Add a column holding x squared to the
training file and fit again, and the arch goes; the header names the terms, so
nothing else changes.

So the quartet exercises all three parts at once: groups fit in one pass, a
summary that cannot tell the four apart, and a residual check that separates the
one case it is built for and says plainly that it does not catch the other two.

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

## The three files, and what a group is

**A group is one fitted line.** Rows sharing a group code are fitted together
and get their own coefficients; a different code gets different ones. Groups
exist so that one file and one pass produce one model per subset, instead of
splitting the file and running the program once per part. With a single group
this is plain least squares.

`example/routes.csv` is delivery time against distance and stops on three kinds
of route. The terms are the same everywhere; what each term is worth is not:

    $ ./linearr -t example/routes.csv
    reading: column 1 is the group, 'minutes' is the value being predicted, and the other 2 columns are terms. Use -y NAME if that is the wrong column
    fit: 3 groups, 18 rows, worst R2=1.0000, worst resid SD<6.994e-07, least df=3, worst cond=1.02 (normal equations)
    # response: minutes
    group,intercept,km,stops
    city,5,3,2
    suburb,4,2,1.5
    highway,8,1,5

Three minutes per kilometre in the city, one on the highway; a stop costs two
minutes in the city and five on a highway route. Pooling the same eighteen rows
into one line gives an average of the three that describes none of them:

    $ ./linearr -t example/routes.csv -g '*'
    reading: column 1 is the group, 'minutes' is the value being predicted, and the other 2 columns are terms. Use -y NAME if that is the wrong column
    fit: 18 rows, R2=0.8159, resid SD=7.693, df=15, cond=1.02 (normal equations)
    group,intercept,km,stops
    *,5.66666666667,2,2.83333333333

R2 of 0.82 still looks acceptable. The residual SD is what shows the cost: the pooled
line misses the rows it was fitted to by about 7.7 minutes, against essentially
zero for the per-group fits above. On rows it has not seen it would do no
better.

Every file puts the group first and the terms last, in the same order. Only the
middle differs:

| file | column 1 | column 2 | columns 3.. |
| --- | --- | --- | --- |
| training (`-t`) | group | **the goal**, what you are predicting | one per term |
| coefficients (`-c`) | group | the intercept | one per term |
| a case | group | *(none)* | one per term |
| residuals (`--residuals`) | group | observed | predicted, residual |

So a training header of `group,minutes,km,stops` says: predict `minutes` from
`km` and `stops`, separately for each group. The names are yours (the program
reads position, not the word), but the ORDER is fixed, and the goal is the
second column, not the first.

**Nothing in the data can say which column is the goal**, so a file written in
another order does not fail. It fits, it reports a good R2, and it answers a
question you did not ask: with `group,km,minutes,stops` it predicts distance
from time and stops, which is arithmetic about the same rows and not the model
you wanted. There is no way for the program to notice. What it can do is say
what it took, which it does, first, on every fit:

    $ ./linearr -t example/simple-train.csv
    reading: column 1 is the group, 'minutes' is the value being predicted, and the other 2 columns are terms. Use -y NAME if that is the wrong column
    fit: 2 groups, 13 rows, worst R2=1.0000, worst resid SD<5.17e-07, least df=3, worst cond=1.03 (normal equations)
    # response: minutes
    group,intercept,km,stops
    A,5,2.5,1.5
    B,12,2.5,1.5

Read that line once and the mistake is visible immediately. The same name is
written into the coefficient file as `# response: minutes`, so a table found a
year later still says what it predicts.

## What it will not read

The reader is a comma splitter, not a CSV parser. Each of the following is
refused, with a status of 1 and a message naming the row, the column and the
reason. None of them is a crash, and none produces a partial model.

**No missing values, and no imputation.** An empty field, `NA`, `NULL`, `-`:

    $ ./linearr -t example/gaps.csv
    cannot fit: example/gaps.csv row 2: term km is empty, which is not a number. This fits numbers only: there is no imputation for an empty field and no encoding for a category name

This is a design decision and not an omission. Mean-filling, last-observation
carry-forward and multiple imputation each change the answer, and which one is
right is a question about your data that a program reading it one row at a time
cannot answer. Filling gaps is a decision you should make on purpose, in
whatever wrote the file. R and Python have libraries for it.

**No categorical columns.** A column of `red`/`blue` is refused by the same
message. Encode it yourself as indicator columns, one per level minus one; that
is what `example/train.csv` is made of, and the fit reports the level that
cannot be separated rather than dropping it silently.

**No quoting.** Fields are split on commas and nothing else, so a quoted field
keeps its quotes. That is refused now, in both directions: a quoted number is
not a number, and a quoted name would silently become a DIFFERENT name, which
is worse. `"A"` and `A` would have been two groups.

**No embedded line breaks.** A record is a line. A quoted field containing a
newline is two lines here, and the row that results is refused for having the
wrong number of fields, with the quote named as the likely cause.

**Commas only.** Semicolons and tabs are refused by name, in the header and in
the rows:

    $ ./linearr -t example/semicolons.csv
    cannot fit: example/semicolons.csv has no commas in its header, but does have semicolons. It looks semicolon-separated; this program reads commas only

Excel writes semicolons wherever the comma is the decimal separator, which is
most of continental Europe. `tr ';' ','` fixes it when the decimal mark is a
point; when it is a comma, the file needs a real conversion and this program is
not the place for it.

**CRLF is fine.** A file saved on Windows reads normally.

The common thread: this reads files a program wrote for it, not files a
spreadsheet exported. One pass of `tr`, `awk` or `csvkit` puts a real CSV into
this shape, and doing it there keeps the decisions where you can see them.

## Options

Every setting is an option. There is no configuration file to find, write or
keep in step with the command line:

| option | default | meaning |
| --- | --- | --- |
| `-c FILE` | *(required to score)* | the fitted model: `group,intercept,<one column per term>` |
| `--trim FILE` | *(none)* | `group,trim_addition`. Without it the trim point is the prediction |
| `--no-trim` | | says the same thing explicitly |
| `--scale N` | 4 | decimal places the prediction is rounded to, 0 to 9 |
| `--trim-scale N` | 1 | decimal places the trim point is rounded to, 0 to 9 |

A scale outside 0 to 9 is an error rather than a silent fallback to the
default.

**Coefficients are written to 12 significant digits**, so an exact 5 prints as
`5`. That is far below the residual standard deviation of any fit that produced
them, and it is significant digits rather than decimal places: four decimals
would write every coefficient below 5e-5 as `0.0000`. `--scale` governs the
prediction, not the model.

Rounding is half away from zero, not `printf`'s half to even, and it is part of
the answer rather than presentation: the trim point is built on the *rounded*
prediction, because the published figure is what the next step is entitled to
use.

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
exist so the [refusal messages](#what-it-will-not-read) can be shown rather than
described.

No real data is distributed with this project. Point `-c` at your own table, or
produce one with `-t`, before any number here is worth reading.

## Where the model is wrong

Every number in the fit summary is one figure for the whole sample, so none of
them can see structure within it, and `cond=` does not look at the response at
all. The residuals are where a wrong shape is written, and Anscombe's set II
above is what one looks like: an arch, negative at both ends and positive
through the middle, summing to zero as least squares guarantees. No summary of
those eleven numbers can see it. Only their order can.

`--residuals` writes them, and the pass that writes them runs three checks.

**Per term, for a curve.** The correlation between the residual and the part of
the term's square that the fit has not already used, and separately its cube,
since a cubic bend is invisible to a square. On `example/curve.csv`, an exact
parabola fitted with a line:

    $ ./linearr -t example/curve.csv --residuals r.csv
    residuals: r.csv
    reading: column 1 is the group, 'value' is the value being predicted, and the other 1 column is a term. Use -y NAME if that is the wrong column
    fit: 13 rows, R2=0.0000, resid SD=13.49, df=11
    warning: in group A the residuals still depend on x after the line is subtracted (t=9999.0). A straight line is probably the wrong shape in that term; consider adding its square as a column.
    # response: value
    group,intercept,x
    A,24,0

The fit succeeds and the check names the term. The sign is the direction of the
curve, and 9999 is a cap meaning the relation is exact rather than merely
strong.

**On the fitted value.** The same probe against the prediction itself, which is
how an interaction between two terms shows up when no single term looks bent.

**On the spread.** How much of the squared residual's own variation the
prediction and its square account for: the score test `bptest` and
`car::ncvTest` use, reported as the square root of its F so it sits on the same
scale as the others. It was a linear correlation of the residual's SIZE against
the prediction, and a linear correlation cannot see an error that grows
symmetrically about the middle of the range, which is the textbook picture of
the thing. Measured on 200 correctly specified fits it produces no warning, and
it now catches both the monotone and the symmetric case.

None is a hypothesis test and none reports a p-value. Each is a correlation
turned into a t statistic, reported when |t| passes 3.5. Nothing is reported
below ten rows, or when the residuals are already negligible against the
response's own spread; that second guard exists because `example/routes.csv`
fits to 1e-7 and correlating rounding error against anything measures the
floating point unit.

**What to do about a warning.** The program cannot add a column for you. When a
term is named as curved, add its square to the training file with whatever wrote
the file, call it `x2`, and fit again; the header names the terms, so nothing
else changes. When the error grows with the prediction, the usual answers are to
model the logarithm of the response or to weight the rows, and this program does
neither. That is where R or Python is the right tool.

**Where the checks are wrong.** The t statistic assumes the rows are
independent. On a series in time, or repeat measurements of the same subject,
it is too large: over 100 correctly specified fits of 300 rows, independent
noise produced no warning and AR(1) noise at rho=0.85 produced 20. On ordered
data, read a curvature warning as a reason to look at the residual file, not as
a conclusion.

**Per-group figures, not just the worst of each.** The summary reports the
least df, the worst residual SD and the worst conditioning over the whole file,
which for 580 groups is three numbers and no way to tell which group they came
from. `--stats` writes the table:

    $ ./linearr -t example/routes.csv --stats -
    reading: column 1 is the group, 'minutes' is the value being predicted, and the other 2 columns are terms. Use -y NAME if that is the wrong column
    fit: 3 groups, 18 rows, worst R2=1.0000, worst resid SD<6.994e-07, least df=3, worst cond=1.02 (normal equations)
    # response: minutes
    group,intercept,km,stops
    city,5,3,2
    group,rows,df,r2,resid_sd,cond,pinned
    city,6,3,1.000000,<6.99383906911e-07,1.02103,0
    suburb,4,2,1.5
    suburb,6,3,1.000000,<4.73703684717e-07,1.02103,0
    highway,8,1,5
    highway,6,3,1.000000,<3.82817762454e-07,1.02103,0

R gets this from `broom::glance` over a `split`; here it is one flag and one
pass.

**Two passes, and the second one is why the checks are trustworthy.**
`--residuals` reads the training file again rather than keeping a copy: the fit
forgets each row as it reads it, so the rows have to be read a second time to
be subtracted from. Memory stays a function of the model, and a pipe is refused
because it cannot be rewound.

The second pass gets something the first cannot have. Each probe takes its
term's powers about a centre, and that centre is the term's mean **from the
completed fit**. A single pass would have to guess it from the first row, and
that is what this used to do: an outlier arriving first became the centre, and
the same 201 rows in a different order gave t=9999 in one order and t=78.8 in
the other. A running mean is not an alternative, because moving the centre
means re-normalising every power sum already accumulated, six per row, through
a binomial expansion. Two passes make it free.

Both passes report progress on a long run, separately, since the second is a
second run over the same rows:

    fitting:   314572800 rows in 1m 4s, 4.91M rows/s
    residuals: 104857600 rows in 1m 2s, 1.69M rows/s

Nothing that finishes inside a minute prints either.

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

**Both are checked against answers computed by somebody else.** The NIST
reference values for Norris and Longley come back to eleven digits, and R's
`lm()` agrees to 1e-11 on most examples and 1e-6 on all of them. `lm()` solves
by a different method, so the agreement is evidence rather than the same
arithmetic checked twice. `make check` runs both comparisons.

Where each solver loses digits, what the certified figures are term by term,
and what a long run costs in accuracy: [`doc/NUMERICS.md`](doc/NUMERICS.md).

## Scale

Two properties this section exists to test. **The terms are not compiled in:**
the header line of your CSV names them, so adding a term to the polynomial is
adding a column to a file, and the same binary fits a 24-term model and a
2-term one. **Memory is a function of the model, not of the data:** rows are
accumulated into centered cross-products one at a time and then forgotten, and
nothing on the row path allocates.


**2,000,000 rows by 8 terms, a 46 MB file, fitted in 0.42 s using 2.4 MB**, and
the same through a pipe rather than a file. Nothing lands on disk.

**The row count is bounded by time, not by memory.** A row is folded into the
cross-products and dropped, so the tenth row and the ten-billionth cost the same
space. Measured at 10,000,000 rows: 2.04 s, 2.0 MB, the same memory as at two
million. That is 4.9 million rows a second.

The same file fitted by a streaming Python and by R, so the extrapolation has
something to be compared against: Python reads 215,000 rows a second in 10 MB,
and R's `read.csv` plus `lm()` reads 570,000 a second with a peak of 3.2 GB,
about 318 bytes a row, which is what decides where its column ends. That peak
is the parser's high-water mark, not what the frame occupies: `object.size()`
on the resident frame is about 48 bytes a row, so a machine that can tolerate
the transient holds far more rows than the table's ceiling suggests.

| rows | linearr | Python, streaming | R, `read.csv` + `lm()` |
| --- | --- | --- | --- |
| 10 million | 2 seconds | 47 seconds | 18 seconds, 3.2 GB |
| 100 million | 20 seconds | 8 minutes | 3 minutes, 32 GB |
| a billion | 3.5 minutes | 1.3 hours | 318 GB, will not fit |
| a trillion | 2.4 days | 54 days | will not fit |
| **10 trillion** | **24 days** | **1.5 years** | **will not fit** |

The R column stops at about 200 million rows on a machine with 64 GB if the
peak is what has to fit, and around 1.3 billion if only the resident frame
does. Either way it is memory that ends it, not the clock, and the two
streaming columns only get slower. `bench/fit.R` is also the slow variant: it
rescans the whole frame per group and rebuilds the formula in the loop, so a
`scan()` plus `.lm.fit()` version is roughly twice as fast at 580 groups.

**When the residual SD is a bound, it says so.** The default solver recovers
the residual as `Cyy - b'Cxy`, a subtraction of two nearly equal numbers, and
below a floor of about `|mean(y)| * eps * sqrt(n * Cyy)` the difference has no
digits left. Where that happens the figure is printed as `resid SD<0.0005`
rather than `=`: an upper bound that contains the truth, instead of a value
that may not. On 200 rows of an exact quadratic fitted with a line, with x near
1e5, it used to print `resid SD=0`, a claim of a perfect fit on data the line
misses by 0.37. `--qr` carries the residual through the rotation and does not
pay this at all.

What a long run costs in accuracy, measured against the same accumulation
carried in long double, is in [`doc/NUMERICS.md`](doc/NUMERICS.md).

**A long run says where it has got to.** After the first minute, and once a
minute after that, a line goes to stderr:

    fitting: 314572800 rows in 1m 4s, 4.91M rows/s

Nothing that finishes inside a minute prints one, so an ordinary fit is as
quiet as it was. `--residuals` reads the file a second time and reports that
pass separately, since it is a second run over the same rows.


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
and [What it will not read](#what-it-will-not-read).

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
| [`doc/NUMERICS.md`](doc/NUMERICS.md) | what each solver does, where each loses digits, the certified figures term by term, and what a long run costs in accuracy |
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

