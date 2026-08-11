# File formats, groups, and what the reader refuses

The three files this program reads and writes, what a group is, and every input
it will not accept. The README has the one-paragraph version.

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

## Rounding

**Coefficients are written to 12 significant digits**, so an exact 5 prints as
`5`. That is far below the residual standard deviation of any fit that produced
them, and it is significant digits rather than decimal places: four decimals
would write every coefficient below 5e-5 as `0.0000`. `--scale` governs the
prediction, not the model.

Rounding is half away from zero, not `printf`'s half to even, and it is part of
the answer rather than presentation: the trim point is built on the *rounded*
prediction, because the published figure is what the next step is entitled to
use.

## Fitting and scoring, in full

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
