# Where this came from, and what it will not do

The longer version of the README's Origin section.

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

#