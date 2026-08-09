# AGENTS.md: how to develop linearr (for humans and AI agents)

linearr fits a linear model from a CSV and scores rows against it, in C99. This
is the operating manual for working on it. Read it, then `README.md`.

## The contract (read first)

- **`README.md`**: what the program promises: the two directions (fit and
  score), the data-driven schema, the configuration keys, the memory bound, and
  the Style section. Behaviour that contradicts it is a defect in one of them.
- **`los.h`**: the model: the schema, the tables, the arithmetic. The header
  comments are the specification the `.c` implements.
- **`regress.h`**: what the fitter guarantees, including what it does with a
  term the data cannot identify. Read it before touching `regress.c`; the
  pinning rule is a decision, not an accident.
- **`process.h`**: the slot main.c calls. Its comments define the output
  format, and `tests.c` asserts that format literally.

Do not change behaviour without changing these first.

## Build and test

    make          # build ./linearr
    make check    # ut + cliut: the commit gate
    make ut       # the unit tests: the fast inner loop
    make cliut    # black-box: the built binary through a shell and a pty
    make ut-asan  # the tests under AddressSanitizer
    make ut-ubsan # the tests under UndefinedBehaviorSanitizer
    make pedantic # -pedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes ...
    make hooks    # install the pre-push hook (runs both sanitizers)
    make clean

Run the tests from the project root: they read `conf/` and `example/` by
relative path.

Before tagging, run `make ut-asan` and `make ut-ubsan`. You do not have to
remember: `.github/workflows/sanitizers.yml` runs both on Linux and macOS on
every push, and `make hooks` installs a pre-push hook that runs them locally
first (bypass once with `git push --no-verify`). Keep them out of the default
build: they are 2-3x slower and not universally available, so `make` and
`make ut` stay portable.

## The invariants this program is built on

Both are asserted by tests, and both are easy to destroy with a well-meaning
refactor:

