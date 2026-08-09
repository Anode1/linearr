# Contributing

Read `AGENTS.md` first: it is the operating manual, and its rules are the
result of specific defects rather than taste.

## Before you send anything

    make check        # unit tests + CLI black-box tests
    make pedantic     # must be warning-free
    make ut-asan
    make ut-ubsan

`make hooks` makes the sanitizers run before every push.

## What a change looks like here

- **A feature ships with a test.** `tests.c` for anything reachable from the
  API; `tests/cli.sh` for anything that only exists when the binary is run:
  exit codes, which stream a message went to, a terminal on stdin.
- **A comment is a claim.** Header comments, the Makefile and the usage text go
  stale exactly like a README. When you change behaviour they move with it.
- **Numerics need hostile inputs.** A test whose values are all around 1, with
  no offset and comparable column scales, is not a test of numerics. Every
  serious defect this project has had was invisible to exactly that kind of
  test. Add the scaled, offset and near-collinear cases too.
- **Before adding a `malloc`,** check it against the three the project already
  sanctions (`README.md`, Style). A fourth needs an argument.

## Reporting a bug

The exact command, the input that reproduces it, what you expected, and what
happened. A reproduction that fits in a shell snippet is worth more than a
description of the problem.
