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
