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
    ./linearr -c model.csv --no-trim A x=3    # score a case against it

## What it is, and what it is not

Three things it does that a small OLS implementation usually does not:

**The arithmetic is checkable, not asserted.** It reproduces the NIST
Statistical Reference Datasets to eleven digits, and agrees with R's `lm()` to
1e-11 under `--qr`. Both run in `make check`, not in a paragraph:

    $ sh scripts/r-check.sh
    file                         --qr vs lm() default vs lm()
    example/anscombe.csv         1.51e-12     1.51e-12
    example/curve.csv            1.32e-08     1.05e-07
    example/longley.csv          4.97e-12     3.48e-12
    example/nearly-the-same.csv  1.30e-11     1.13e-05
    example/norris.csv           4.53e-13     4.53e-13
    example/routes.csv           3.20e-15     3.20e-15
    example/simple-train.csv     1.18e-15     1.18e-15
    example/three-rows.csv       6.66e-16     6.66e-16
    example/together.csv         4.44e-16     4.44e-16
    example/train.csv            4.25e-15     4.25e-15
    example/wampler1.csv         3.35e-10     4.53e-09
    R: 11 agreed with lm() to 1e-6 under --qr, 0 differed, 5 not a training file

`lm()` solves by QR with column pivoting, a different method from the default
here, so agreement is evidence rather than a tautology. The one file where the
two columns differ by six orders of magnitude is the one that exists to show
what squaring `X'X` costs. See [Checked against answers somebody else
certified](#checked-against-answers-somebody-else-certified).

**It reads the residuals, not just the summary.** R2 and a residual SD are
averages over the residuals, so neither can see structure *in* them. This names
the term whose square explains what is left, tests the fitted value for a
missing interaction, and tests whether the error grows with the prediction:

    warning: in group II the residuals still depend on x after the line is
    subtracted (t=-2219.2). A straight line is probably the wrong shape in that
    term; consider adding its square as a column.

It also names the three ways a fit succeeds and misleads: two columns the data
cannot separate, a line with no residual freedom, and a design whose trailing
digits are noise. See [Where the model is wrong](#where-the-model-is-wrong).

**Memory is bounded by the model, not by the data.** Rows are read one at a time
and forgotten, so a 40 MB file and a 4 GB file cost the same. What memory does
scale with is the number of GROUPS, one accumulator each, and `--footprint`
prints the figure rather than leaving you to trust a sentence:

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

    Neither depends on the number of ROWS, which is the point:
    the same figures cover a thousand rows and a trillion.

### What it does not do

Said here rather than discovered later.

- **No inference.** No standard errors, no confidence or prediction intervals,
  no p-values, no cross-validation, no regularization. The residual SD is
  reported and is an in-sample figure. If you need any of that, `statsmodels`
  and R are the right tools and this is not competing with them.
- **One thread, one stream.** No sharding, no parallelism, no restart from a
  partial fit. At roughly 4.7 million rows a second it is bound by parsing
  text, not by arithmetic, so a second core would help more than a faster
  solver would.
- **A comma splitter, not a CSV parser.** No quoting, no embedded line breaks,
  no separator but the comma, no missing values, no categorical columns. Each
  is refused with a message naming the row, the column and the reason; see
  [What it will not read](#what-it-will-not-read).
- **A fixed column order.** Column 1 the group, column 2 the value being
  predicted, the rest terms. Nothing in the data can say which is which, so the
  program prints what it took on every fit.

## All of least squares, in three short pieces

**What it does.** Ordinary least squares (OLS), which is linear regression
fitted by minimising the sum of squared residuals. You have rows: some
measurements, and a number you care about. It finds the straight line through
them that misses by as little as possible, squared, so that a miss of 2 counts
four times a miss of 1. The coefficients it
returns are how much each measurement moves the answer. That is the whole
method; it dates from Legendre and Gauss around 1805 and remains a reasonable
choice wherever the relationship is close to linear.

Three situations are worth knowing about, because in each of them the fit
succeeds and the result is not what it appears to be. The program reports all
three.

**One: the data cannot tell two columns apart.** If `night` and `headlights`
are 1 on exactly the same rows, because no journey had one without the other,
the data can say the pair adds 8 minutes. It cannot say how to divide those 8
between them. Every division fits equally well, so there is nothing to
determine:

    $ ./linearr -t example/together.csv
    reading: column 1 is the group, 'minutes' is the value being predicted, and the other 2 columns are terms
    fit: 1 group, 5 rows, 1 term-slot pinned to 0, least df=3, worst resid SD=4.397
    # response: minutes
    group,intercept,night,headlights
    A,6,8,0
    # pinned A: collinear headlights

All 8 is assigned to the first column and the second is set to 0, with a note
recording that this happened. Without the note the 0 would be indistinguishable
from an estimate that headlights make no difference. A column that never varies at all is a
different verdict, and the program prints it as `constant` rather than
`collinear`: with a collinear pair there is an effect that cannot be attributed,
and with a constant column there is nothing to attribute.

**Two: no degrees of freedom left.** Three rows and three unknowns will fit perfectly,
the way two points always define a line exactly. It would fit perfectly on any
numbers whatsoever, so a perfect fit tells you nothing:

    $ ./linearr -t example/three-rows.csv
    reading: column 1 is the group, 'minutes' is the value being predicted, and the other 2 columns are terms
    fit: 1 group, 3 rows, least df=0, worst cond=1.33 (normal equations)
    warning: at least one group has no residual degrees of freedom; its line passes through every row by construction. Fit those groups on more rows.
    # response: minutes
    group,intercept,km,stops
    A,1,3,6

*Degrees of freedom* is rows minus the parameters the data could actually
identify, and it is the amount of disagreement the fit had to accommodate. Not
rows minus unknowns: the first example above has 5 rows and 3 unknowns and
reports `df=3`, because one of its terms was pinned and never estimated. At zero there is none, so the quality
of the fit is not evidence of anything. Six rows against three unknowns leaves
three rows' worth, and it is that residual disagreement which makes a small
error informative.

**Three: the arithmetic squares the data before solving.** Keeping the sums of
products rather than the rows is what bounds the memory, and it costs precision:
squaring roughly halves the significant digits available. With two columns that
differ in the sixth decimal, asked for `1 + 2*x1 + 3*x2`:

    $ ./linearr -t example/nearly-the-same.csv
    reading: column 1 is the group, 'value' is the value being predicted, and the other 2 columns are terms
    fit: 1 group, 40 rows, least df=37, worst resid SD=0, worst cond=5.34e+10 (normal equations)
    warning: at least one group is ill-conditioned (cond=5.34e+10); the trailing digits of its coefficients are noise. Try --qr, which does not square the condition number.
    # response: value
    group,intercept,x1,x2
    A,1.00000000001,2.00002262993,2.99997737008

It returns 2.00002262993 and 2.99997737008 where the true values are 2 and 3,
a loss of about five significant digits, and it reports the fact. Those trailing
digits are not reproducible across platforms either: the same source on macOS
returns 2.00015811817, because the two maths libraries round differently and
this design amplifies the difference. That is what an ill-conditioned fit is. A method that does not
form the cross-products first, such as QR or SVD, would separate the two
columns. R2 does not detect this: an ill-conditioned fit still describes its own
training sample closely, so its in-sample error stays small while its
predictions do not. The `cond=` figure is the diagnostic, and a warning is
printed above 1e8.

## The textbook case: Anscombe's quartet

Anscombe built four sets of eleven points in 1973 so that every summary you
would normally quote is the same for all four, while the four are nothing alike.
It is the standard demonstration that a fitted line and an R2 do not describe a
dataset. Fitting all four at once is what groups are for:

    $ ./linearr -t example/anscombe.csv
    reading: column 1 is the group, 'y' is the value being predicted, and the other 1 column is a term
    fit: 4 groups, 44 rows, least df=9, worst resid SD=1.237
    # response: y
    group,intercept,x
    I,3.00009090909,0.500090909091
    II,3.00090909091,0.5
    III,3.00245454545,0.499727272727
    IV,3.00172727273,0.499909090909

The same line four times, to two decimals, which is as exactly as Anscombe
constructed them; this program prints twelve significant digits, so the small
differences are visible here and would not be in most tools. Each set on its own
(`-g I` and so on) gives `R2=0.666`, `resid SD=1.24`, `df=9`. That is Anscombe's
point, and it is the argument for reading residuals: the summaries agree and
only one of the four is a straight line with scatter around it.

    $ ./linearr -t example/anscombe.csv --residuals r.csv
    residuals: r.csv
    reading: column 1 is the group, 'y' is the value being predicted, and the other 1 column is a term
    fit: 4 groups, 44 rows, least df=9, worst resid SD=1.237
    warning: in group II the residuals still depend on x after the line is subtracted (t=-2219.2). A straight line is probably the wrong shape in that term; consider adding its square as a column.
    # response: y
    group,intercept,x
    I,3.00009090909,0.500090909091
    II,3.00090909091,0.5
    III,3.00245454545,0.499727272727
    IV,3.00172727273,0.499909090909

Set II is an exact parabola, and it is named. **Sets III and IV are not
reported, and should not be taken as passing.** III is a perfect line with one
point moved off it; IV is a vertical stack of ten identical x values with one
point far to the right, which alone decides the slope. Neither is a wrong shape:
both are single points with more influence than the other ten together, and this
program has no measure of leverage or influence to find them with. It says so
here rather than leaving the silence to be read as approval.

### What the residuals of set II actually look like

The warning is a number. The residuals are the evidence, and set II is the case
every course uses to show what a line cannot do. Fit that set alone and keep
them:

    $ ./linearr -t example/anscombe.csv -g II --residuals r.csv
    residuals: r.csv
    reading: column 1 is the group, 'y' is the value being predicted, and the other 1 column is a term
    fit: 1 group, 11 rows, least df=9, worst resid SD=1.237
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
one case it is built for and is honest about the two it is not.

## The three files, and what a group is

**A group is one fitted line.** Rows sharing a group code are fitted together
and get their own coefficients; a different code gets different ones. Groups
exist so that one file and one pass produce one model per subset, instead of
splitting the file and running the program once per part. With a single group
this is plain least squares.

`example/routes.csv` is delivery time against distance and stops on three kinds
of route. The terms are the same everywhere; what each term is worth is not:

    $ ./linearr -t example/routes.csv
    reading: column 1 is the group, 'minutes' is the value being predicted, and the other 2 columns are terms
    fit: 3 groups, 18 rows, least df=3, worst resid SD=3.832e-07, worst cond=1.02 (normal equations)
    # response: minutes
    group,intercept,km,stops
    city,5,3,2
    suburb,4,2,1.5
    highway,8,1,5

Three minutes per kilometre in the city, one on the highway; a stop costs two
minutes in the city and five on a highway route. Pooling the same eighteen rows
into one line gives an average of the three that describes none of them:

    $ ./linearr -t example/routes.csv -g '*'
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
    reading: column 1 is the group, 'minutes' is the value being predicted, and the other 2 columns are terms
    fit: 2 groups, 13 rows, least df=3, worst resid SD=0, worst cond=1.03 (normal equations)
    # response: minutes
    group,intercept,km,stops
    A,5,2.5,1.5
    B,12,2.5,1.5

Read that line once and the mistake is visible immediately. The same name is
written into the coefficient file as `# response: minutes`, so a table found a
year later still says what it predicts.

## What it will not read

The reader is a comma splitter, not a CSV parser, and the distance between
those is worth stating rather than discovering. Each of the following is
refused, with a status of 1 and a message naming the row, the column and the
reason. None of them is a crash, and none of them produces a partial model.

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

## Where the model is wrong

The coefficients describe the model; the residuals show where it does not fit,
which no single summary number can. Fitting a parabola with a straight line
produces summary statistics that give no sign of the problem:

    $ ./linearr -t example/curve.csv --residuals r.csv
    residuals: r.csv
    reading: column 1 is the group, 'value' is the value being predicted, and the other 1 column is a term
    fit: 1 group, 13 rows, least df=11, worst resid SD=13.49
    warning: in group A the residuals still depend on x after the line is subtracted (t=9999.0). A straight line is probably the wrong shape in that term; consider adding its square as a column.
    # response: value
    group,intercept,x
    A,24,0

    $ cat r.csv
    group,observed,predicted,residual
    A,46,24,22
    A,35,24,11
    A,26,24,2
    A,19,24,-5
    A,14,24,-10
    A,11,24,-13
    A,10,24,-14
    A,11,24,-13
    A,14,24,-10
    A,19,24,-5
    A,26,24,2
    A,35,24,11
    A,46,24,22

The residuals are positive at both ends and negative in the middle. That is a
systematic pattern rather than scatter, and it indicates the model has the wrong
shape. R2 and the residual SD are each one figure for the whole sample, so neither can
show it, and `cond=` never looks at the response at all.

It costs a second pass over the training file rather than a copy of it in
memory: the fit forgets each row as it reads it, so the rows have to be read
again to be subtracted from. Memory stays a function of the model.

## Two solvers

The fit accumulates `X'X` and solves it. That is what bounds the memory, and it
squares the condition number of the design, so a near-collinear or badly scaled
problem loses about twice the digits it needs to. `--qr` rotates each row into a
triangular factor instead, with Givens rotations, one row at a time. It squares
nothing. It is still streaming. Its factor is `p^2 + 6p + 5` doubles against the
normal equations' `p^2 + 2p`, so it is LARGER by `4p + 5`, not smaller. At 24
terms that is 725 doubles against 624.

On `example/nearly-the-same.csv`, where two columns differ in the sixth decimal
and the answer is `1 + 2*x1 + 3*x2`:

    normal equations   A,1.00000000001,2.00002262993,2.99997737008
    --qr               A,1,1.99999999974,3.00000000026

Five correct digits against ten. Both report `cond=`, and neither figure is a
condition number in the textbook sense: each is a ratio of pivots, on
differently scaled matrices, meant as an order-of-magnitude alarm. They are not comparable
to each other, which is why the summary names the solver, and no fixed
relationship holds between them: on the example above they are 5.34e+10 and
2.78e+06.

`--qr` is not a strictly better solver. It does not centre the data, and the
first version of it deleted a well-identified column for being measured in a
small unit, which is the defect the default solver documents as fixed. Columns
are scaled before the rank test now, and the figures that depend on a dropped
column (`R2`, `resid SD`) are withheld rather than reported. The cost measured
at 300k rows by 20 terms is about 7 percent, not the larger penalty an earlier
version of this section implied.

The algorithm is Gentleman's 1974 row-wise updating QR, which R's `biglm` has
used for two decades. Nothing about the method is new here.

## Checked against answers somebody else certified

Every test a project writes for itself checks the code against arithmetic the
same project wrote. If the solver and the expected value came from the same
understanding, they are wrong together and the suite stays green.

So the suite also fits datasets published with their answers. These are from the
NIST Statistical Reference Datasets, computed to fifteen digits, a US Government
work and not under copyright. Statisticians know them.

**Norris** is the easy one, and it is here because a suite that only tests hard
cases does not notice when something ordinary breaks. Thirty-six calibration
points, one term:

    $ ./linearr -t example/norris.csv
    reading: column 1 is the group, 'y' is the value being predicted, and the other 1 column is a term
    fit: 1 group, 36 rows, least df=34, worst resid SD=0.8848
    # response: y
    group,intercept,x
    A,-0.262323073774,1.00211681802

The certified values are -0.262323073774029 and 1.00211681802045. Twelve digits
agree, which is every digit printed.

**Longley**, 1967: sixteen years of US macroeconomic series, employment against
six predictors. Longley published it because the regression programs of the day
returned as few as two correct digits on it, and it has been the standard hard
case since.

    $ ./linearr -t example/longley.csv
    reading: column 1 is the group, 'employment' is the value being predicted, and the other 6 columns are terms
    fit: 1 group, 16 rows, least df=9, worst resid SD=304.9, worst cond=934 (normal equations)
    # response: employment
    group,intercept,deflator,gnp,unemployed,armed_forces,population,year
    A,-3482258.6346,15.0618722714,-0.0358191792926,-2.02022980382,-1.03322686717,-0.0511041056535,1829.15146461

The certified intercept is -3482258.63459582 and the certified coefficient on
year is 1829.15146461355. Every one of the seven agrees to eleven digits, which
is as many as the output prints. The plain solver manages Longley because it
accumulates centered co-moments rather than raw cross-products; on the raw ones
this set is the textbook catastrophe.

**Wampler1** is the case that separates the two solvers. It is y = 1 + x + x2 +
x3 + x4 + x5 for x from 0 to 20, in whole numbers, with the powers supplied as
columns. Every certified coefficient is 1 and the certified residual is exactly
0, so anything other than that is the solver's own error with nothing in the
data to hide behind.

    $ ./linearr -t example/wampler1.csv
    reading: column 1 is the group, 'y' is the value being predicted, and the other 5 columns are terms
    fit: 1 group, 21 rows, least df=15, worst resid SD=0.02282, worst cond=9.96e+04 (normal equations)
    # response: y
    group,intercept,x,x2,x3,x4,x5
    A,0.999999995576,0.999999996707,1.0000000034,0.999999999361,1.00000000004,0.999999999999

    $ ./linearr -t example/wampler1.csv --qr
    reading: column 1 is the group, 'y' is the value being predicted, and the other 5 columns are terms
    fit: 1 group, 21 rows, least df=15, worst resid SD=6.663e-11, worst cond=234 (QR)
    # response: y
    group,intercept,x,x2,x3,x4,x5
    A,1.00000000044,0.999999999992,1.00000000001,0.999999999997,1,1

The true residual SD is zero. The normal equations report 0.0228 and QR reports
7e-11. Neither is lying about its own arithmetic: x5 times x5 reaches 1e16 and a
double has no places left to keep the difference. QR never forms that product.
This is the whole argument for `--qr`, on data a reader can check.

Both solvers are held to these numbers by `make check`, and the example files
are held to them separately, so neither the code nor the data can drift alone.

## When a straight line is the wrong shape

Every number in the fit summary is one figure for the whole sample, so none of
them can see structure WITHIN it, and that is where a wrong shape is written.
(`cond=` does not even look at the response: it is a property of the columns
alone.) With `--residuals` the pass that writes them also checks two things.

    $ ./linearr -t example/curve.csv --residuals r.csv
    residuals: r.csv
    reading: column 1 is the group, 'value' is the value being predicted, and the other 1 column is a term
    fit: 1 group, 13 rows, least df=11, worst resid SD=13.49
    warning: in group A the residuals still depend on x after the line is subtracted (t=9999.0). A straight line is probably the wrong shape in that term; consider adding its square as a column.
    # response: value
    group,intercept,x
    A,24,0

That file is a parabola. The fit succeeds, and the check names the term to look
at rather than only reporting that something is wrong. The sign is the direction
of the curve, and 9999 is a cap meaning the relation is exact rather than merely
strong.

There are three checks, not one. The first probes each term for a curve, trying
both its square and its cube, since a cubic bend is invisible to a square. The
second probes the fitted value itself, which is how an interaction between two
terms shows up when no single term looks bent. The third compares the size of
the error with the size of the prediction: when the error grows, the residual SD
is not a typical error at either end of the range.

None is a hypothesis test, and none reports a p-value. Each is a correlation
turned into a t statistic, reported when |t| passes 3.5, and nothing is reported
below ten rows or when the residuals are already negligible against the
response's own spread. That last guard exists because `example/routes.csv` fits
exactly, to 1e-7, and correlating rounding error against anything measures the
floating point unit.

**What to do about a warning.** The program cannot add a column for you. When it
says a term is curved, add its square to your training file with whatever wrote
the file, name it `x2` or similar, and fit again; the header names the terms, so
nothing else has to change. When it says the error grows with the prediction,
the usual answers are to model the logarithm of the response or to weight the
rows, and this program does neither, which is the point at which R or Python is
the right tool.

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

## Two properties

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

      Neither depends on the number of ROWS, which is the point:
      the same figures cover a thousand rows and a trillion.

Ten times the data, the same memory: that is the first pair of rows. The second
pair is the part a memory claim usually omits. Fitting 580 groups instead of one
costs 8.5 MB more, and the per-group figure printed underneath predicts 8.4 MB,
so the script measures the claim rather than restating it. Time scales with the
number of rows, memory with the number of groups.

## The same job in other languages

Read the file, fit a line per group, write the table. `scripts/bench.sh` checks
each one's coefficients against linearr's before timing it; an unchecked speed
number may be timing a different answer. All agree to the printed digit: one
answer, different prices.

    $ sh scripts/bench.sh 8 50 500000        # 8 terms, 50 groups, 500k rows

    implementation   shape        time     peak RSS   check
    linearr (C)      streaming    0.11s    2432 KB    agrees to 0
    Java             streaming    0.28s    105612 KB  agrees to 0
    Python           streaming    2.37s    10240 KB   agrees to 0
    awk              streaming    10.47s   5632 KB    agrees to 0
    Python           frame        3.01s    315904 KB  agrees to 0

All but the last read the file one row at a time, which each of these languages
permits. Writing the C as a stream and the Python with pandas would compare two
styles rather than two languages, so the materialising row is also Python: the
same language, machine and algorithm, with one variable changed. Over a tenfold
increase in rows the streaming figures are unchanged and that row rises from
41 MB to 316 MB.

The JVM's number is mostly the JVM. Capping its heap separates the runtime's
appetite from what the algorithm needs: 500,000 rows fit in a 16 MB heap at
the same speed:

    -Xmx16m   0.26s  61976 KB
    -Xmx64m   0.27s  89644 KB

`bench/fit.R` is the ecosystem case, and it says so in its own header: `read.csv`
materialises the frame because that is R's idiom. It SKIPs unless R is
installed.

## Two implementations

`java/` fits the same model in the style this project's C came from: a
`BufferedReader` and a `readLine` loop, `Hashtable`, `Vector`, `StringBuffer`,
and one reused record rather than one object per row. `Regress.java` follows
`regress.c` closely enough to be read beside it: the same centered accumulation,
the same equilibrated rank test, the intercept recovered from the means. The two
produce identical coefficient files.

    cd java && ant jar          # or: javac -nowarn *.java
    java Linearr ../example/train.csv

Reusing the record instead of allocating one per row is an optimisation from
when its author started writing Java, and it is still the reason the memory
column is flat. `String.split()` in that loop allocates an array and a string
per field; over 500,000 rows that is five million short-lived objects, and the
heap that grows to hold them gets read as "Java needs 400 MB for this". It does
not.

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
    reading: column 1 is the group, 'minutes' is the value being predicted, and the other 2 columns are terms
    fit: 2 groups, 13 rows, least df=3, worst resid SD=0, worst cond=1.03 (normal equations)

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
the redirect above is the whole workflow.

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

**`-c` is required for scoring.** There is no default table and no search. Until
recently there was: a `system.properties` file, and failing that a model shipped
beside the binary, so a bare `linearr 001 x=1` answered from a table the reader
had never seen, and the same command in two directories could give two different
answers with nothing saying which. A model is the whole of what an answer means.
It is not something to find by convention.

When something is wrong, the message says what:

    $ ./linearr -c example/coefficients.csv --trim example/trim_additions.csv 001 nosuchterm=1
    cannot score group '001': no term 'nosuchterm' in example/coefficients.csv; run --terms to list them

`./linearr -h` prints the options; `-d` traces to stderr.

## Options instead of a configuration file

There is no configuration file. Everything is an option, which is one place to
look rather than two, and no file that has to be found before it can be read:

| option | default | meaning |
| --- | --- | --- |
| `-c FILE` | *(required to score)* | the fitted model: `group,intercept,<one column per term>` |
| `--trim FILE` | *(none)* | `group,trim_addition`. Without it the trim point is the prediction |
| `--no-trim` | | says the same thing explicitly |
| `--scale N` | 4 | decimal places the prediction is rounded to, 0 to 9 |
| `--trim-scale N` | 1 | decimal places the trim point is rounded to, 0 to 9 |

This replaced a `system.properties` file with four keys, all of which duplicated
an option, in a directory called `conf` that held no configuration and two data
files. Two of the keys behaved differently from what the file itself documented:
commenting out `trim.file` was said to turn the trim off and did not (an absent
key meant the built-in default, so the table loaded), and `predict.scale = 99`
was accepted and quietly gave four decimals. `--scale 99` is an error.

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

**One model that was actually deployed.** `coefficients.csv`, `trim_additions.csv`,
`train.csv` and `cases.csv` are the 24-term shape of a length-of-stay model the
author ran in production in 2011, with the terms kept and the data replaced.
Its purpose is different from the sets above and it is not a substitute for
them: it shows the program at a width and a shape that came from a real
problem rather than from a book, including a trim table, twelve groups, and
term names nobody would invent for an example. The numbers in it are
**generated**, chosen so that fitting `train.csv` returns exactly the
coefficients in `coefficients.csv`. They are fitted to nothing and mean nothing.

**Files built to fail in one specific way**, each used by a teaching section:

| file | the failure it shows |
| --- | --- |
| `together.csv` | two columns the data cannot tell apart |
| `three-rows.csv` | a line with no residual degrees of freedom |
| `nearly-the-same.csv` | a design whose trailing digits are noise, and what `--qr` does about it |
| `curve.csv` | a parabola fitted with a straight line |
| `simple-train.csv`, `routes.csv` | the smallest honest fit, and why groups exist |
| `gaps.csv`, `semicolons.csv` | input the reader refuses, and what it says |

No real data is distributed with this project. Point `-c` at your own table, or
produce one with `-t`, before any number here is worth reading.

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

The author's work in industry was largely this: taking models written by
scientists in SAS, R and Matlab and turning them into C, or into plain Java
without frameworks, so that they could run where the original could not. The
view behind that work, and behind this program, is that computation belongs as
close to the processor and to the memory it touches as the problem allows;
distance from it costs time, energy and hardware that a straight line does not
need.

The second view is about shape rather than language: that data of any size
should be processed as a stream. Every one of the author's projects is written
that way, this one included. A stream has one property that matters more than
speed, which is that the size of the input stops being a design question. There
is no point at which the file no longer fits, no partitioning step, no
out-of-memory failure at the end of a long run, and no difference in the code
between the small case and the large one. The cost of it is real and is stated
below: one pass, one core, and anything needing a second look at the data needs
a second pass.

That is a claim about implementation, not about tools. **This does not replace
Python, R, SAS or Matlab, and is not trying to.** Those are where a model should
be explored, chosen, tested and argued about, and they have decades of
statistics behind them that this has not. What this offers is a small, checkable
implementation of one method, useful in three places: as something to check an
implementation against, since it reproduces the NIST certified values to eleven
digits (see [Checked against answers somebody else
certified](#checked-against-answers-somebody-else-certified)); on embedded and
small ARM targets where no interpreter is going to be installed; and in cloud
batch work, where the memory a process holds is what it costs.

An earlier version of this paragraph said it agreed with R's `lm()` to the
printed digit. Nothing here tested that, no gate could fail if it stopped being
true, and a reviewer repeated it back as a property of the test suite. The NIST
sets are the claim that is actually checked, on every run of `make check`. If
you have R, the comparison takes three lines and you should not take this file's
word for it:

    d <- read.csv("example/longley.csv", comment.char = "#")
    print(coef(lm(employment ~ deflator + gnp + unemployed +
                  armed_forces + population + year, data = d)), digits = 12)

Both architectures the CI builds on are covered on every push: `x86_64` on
Linux and `arm64` on macOS, each running the full suite under AddressSanitizer
and UndefinedBehaviorSanitizer. That is evidence for arm64 generally; it is not
evidence for a microcontroller, which has no operating system to run these tests
on. The code is plain C99 with a few POSIX calls, so a small target is a
question of the toolchain rather than of the source.

Measured for the third case: **2,000,000 rows by 8 terms, a 46 MB file, fitted
in 0.42 s using 2.6 MB of memory**, and the same through a pipe rather than a
file. Nothing lands on disk, and the footprint does not depend on how much data
arrives.

**The row count is bounded by time, not by memory.** A row is folded into the
cross-products and dropped, so the tenth row and the ten-billionth cost the same
space. Measured at 10,000,000 rows: 1.29 s, 2.5 MB, the same figure as at two
million. At that rate, 7.75 million rows a second:

| rows | time on one core |
| --- | --- |
| a billion | about 2 minutes |
| a trillion | about a day and a half |
| **100 trillion** | **about five months** |

A hundred trillion rows is where the table stops, because that is roughly what
one core gets through in half a year and it is already more than the question
usually is. Past it the answer stops being about memory and becomes about
scheduling. The counters are 64-bit, so much larger numbers are representable;
quoting them would be quoting arithmetic rather than a run anybody would start.

Two caveats on long runs, both real: the cross-products accumulate over the
whole stream, so at these lengths their last digits decay even though the means
are updated stably; and the check that the model has the right shape holds one
accumulator per group, which is memory in the groups, not in the rows.

### The limitations of that, stated

- **One core.** No threading, no vectorisation beyond what the compiler finds.
  A parallel implementation would beat it on a machine with cores to spare.
- **One stream.** It reads one input sequentially. There is no sharding, no
  distribution, no restart from a partial fit.
- **Memory is bounded by the MODEL, not by the data, and the model includes the
  groups.** Fitting every group in one pass holds one accumulator per group.
  Ask the program rather than trusting this sentence:

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

      Neither depends on the number of ROWS, which is the point:
      the same figures cover a thousand rows and a trillion.

  Rows are free, groups are not. The two figures are different and used to be
  quoted as one: this section said 2 KB per group for fitting, which is the
  scoring number, and three other places in the project each said something
  else. They now all come from `process_group_bytes()`, which is also what the
  allocation calls.
- **`--residuals` needs a second pass**, so it needs a real file. From a pipe it
  refuses rather than half-work.
- **The parsing is the cost, not the arithmetic.** At 4.7 million rows a second
  the program is reading and converting text; a binary input format would be
  faster and does not exist here.
- **No weights, no sparse input, no categorical columns, no missing values.**
  A missing field is an error, not an imputation.
- **Commas only.** No quoting, no embedded separators, no other delimiter.


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

Code under `c/` and `java/`, data at the top level. There is no `conf/`: it held
no configuration, only two data files, and the settings it implied are options
now.


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
  allocate, each bounded by the model or the config and never by the data, and
  each freed on every path: the coefficient table (`los.c`), the config table
  (`params.c`), and one accumulator per group while `-t` fits them all
  (`process.c`). That is the complete list, and it is kept short enough to check.
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

## Platforms, and reporting a bug

Built and tested on Linux and macOS on every push (`.github/workflows/sanitizers.yml`
runs the suite plus AddressSanitizer and UndefinedBehaviorSanitizer on both).
BSD should work and is untested. For Windows see [Windows](#windows): WSL runs
this unmodified, and a native `.exe` links but has not been run.

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

BSD 2-Clause; see `LICENSE`.

Chosen over the GPL deliberately. The uses this is for, checking an R
implementation against it, embedding it on a small target, vendoring
`regress.c` into a C codebase, all mean copying the source into somebody else's
program, and a licence that forbids that works against the reason the project
exists.

