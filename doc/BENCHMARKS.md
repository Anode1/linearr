# The same job in other languages

Every timing here, and the extrapolations built on them, came from one machine.
Read the ratios between the rows, which hold elsewhere, rather than the seconds,
which do not. `sh scripts/bench.sh` and `sh scripts/scale.sh` produce your own.

Read the file, fit a line per group, write the table. `scripts/bench.sh` checks
each one's coefficients against linearr's before timing it; an unchecked speed
number may be timing a different answer. All agree to the printed digit: one
answer, different prices.

    $ sh scripts/bench.sh 8 50 500000        # 8 terms, 50 groups, 500k rows

    implementation   shape        time     peak RSS   check
    linearr (C)      streaming    0.11s    2560 KB    agrees to 0
    Java             streaming    0.36s    106896 KB  agrees to 0
    Python           streaming    2.76s    10240 KB   agrees to 0
    awk              streaming    11.75s   5760 KB    agrees to 0
    Python           frame        3.30s    315904 KB  agrees to 0
    R                frame        1.32s    128780 KB  agrees to 1e-12

An implementation that is not installed, or that fails, prints why and the
others still run.

All but the last read the file one row at a time, which each of these languages
permits. Writing the C as a stream and the Python with pandas would compare two
styles rather than two languages, so the materialising row is also Python: the
same language, machine and algorithm, with one variable changed. Over a tenfold
increase in rows the streaming figures are unchanged and that row rises from
41 MB to 316 MB.

The JVM's number is mostly the JVM. Capping its heap separates the runtime's
appetite from what the algorithm needs: 500,000 rows fit in a 16 MB heap at
the same speed:

    -Xmx16m   0.33s  64388 KB
    -Xmx64m   0.34s  95028 KB

`bench/fit.R` is the ecosystem case, and it says so in its own header: `read.csv`
materialises the frame because that is R's idiom, so its memory figure is the
cost of the idiom rather than a statement about the language. A streaming R
using `readLines` and a manual accumulator would sit with the others.

### The Java baseline

`java/` is not a strawman written to lose. It fits the same model in the style
this project's C came from: a `BufferedReader` and a `readLine` loop,
`Hashtable`, `Vector`, `StringBuffer`, and one reused record rather than one
object per row. `Regress.java` follows `regress.c` closely enough to be read
beside it, with the same centered accumulation, the same equilibrated rank test
and the intercept recovered from the means.

    cd java && ant jar          # or: javac -d classes *.java
    java -cp classes Linearr ../example/train.csv

`make java` fits every example with both and diffs the output, so "the two
produce identical coefficient files" is a gate rather than a claim. Reusing the
record instead of allocating one per row is why its memory is flat in the rows:
`String.split()` in that loop would allocate an array and a string per field,
five million short-lived objects over 500,000 rows, and the heap grown to hold
them gets read as "Java needs 400 MB for this".

### Ten times the rows

The same command at 5,000,000 rows, to separate what scales with the data from
what does not:

    $ sh scripts/bench.sh 8 50 5000000

    implementation   shape        time     peak RSS   check
    linearr (C)      streaming    1.31s    2432 KB    agrees to 0
    Java             streaming    1.98s    421088 KB  agrees to 0
    Python           streaming    28.30s   10368 KB   agrees to 0
    awk              streaming    120.06s  5632 KB    agrees to 0
    Python           frame        35.36s   3068800 KB agrees to 0
    R                frame        12.11s   959236 KB  agrees to 1.7e-11

Time is linear in the rows for all six. Memory is not:

| implementation | time, 500k to 5M | peak RSS, 500k to 5M |
| --- | --- | --- |
| linearr (C) | 0.11s to 1.31s | 2.5 MB to 2.4 MB |
| Python, streaming | 2.76s to 28.30s | 10.0 MB to 10.1 MB |
| awk | 11.75s to 120.06s | 5.6 MB to 5.5 MB |
| Java, streaming | 0.36s to 1.98s | 104 MB to 411 MB |
| R, frame | 1.32s to 12.11s | 126 MB to 937 MB |
| Python, frame | 3.30s to 35.36s | 308 MB to 2.9 GB |

The three streaming rows are flat to within measurement noise over a tenfold
increase. The two frame rows grow with the file: R holds about 196 bytes per
row and the materialising Python about 629. The Java row grows because the JVM takes
more heap when a machine has it, not because the algorithm needs it; capped at
`-Xmx16m` the same 5,000,000 rows fit in 65 MB and finish in 1.83s.

Extrapolating the memory, on a machine with 64 GB to give:

| implementation | rows before it runs out of memory |
| --- | --- |
| linearr, Python streaming, awk | no limit from memory; time is the only cost |
| R, `read.csv` + `lm()` | about 340 million |
| Python, materialised in memory | about 105 million |

Those two are the shape of the comparison. The streaming implementations
get slower; the frame ones stop. At a billion rows R would need about 200 GB and
the materialising Python about 600 GB, while linearr holds 2.4 MB and takes
about four minutes.

**That row is not pandas.** `bench/fit-frame.py` is hand-written Python holding
the file as a list of tuples, and it is the control for STYLE: same language,
same machine, same algorithm as `bench/fit.py`, with one variable changed.
Real pandas is faster and lighter than it and is not measured here. A reviewer
did measure it and reported roughly 3.5s and 1.0 GB where this row says 34.6s
and 3.1 GB, which would move the ceiling from 105 to about 320 million rows.
Read the row as what materialising costs in principle, not as a figure for
pandas.

The ceiling is not a criticism of R or of a data frame, which materialise
because that is what an exploratory session wants: the whole dataset addressable
while you decide what to ask. When the question is settled and the file is the
size of a disk, the trade goes the other way.

Every timing in this section, and in the extrapolation above, was measured on
one machine: an 11th-generation Intel Core i7-1165G7 at 2.80 GHz, 8 threads,
62 GB, Ubuntu 24.04, gcc 13.3, using one core. A different machine will give
different figures, and a server core will beat a laptop one. Read the ratios
between the rows, which hold, rather than the seconds, which do not. `sh
scripts/bench.sh` and `sh scripts/scale.sh` produce your own.

**These timings are not gated.** Every other transcript in this file is run and
diffed by `make readme`; this one cannot be, because a wall-clock figure differs
between machines and between runs. Read the ratios, not the digits, and run
`sh scripts/bench.sh` yourself if the ratios matter to you.

## Scale, in full

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
