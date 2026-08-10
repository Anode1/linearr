# linearr: least squares in C, as simple as the method actually is

### It reports the three ways the fit can mislead you, and the memory does not grow with the file

Reads a CSV and returns the coefficients; reads a case and returns a prediction.
It requires a C compiler and `make`, and nothing else. The training file is read
one row at a time, so its size does not affect how much memory the fit uses.

    make
    ./linearr -t mydata.csv > model.csv       # fit every group in one pass
    ./linearr -c model.csv --no-trim A x=3    # score a case against it

## All of least squares, in three short pieces

**What it does.** You have rows: some measurements, and a number you care about.
It finds the straight line through them that misses by as little as possible,
squared, so that a miss of 2 counts four times a miss of 1. The coefficients it
returns are how much each measurement moves the answer. That is the whole
method; it dates from Legendre and Gauss around 1805 and remains a reasonable
choice wherever the relationship is close to linear.

Three situations are worth knowing about, because in each of them the fit
succeeds and the result is not what it appears to be. The program reports all
three.

**One: the data cannot tell two columns apart.** If `icu` and `vent` are 1 on
exactly the same rows (nobody ever had one without the other), the data can
say the pair adds 8 days. It cannot say how to split that 8 between them. Every
split fits equally well, so there is no answer to find:

    $ ./linearr -t example/together.csv
    fit: 1 group, 5 rows, 1 term-slot pinned to 0, least df=3
    group,intercept,icu,vent
    A,6,8,0
    # pinned A: collinear vent

All 8 is assigned to the first column and the second is set to 0, with a note
recording that this happened. Without the note the 0 would be indistinguishable
from an estimate that ventilation has no effect. A column that never varies at
all, an intervention nobody in the group received, is the same case: the data
carries no information about it.

**Two: no degrees of freedom left.** Three rows and three unknowns will fit perfectly,
the way two points always define a line exactly. It would fit perfectly on any
numbers whatsoever, so a perfect fit tells you nothing:

    $ ./linearr -t example/three-rows.csv
    fit: 1 group, 3 rows, least df=0, worst cond=1.33
    warning: at least one group has no residual degrees of freedom; its line
    passes through every row by construction. Fit those groups on more rows.
    group,intercept,a,b
    A,1,3,6

*Degrees of freedom* is rows minus unknowns, and it is the amount of
disagreement the fit had to accommodate. At zero there is none, so the quality
of the fit is not evidence of anything. Six rows against three unknowns leaves
three rows' worth, and it is that residual disagreement which makes a small
error informative.

**Three: the arithmetic squares the data before solving.** Keeping the sums of
products rather than the rows is what bounds the memory, and it costs precision:
squaring roughly halves the significant digits available. With two columns that
differ in the sixth decimal, asked for `1 + 2*x1 + 3*x2`:

    $ ./linearr -t example/nearly-the-same.csv
    fit: 1 group, 40 rows, least df=37, worst resid SD=0, worst cond=5.34e+10
    warning: at least one group is ill-conditioned (cond=5.34e+10); the trailing
    digits of its coefficients are noise.
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

## The three files, and what a group is

**A group is one fitted line.** Rows sharing a group code are fitted together
and get their own coefficients; a different code gets different ones. Groups
exist so that one file and one pass produce one model per subset, instead of
splitting the file and running the program once per part. With a single group
this is plain least squares.

`example/routes.csv` is delivery time against distance and stops on three kinds
of route. The terms are the same everywhere; what each term is worth is not:

    $ ./linearr -t example/routes.csv
    fit: 3 groups, 18 rows, least df=3, worst resid SD=3.832e-07, worst cond=1.02
    group,intercept,km,stops
    city,5,3,2
    suburb,4,2,1.5
    highway,8,1,5

Three minutes per kilometre in the city, one on the highway; a stop costs two
minutes in the city and five on a highway route. Pooling the same eighteen rows
into one line gives an average of the three that describes none of them:

    $ ./linearr -t example/routes.csv -g '*'
    fit: 18 rows, R2=0.8159, resid SD=7.693, df=15, cond=1.02
    group,intercept,km,stops
    *,5.66666666667,2,2.83333333333

