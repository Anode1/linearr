# Reading the residuals

What the three checks compute, what they catch, what they cannot, and what to
do about a warning. The README has the three-sentence version.

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

