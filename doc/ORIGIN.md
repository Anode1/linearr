# Origin and limits

The longer version of the README's Origin section.

Releasing a C implementation of the 2011 predictor as open source was the
intention at the time, and there was never time for it. This is that
implementation. The coefficients shipped here are synthetic, and real tables
belong to whoever owns the data and produced the coefficients.

The problem itself has not changed since 2011: a table of coefficients, a
stream of rows to apply them to, and often a machine on which installing a
scientific stack is inconvenient or not permitted.

The view behind that porting work, and behind this program, is that
computation belongs as close to the processor and to the memory it touches as
the problem allows; distance from it costs time, energy and hardware that a
straight line does not need.

The second view is about shape rather than language: data of any size should be
read as a stream, so that its size stops being a design question, at the cost of
one pass and one core and a second pass for anything needing a second look.

Both are claims about implementation, not about tools. The tools this does not
replace have decades of statistics behind them that this has not; the places
this goes that they cannot are three: as something to check an implementation
against, since it reproduces NIST's Norris and Longley to eleven digits -- not
the whole NIST suite, and Wampler1's coefficients only to eight (see
[Checked against answers somebody else
certified](NUMERICS.md#checked-against-answers-somebody-else-certified)); on embedded and
small ARM targets where no interpreter is going to be installed; and in cloud
batch work, where the memory a process holds is what it costs.

And if you need some other model ported from Python, R, SAS or Matlab into C
or plain Java, the author will only be glad to help:
[open an issue](https://github.com/Anode1/linearr/issues).

#

## The limitations of that, stated

One core and one stream: no threading, no sharding, no restart from a partial
fit, and at 4.1 million rows a second the cost is reading text rather than the
arithmetic, so a second core would buy more than a faster solver.
`--residuals` reads the file a second time and needs a real file rather than a
pipe.

What it costs in memory is in [Scale](../README.md#scale), and what it does not do at all
is in [Where this is the right tool](../README.md#where-this-is-the-right-tool-and-where-it-is-not)
and [What it will not read](FORMATS.md#what-it-will-not-read).

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
with them were production values and are not in this repository; see [what each example
file is for](FORMATS.md#the-example-data-and-what-each-file-is-for). Anyone reimplementing this fits their own.
