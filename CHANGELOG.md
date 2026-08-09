# Changelog

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