R2 of 0.82 still looks acceptable. The residual SD is what shows the cost:
predictions from the pooled line are typically 7.7 minutes out, against
essentially zero for the per-group fits above.

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

## Where the model is wrong

The coefficients describe the model; the residuals show where it does not fit,
which no single summary number can. Fitting a parabola with a straight line
produces summary statistics that give no sign of the problem:

    $ ./linearr -t curve.csv --residuals r.csv
    fit: 1 group, 9 rows, least df=7, worst resid SD=6.633

    $ cat r.csv
    group,observed,predicted,residual
    A,26,16.6666666667,9.33333333333
    A,19,16.6666666667,2.33333333333
    A,14,16.6666666667,-2.66666666667
    A,11,16.6666666667,-5.66666666667
    A,10,16.6666666667,-6.66666666667
    A,11,16.6666666667,-5.66666666667
    A,14,16.6666666667,-2.66666666667
    A,19,16.6666666667,2.33333333333
    A,26,16.6666666667,9.33333333333

The residuals are positive at both ends and negative in the middle. That is a
systematic pattern rather than scatter, and it indicates the model has the wrong
shape. R2, the residual SD and the conditioning figure all average over the
residuals, so none of them can show it.

It costs a second pass over the training file rather than a copy of it in
memory: the fit forgets each row as it reads it, so the rows have to be read
again to be subtracted from. Memory stays a function of the model.

## Two solvers

The fit accumulates `X'X` and solves it. That is what bounds the memory, and it
squares the condition number of the design, so a near-collinear or badly scaled
problem loses about twice the digits it needs to. `--qr` rotates each row into a
triangular factor instead, with Givens rotations, one row at a time. It squares
nothing. It is still streaming. Its factor is (p+1)(p+2) doubles against the normal
equations' p^2+2p, so it is slightly LARGER, not smaller; an earlier version of
this paragraph had that backwards.

On `example/nearly-the-same.csv`, where two columns differ in the sixth decimal
and the answer is `1 + 2*x1 + 3*x2`:

    normal equations   A,1.00000000001,2.00002262993,2.99997737008
    --qr               A,1,1.99999999974,3.00000000026

Five correct digits against ten. Both report `cond=`, and neither figure is a
condition number in the textbook sense: each is a ratio of pivots, on
differently scaled matrices, meant as an order-of-magnitude alarm. They are not
comparable to each other, which is why the summary names the solver. The square
root relationship holds for well-scaled designs and not otherwise: on the
example above the two are 5.34e10 and 2e6, and the square root of the first is
2.3e5.

`--qr` is not a strictly better solver. It does not centre the data, and the
first version of it deleted a well-identified column for being measured in a
small unit, which is the defect the default solver documents as fixed. Columns
are scaled before the rank test now, and the figures that depend on a dropped
column (`R2`, `resid SD`) are withheld rather than reported. The cost measured
at 300k rows by 20 terms is about 7 percent, not the larger penalty an earlier
version of this section implied.

The algorithm is Gentleman's 1974 row-wise updating QR, which R's `biglm` has
used for two decades. Nothing about the method is new here.

## When a straight line is the wrong shape

Every number in the fit summary is an average over the residuals, so none of
them can see structure IN the residuals, and that is where a wrong shape is
written. With `--residuals` the pass that writes them also checks two things.

    $ ./linearr -t example/curve.csv --residuals r.csv
    fit: 1 group, 13 rows, least df=11, worst resid SD=13.49
    warning: the residuals correlate with x squared (r=1.00). A straight line is
    probably the wrong shape in that term; look at the residual file, and
    consider adding its square as a column.

That file is a parabola. The fit succeeds, and the warning names the term to
look at rather than only reporting that something is wrong. The second check is
the size of the error against the size of the prediction: when it grows, the
residual SD is not a typical error at either end of the range.

Neither is a hypothesis test. Both are correlations against a threshold that is
the larger of 0.4 and 3/sqrt(n), which is roughly what unstructured residuals
produce by chance at that sample size, and neither reports anything when the
residuals are already negligible against the response's own spread. Both guards
came from a false positive: `example/routes.csv` is exactly linear, its
residuals are rounding error at 1e-7, and correlating rounding error with
anything over 18 rows found r = -0.44.

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
between a prediction and the observed value, in the response's own units. R2 is
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

