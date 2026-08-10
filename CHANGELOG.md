# Changelog

## 0.3.0

- **Licence changed from GPL v2 to BSD 2-Clause.** The project is a reference
  implementation meant to be copied into other programs, and the GPL forbade
  exactly that for the embedded and commercial cases it is written for. Done
  before any announcement, with every copyright line held by one author and no
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