- **Memory is a function of the model, never of the data.** `regress_add` folds
  one observation into the centered co-moments and forgets it; the trainers
  stream the file one line at a time; a case is a stack struct. Nothing in the
  footprint formula is a data size. Collecting rows into an array to "make it
  simpler" throws this away, and it is the reason the program exists. `sh
  scripts/scale.sh` is the check: fit the same model over 10x the rows and peak
  RSS must not move. Three things allocate, all bounded by the model or the
  config: the coefficient table (`los.c`), the config table (`params.c`), and
  one accumulator per group during a fit-everything pass (`process.c`). Adding a
  fourth needs an argument. Note also that the fitter's matrices are STATIC, not
  automatic: as locals they needed 1.18 MB of contiguous stack and killed the
  program under `ulimit -s 1024` with no diagnostic.
- **The terms come from the file, not from the source.** `los_schema_set` is the
  only place a column list is established, and both loaders feed it from a
  header line. Hardcoding a count or a name anywhere else (including in a test
  helper) silently re-couples the program to one dataset.

## A claim is not only prose

**Makefile comments, header comments, source comments and the usage text the
binary prints are CLAIMS, and they go stale exactly like a README.** When you
change behaviour, they move with the code; when you audit, they are in scope.

This is not a style note. Two defects in this repository's short history were
found by reading a claim against the code rather than by running anything:

- The `Makefile` inherited a default goal of `modeclean`, so a bare `make`
  (the first command the README documents) ran `rm -f` on the objects and the
  binary, built nothing, and exited 0. It looked exactly like an up-to-date
  no-op. `all:` now sits above `modeclean` for that reason.
- `system.properties` said a trim table could be turned off; the loader made it
  mandatory, which meant a coefficient file produced by `-t` could not be scored
  against at all. The loader was split (`los_load` / `los_load_trims`) so the
  comment became true.
- And then the same line was wrong a second time, for a year's worth of commits
  in one afternoon: it said "comment this out and the trim point is just the
  prediction", when an absent key means the built-in default, so the table loads
  and the trim is applied. The README got the correction; the config file that
  originally made the claim did not. Three independent reviews missed it.
  **When you correct a claim, grep for the other places that make it**: a
  fixed sentence in one file is not a fixed claim.

## Nullable returns, and the segfault this project already had

`los_var_name`, `process_term_name`, `params_get`, `hash_get`, `los_model_get`
and `resolve_program_dir` are documented as possibly returning NULL, and that is
the right design: "out of range" and "absent" are real answers. The hazard is
what a caller does with one.

This suite crashed once, with a SEGV inside `strcmp`, because a test asserted
`strcmp(process_term_name(16), "icu_indicator") == 0` while a state bug had left
the schema empty. The assertion was correct to fail; it was written so that
failing meant dying. Two things came out of it, and both are rules now:

- **In tests, never pass a nullable return straight to a string function.** Use
  `streq()`. A wrong return value must produce a FAIL naming the check, not a
  core dump that tells you nothing about the other 144 tests.
- **In the sources, a guard must be visible above the use.** `main.c`'s
  `--terms` loop is bounded by `process_nterms()` so the index is always in
  range, and the line after it is written `n > 0 ? process_term_name(0) : "TERM"`
  for the same reason. `resolve.c` checks `if (dir)`. Anywhere that is not
  immediately readable off the surrounding lines, it is a defect.

The state bug underneath was the real lesson: `process.c` trusted a private
`tables_loaded` flag while the state it described lived in `los.c`, where
`los_free` could clear it and `process_train` could replace it with a training
file's schema. A flag that another module can invalidate is not a fact.
`ensure_tables` now asks `los_nvars() > 0` as well, so it heals instead of
lying, and `process_train` clears the flag because it repurposes the schema.

Note what did work: `make ut-asan` located it at a file and line on the first
run. The sanitizers are not a formality.

## What 145 green tests did not catch

Three independent reviewers (statistics, safety, adversarial) were turned on
this project after it looked finished. Memory safety survived: ~5,000 fuzz
rounds under ASan and UBSan produced no crash and no sanitizer report, which is
the part the discipline in this file was actually built to protect. Everything
they did find was a **confidently printed wrong number with exit 0**, and the
reason the suite could not see any of it is worth keeping:

`tests.c` only ever exercised the fitter on well-behaved data: values around
1, a response with no offset, columns of comparable scale, well-conditioned
designs. Under those conditions every defect below is invisible:

- the rank tolerance was absolute, so a column measured in dollars deleted an
  indicator column for being small. Units decided which terms existed.
- `R2` was computed from uncentered sums, so an offset response produced
  catastrophic cancellation, and the `sse < 0` clamp turned the wreckage into
  a printed `R2=1.0000`.
- coefficients were written at `%.4f`, so the published table was a different
  model from the fitted one whenever an effect was below 5e-5.
- `nan` and `inf` parsed fine, propagated into every coefficient, and scored.

**So: a test whose inputs are all the same order of magnitude is not a test of
numerics.** When you add one, ask what it would look like if the data were
scaled by 1e6, offset by 1e8, or nearly collinear, and add that case too.
There are now tests named `units:`, `offset:` and `conditioning:` for exactly
this, and they should grow rather than be trimmed.

## Marking work done

Task #16 of this project's own review was marked completed with two of its five
items done. Nothing failed; the remaining three were simply not looked at again,
because "MISRA tail" had a tick beside it. Finish the list, or leave the item
open with the remainder written into it; a partly-done task recorded as done
is worse than one recorded as pending, because it stops being looked at.

## The development loop (test-driven)

Tests are the objective gate. Never trust output you have not verified.

1. **Lock the contract.** If the change needs new behaviour, update `README.md`
   and the relevant header first, so there is one agreed spec.
2. **Implement** against it, in the README's Style idiom: one concept per file,
   modules return codes, only the CLI `die()`s.
3. **Test.** Add or extend `tests.c`: linear, inline, ONE comment per test
   saying what it checks. Cover the new behaviour and its edges.
4. **Verify.** `make check` green, `make pedantic` warning-free, then the two
   sanitizers.

**Some behaviour has no unit test, by construction.** `make ut` runs with a pipe
on stdin, never a terminal; it sees a return value, never an exit code; it calls
`process()`, never the binary. A bare `./linearr` sat reading stdin and looked
hung, printing its usage only after a Ctrl-C, while all 118 unit tests were
green, because none of them could have been the one to notice. That class of
behaviour goes in `tests/cli.sh`. When you fix something a user hit and no test
failed, the first question is which gate could not have caught it.

Red -> green -> refactor. Every change keeps the whole suite green.

## The numbers in the tests are load-bearing

`example/train.csv` was generated from `conf/coefficients.csv`, so the fit must
return those coefficients to the last printed digit; `example/simple-train.csv`
is `5 + 2.5*km + 1.5*stops` and must come back as exactly that. When one of
those assertions fails, the fitter is wrong; do not adjust the expectation to
match the output. If you regenerate the example data, regenerate the expected
values with it and say so in the commit.

## The data is synthetic, and stays that way

Everything in `conf/` and `example/` is generated. No real data belongs in this
repository: not as a fixture, not as an example, not "temporarily". A user
supplies their own tables through `coef.file` and `trim.file`.