Ten times the data, the same memory. Time scales with the number of rows.

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

    $ cat mine.csv
    group,minutes,km,stops
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
    group,intercept,km,stops
    A,5,2.5,1.5

    $ ./linearr -c model.csv --no-trim A km=10 stops=3
    A prediction=34.5000 trim=34.5

Those three columns were the whole schema; it ships as
`example/simple-train.csv` if you want to run it as it stands.

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

## Configuration

`system.properties`: `key = value`, `#` comments. Every key has a built-in
default, so the file may be absent.

| key | default | meaning |
| --- | --- | --- |
| `coef.file` | `conf/coefficients.csv` | the fitted model: `group,intercept,<one column per term>`. `-c` overrides it |
| `trim.file` | `conf/trim_additions.csv` | `group,trim_addition`. Set it empty, or pass `--no-trim`, for none |
| `predict.scale` | 4 | digits the prediction is rounded to |
| `trim.scale` | 1 | digits the trim point is rounded to |

With no trim table the trim point simply equals the prediction; it is not a
separate quantity that failed to load. A trim file *named* in the config and
unreadable is an error; the built-in default merely being absent is not.

**Coefficients are written to 12 significant digits**, so an exact 5 prints as
`5`. That is far below the residual standard deviation of any fit that produced
them, and it is significant digits rather than decimal places: four decimals
would write every coefficient below 5e-5 as `0.0000`. `predict.scale` governs the prediction, not the
model: rounding coefficients to four decimals would write any effect below 5e-5
as zero and publish a different model from the one that was fitted.

Rounding is half away from zero, not `printf`'s half to even, and it is part of
the answer rather than presentation: the trim point is built on the *rounded*
prediction, because the published figure is what the next step is entitled to
use.

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

That is a claim about implementation, not about tools. **This does not replace
Python, R, SAS or Matlab, and is not trying to.** Those are where a model should
be explored, chosen, tested and argued about, and they have decades of
statistics behind them that this has not. What this offers is a small, checkable
implementation of one method, useful in three places: as something to test an R
implementation against, since it agrees with `lm()` to the printed digit; on
embedded and small ARM targets where no interpreter is going to be installed;
and in cloud batch work, where the memory a process holds is what it costs.

Measured for the third case: **2,000,000 rows by 8 terms, a 46 MB file, fitted
in 0.42 s using 2.6 MB of memory**, and the same through a pipe rather than a
file. Nothing lands on disk, and the footprint does not depend on how much data
arrives.

### The limitations of that, stated

- **One core.** No threading, no vectorisation beyond what the compiler finds.
  A parallel implementation would beat it on a machine with cores to spare.
- **One stream.** It reads one input sequentially. There is no sharding, no
  distribution, no restart from a partial fit.
- **Memory is bounded by the MODEL, not by the data, and the model includes the
  groups.** Fitting every group in one pass holds one accumulator per group:
  about 2 KB per group at the default term ceiling. 500 groups is 1 MB; 400,000
  groups is 800 MB. Rows are free, groups are not.
- **`--residuals` needs a second pass**, so it needs a real file. From a pipe it
  refuses rather than half-work.
- **The parsing is the cost, not the arithmetic.** At 4.7 million rows a second
  the program is reading and converting text; a binary input format would be
  faster and does not exist here.
- **No weights, no sparse input, no categorical columns, no missing values.**
  A missing field is an error, not an imputation.
- **Commas only.** No quoting, no embedded separators, no other delimiter.


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
qr.c/.h           the same fit by Givens rotations, without squaring (--qr)
diag.c/.h         reads the residuals: curvature, and error that grows
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
example/          training files: routes.csv shows why groups exist, plus the
                  three the "three short pieces" section runs (all synthetic)
scripts/scale.sh  measures the memory claim at 200 terms and 500 groups
scripts/bench.sh  the same job in Java, Python, awk and R, answers checked first
java/             the second implementation; Regress.java mirrors regress.c
bench/            the other languages' versions, and the coefficient comparator
scripts/bench.sh  the same fit in C, Java and Python, checked against each other
scripts/hooks/    pre-push: the sanitizers, before anything reaches the remote
Makefile          the build
```

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

GNU GPL v2 or later; see `LICENSE`.

