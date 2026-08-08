# AGENTS.md -- how to develop linearr (for humans and AI agents)

linearr fits a linear model from a CSV and scores rows against it, in C99. This
is the operating manual for working on it. Read it, then `README.md`.

## The contract (read first)

- **`README.md`** -- what the program promises: the two directions (fit and
  score), the data-driven schema, the configuration keys, the memory bound, and
  the Style section. Behaviour that contradicts it is a defect in one of them.
- **`los.h`** -- the model: the schema, the tables, the arithmetic. The header
  comments are the specification the `.c` implements.
- **`regress.h`** -- what the fitter guarantees, including what it does with a
  term the data cannot identify. Read it before touching `regress.c`; the
  pinning rule is a decision, not an accident.
- **`process.h`** -- the slot main.c calls. Its comments define the output
  format, and `tests.c` asserts that format literally.

Do not change behaviour without changing these first.

## Build and test

    make          # build ./linearr
    make ut       # the unit tests -- the fast inner loop, run before every commit
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
build -- they are 2-3x slower and not universally available, so `make` and
`make ut` stay portable.

## The invariants this program is built on

Both are asserted by tests, and both are easy to destroy with a well-meaning
refactor:

- **Memory is a function of the model, never of the data.** `regress_add` folds
  one observation into the cross-product matrix and forgets it; `process_train`
  streams the file one line at a time; a case is a stack struct. Peak footprint
  is `16*(REGRESS_MAX_VARS+1)^2` bytes plus the row buffers, and nothing in that
  formula is a data size. Collecting rows into an array to "make it simpler"
  throws this away, and it is the reason the program exists. `sh
  scripts/scale.sh` is the check: fit the same model over 10x the rows and the
  peak RSS must not move. Before adding any `malloc`, check it against the one
  the core sanctions: the coefficient table in `los.c`, bounded by the number of
  groups and freed on every path.
- **The terms come from the file, not from the source.** `los_schema_set` is the
  only place a column list is established, and both loaders feed it from a
  header line. Hardcoding a count or a name anywhere else -- including in a test
  helper -- silently re-couples the program to one dataset.

## A claim is not only prose

**Makefile comments, header comments, source comments and the usage text the
binary prints are CLAIMS, and they go stale exactly like a README.** When you
change behaviour, they move with the code; when you audit, they are in scope.

This is not a style note. Two defects in this repository's short history were
found by reading a claim against the code rather than by running anything:

- The `Makefile` inherited a default goal of `modeclean`, so a bare `make` --
  the first command the README documents -- ran `rm -f` on the objects and the
  binary, built nothing, and exited 0. It looked exactly like an up-to-date
  no-op. `all:` now sits above `modeclean` for that reason.
- `system.properties` said a trim table could be turned off; the loader made it
  mandatory, which meant a coefficient file produced by `-t` could not be scored
  against at all. The loader was split (`los_load` / `los_load_trims`) so the
  comment became true.

## The development loop (test-driven)

Tests are the objective gate. Never trust output you have not verified.

1. **Lock the contract.** If the change needs new behaviour, update `README.md`
   and the relevant header first, so there is one agreed spec.
2. **Implement** against it, in the README's Style idiom: one concept per file,
   modules return codes, only the CLI `die()`s.
3. **Test.** Add or extend `tests.c` -- linear, inline, ONE comment per test
   saying what it checks. Cover the new behaviour and its edges.
4. **Verify.** `make ut` green, `make pedantic` warning-free, then the two
   sanitizers.

Red -> green -> refactor. Every change keeps the whole suite green.

## Numbers in tests are not decoration

`example/train.csv` was generated from `conf/coefficients.csv`, so the fit must
return those coefficients to the last printed digit; `example/simple-train.csv`
is `5 + 2.5*km + 1.5*stops` and must come back as exactly that. When one of
those assertions fails, the fitter is wrong -- do not adjust the expectation to
match the output. If you regenerate the example data, regenerate the expected
values with it and say so in the commit.

## The data is synthetic, and stays that way

Everything in `conf/` and `example/` is generated. No real data belongs in this
repository -- not as a fixture, not as an example, not "temporarily". A user
supplies their own tables through `coef.file` and `trim.file`.
