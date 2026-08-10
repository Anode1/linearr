# The numerics

## Three ways a fit succeeds and misleads

**One: the data cannot tell two columns apart.** If `night` and `headlights`
are 1 on exactly the same rows, because no journey had one without the other,
the data can say the pair adds 8 minutes. It cannot say how to divide those 8
between them. Every division fits equally well, so there is nothing to
determine:

    $ ./linearr -t example/together.csv
    reading: column 1 is the group, 'minutes' is the value being predicted, and the other 2 columns are terms. Use -y NAME if that is the wrong column
    fit: 5 rows, R2=0.5697, resid SD=4.397, 1 term unidentified and set to 0, df=3
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
    reading: column 1 is the group, 'minutes' is the value being predicted, and the other 2 columns are terms. Use -y NAME if that is the wrong column
    fit: 3 rows, R2=1.0000, df=0, cond=1.33 (normal equations)
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
    reading: column 1 is the group, 'value' is the value being predicted, and the other 2 columns are terms. Use -y NAME if that is the wrong column
    fit: 40 rows, R2=1.0000, resid SD<3.235e-08, df=37, cond=5.34e+10 (normal equations)
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

## Two solvers, and how accurate each one is

What each solver does, where each one loses digits, and what the answers were
checked against. The short version is in the README; this is the arithmetic
behind it.

The fit accumulates `X'X` and solves it. That is what bounds the memory, and it
squares the condition number of the design, so a near-collinear or badly scaled
problem loses about twice the digits it needs to. `--qr` rotates each row into a
triangular factor instead, with Givens rotations, one row at a time. It squares
nothing. It is still streaming. Its factor is `p^2 + 7p + 6` doubles against the
normal equations' `p^2 + 2p`, so the accumulator is larger by `5p + 6`: at 24
terms, 750 doubles against 624. Counting what a caller actually has to
allocate reverses that, because the elimination needs a `p^2 + p` workspace
that the rotation does not: 750 against 1224 at 24 terms.

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

`--qr` is not a strictly better solver. It does not centre the data, and two
consequences follow. Its rank test needs a looser tolerance and still cannot
separate a dependent column from an independent one once the columns sit near
1e9, where `cond=` reports the trouble but the rank test does not cut. And its
`cond=` is not comparable with the default's, because one describes columns
about their means and the other does not. The cost in time, measured at 300k
rows by 20 terms, is about 7 percent.

The algorithm is Gentleman's 1974 row-wise updating QR, which R's `biglm` has
used for two decades. Nothing about the method is new here.

### Checked against answers somebody else certified

Every test a project writes for itself checks the code against arithmetic the
same project wrote. If the solver and the expected value came from the same
understanding, they are wrong together and the suite stays green.

So the suite also fits the NIST Statistical Reference Datasets, whose certified
values are computed to fifteen digits.

**Norris**, the easy one, here because a suite that only tests hard cases does
not notice when something ordinary breaks:

    $ ./linearr -t example/norris.csv
    reading: column 1 is the group, 'y' is the value being predicted, and the other 1 column is a term. Use -y NAME if that is the wrong column
    fit: 36 rows, R2=1.0000, resid SD=0.8848, df=34
    # response: y
    group,intercept,x
    A,-0.262323073774,1.00211681802

The certified values are -0.262323073774029 and 1.00211681802045. Twelve digits
agree, which is every digit printed.

**Longley**, the standard hard case:

    $ ./linearr -t example/longley.csv
    reading: column 1 is the group, 'employment' is the value being predicted, and the other 6 columns are terms. Use -y NAME if that is the wrong column
    fit: 16 rows, R2=0.9955, resid SD=304.9, df=9, cond=934 (normal equations)
    # response: employment
    group,intercept,deflator,gnp,unemployed,armed_forces,population,year
    A,-3482258.6346,15.0618722714,-0.0358191792926,-2.02022980382,-1.03322686717,-0.0511041056535,1829.15146461

The certified intercept is -3482258.63459582 and the coefficient on year
1829.15146461355. All seven agree to eleven digits, which is as many as the
output prints. The plain solver manages it because it accumulates centered
co-moments rather than raw cross-products.

**Wampler1** separates the two solvers. Every certified coefficient is 1 and the
certified residual is exactly 0, so anything else is the solver's own error with
nothing in the data to hide behind.

    $ ./linearr -t example/wampler1.csv
    reading: column 1 is the group, 'y' is the value being predicted, and the other 5 columns are terms. Use -y NAME if that is the wrong column
    fit: 21 rows, R2=1.0000, resid SD=0.02282, df=15, cond=9.96e+04 (normal equations)
    # response: y
    group,intercept,x,x2,x3,x4,x5
    A,0.999999995576,0.999999996707,1.0000000034,0.999999999361,1.00000000004,0.999999999999

    $ ./linearr -t example/wampler1.csv --qr
    reading: column 1 is the group, 'y' is the value being predicted, and the other 5 columns are terms. Use -y NAME if that is the wrong column
    fit: 21 rows, R2=1.0000, resid SD=6.663e-11, df=15, cond=234 (QR)
    # response: y
    group,intercept,x,x2,x3,x4,x5
    A,1.00000000044,0.999999999992,1.00000000001,0.999999999997,1,1

The true residual SD is zero. The normal equations report 0.0228 and QR
7e-11: x5 times x5 reaches 1e16 and a double has no places left for the
difference, and QR never forms that product. That is the case for `--qr`, on
data a reader can check.

Both solvers are held to these numbers by `make check`, and the example files
are held to them separately, so neither the code nor the data can drift alone.

R is the second reference, and an independent one: `lm()` solves by QR with
column pivoting, which is neither of the methods here, so agreement is evidence
rather than the same arithmetic checked twice.

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

The two columns differ by six orders of magnitude on `nearly-the-same.csv`,
which is the file that exists to show what forming `X'X` costs. `make r` runs
this and skips itself where R is not installed.

Only the `--qr` column is a gate. The default column is printed and not tested,
because squaring the condition number is what that solver does and a threshold
on it would be a threshold on the documented behaviour.


## What a long run costs in accuracy

The cross-products accumulate over the
whole stream, so their last digits decay even though the means are updated
stably. Measured against the same accumulation carried in long double:

| rows | worst relative error in a co-moment |
| --- | --- |
| 100 thousand | 1.2e-13 |
| 1 million | 2.5e-13 |
| 10 million | 6.4e-13 |
| 100 million | 3.0e-12 |

The growth is close to the square root of the row count, so a hundred trillion
rows costs about 4e-9. Three things that table does not say, all of which
change the answer:

- **It was measured on columns centred near the origin.** The same measurement
  at 10 million rows with a column offset of 1e6 gives 3.4e-8, five orders of
  magnitude worse, because `x - mean(x)` is itself a cancelling subtraction.
- **It grows faster than the term count.** At 10 million rows: 1.5e-13 at two
  terms, 6.1e-13 at four, 2.3e-12 at eight, 1.8e-11 at sixteen. That is about
  116x for 8x the terms, between p^2 and p^2.5.
- **A co-moment's error is not the fit's error.** The coefficients come from
  solving with that matrix, so the perturbation is amplified by its condition
  number, and for normal equations that is cond(X)^2. At the conditioning
  `nearly-the-same.csv` reports, 5.34e10, a 4e-9 co-moment error is not nine
  digits of anything.

So read it as a floor on one mechanism, on well-scaled centred data, not as
what a long run costs in general.
