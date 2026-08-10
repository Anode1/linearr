# Changelog

## 0.4.0

Two solvers returned wrong answers on inputs nobody had tested. Both are fixed,
and the tests that missed them are fixed too.

- **`--qr` was not least squares on a rank-deficient design** unless the dropped
  column happened to be the last one. With a constant column listed first it
  returned an intercept of 3.00755 where least squares gives 2.91, and the error
  did not shrink with more rows. The kept columns are re-triangularised before
  the solve now, which is what column pivoting achieves. The old test used the
  last position, the one case that was already exact.
- **The residual SD could be false.** `Cyy - b'Cxy` is a subtraction of nearly
  equal numbers, and on an exact quadratic fitted with a line it printed 0, then
  11.57, then 1256, where the truth was 0.3745 throughout. Below the floor of
  that subtraction it now prints a bound, `resid SD<1.799`, which contains the
  truth instead of contradicting it.
- **`-y NAME` / `--response NAME`** names the column being predicted. Without it
  column 2 is the response, as before, and a file written in another order fits
  the wrong column silently. The summary line now ends with the remedy when the
  column was not named.
- **`--stats FILE`** writes one row per group: rows, df, R2, residual SD, cond,
  pinned. The summary reports the worst of each over the whole file, which for
  many groups says nothing about which group.
- **The residual checks were wrong three ways.** Degrees of freedom were `n-3`,
  correct only for one term. There was no allowance for reading `2p+2` probes
  per group across every group, so 580 groups of 35 terms warned on correctly
  specified data every time; now none. The spread check correlated the
  residual's size against the prediction, which cannot see a symmetric change in
  spread; it is the score test `bptest` uses now.
- **The probes centre on the fit**, not on the first row's value. An outlier
  arriving first used to become the centre, and the same rows in a different
  order gave t=9999 one way and t=78.8 the other.
- **A quote anywhere in a field is refused**, not only at the start: `A"` and `A`
  used to become two groups, silently.
- **Row counters are 64-bit**, which they were not on the Windows target.
- Release binaries for linux-x86_64, linux-arm64, macos-arm64 and
  windows-x86_64 on a `v*` tag, each built, tested and checksummed.
- A long run reports progress once a minute, on both passes.
- `doc/INTERNALS.md` holds the layout, the style rules, the make targets and the
  Windows notes, split out of the README.

## 0.3.0

- **Licence changed from GPL v2 to BSD 2-Clause.** Done before any
  announcement, with every copyright line held by one author and no
  third-party code in the tree.
- QR solver (`--qr`), solving without squaring the condition number. Ten correct
  digits where the normal equations give five.
- Residual checks: whether a term's relationship is curved, whether something
  outside the model drives the response, and whether the error grows with the
  prediction. Each names what it found.
- `--residuals` writes one row per training row; residual SD is reported.
- `-t` fits every group in one pass; `-t -` reads training data from a pipe.
- Windows: WSL runs this unmodified, and the section says how.
- Fixes from three reviews (statistics, a student, a beginner), including a QR
  that deleted terms for their units, a curvature check that was invisible on
  any variable with an origin, and a training file with no group column that
  fitted garbage and exited 0.

## 0.1.0 (unreleased)

First public version.

- Ordinary least squares from a CSV, with the terms named by the file's header
  rather than compiled in.
- Centered, equilibrated normal equations: the rank test is scale-invariant and
  R2 survives an offset response. Conditioning is reported, because R2 cannot
  see an ill-conditioned design.
- Terms the data cannot identify are pinned to 0 and marked in the output, with
  constant and collinear reported as the different verdicts they are.
- Fits every group in one pass; scores one case or a stream.
- Coefficients written at full precision; predictions rounded on purpose.
- Bounded memory: peak footprint is a function of the model, never of the rows.
